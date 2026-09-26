#include "radarcfg.h"
#include "log.h"
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

namespace radarcfg {
namespace {

// The defaults are the values the radar shipped with, written out here rather
// than left in the code they used to live in. A player who deletes their ini
// gets exactly the panel they had before the file existed.
// In the order Settings declares them - this is positional initialisation, so
// a field added here in the wrong place silently assigns itself to whatever
// sits at that position instead. The compiler catches it only when the two
// types differ, which is luck rather than safety: adding metricDistance one
// slot early landed a bool on showMapIcons and was caught by a narrowing
// warning that a float-next-to-float mistake would never have produced.
const Settings kDefaults = {
    0xFFFF3EA8u,  // gpsMarker - hot pink
    0xFF000000u,  // gpsMarkerOutline - black, as the game borders its own
    0xFFFF3EA8u,  // gpsRibbon - the same pink, so line and place read as one
    0xFFFFE000u,  // missionMarker - the yellow the gamemode already uses for
                  // a mission map icon, so the panel and the pause map agree
    0xFF241C00u,  // missionMarkerOutline - near-black amber
    0xFFFFE000u,  // missionRibbon
    0xFFFFFFFFu,  // ribbonOutline - white, separates either ribbon from tarmac
    0.60f,        // ribbonWidth
    0.72f,        // ribbonOutlineWidth
    3.5f,         // markerSize
    2.0f,         // markerOutlineWidth - the game's own border weight
    0u,           // captureMovingMs
    250u,         // captureSettledMs
    0xc4u,        // navigationGrade
    true,         // showMapIcons - the game's own legend, on the panel
    true,         // fadeMapIcons - dim with distance, mission marker excepted
    45u,          // iconFadeMinAlpha - faint at the rim, but still readable
    0.0f,         // iconEdgeInset - centred on the rim, as N and the dot are
    0.55f,        // iconMinScale - small at range, full size up close
    1200.0f,      // iconWrapRange - how far out an icon still rides the rim
    false,        // metricDistance - miles and feet unless asked otherwise
    false,        // showOnFoot - a driving display unless asked otherwise
    106.0f,       // panelWidth  \ the GTA VI minimap proportions the panel
    82.0f,        // panelHeight / has always used, tucked into the corner
    8.0f,         // panelLeft
    9.0f,         // panelBottomMargin
    512u,         // captureResolution
};

Settings g_settings = kDefaults;
std::atomic<uint32_t> g_generation{1};

// Current() is called from the render thread and from the route worker, and
// either may be the one that notices the file has changed. The mutex covers
// the check and the reload, so two threads cannot parse and publish at once -
// which is the only way this could produce something that is not simply the
// old or the new value of one setting.
std::mutex g_reloadLock;

// How often the file's timestamp is worth looking at. A stat per frame is not
// expensive, but it is a syscall on the render thread for a file that changes
// when somebody has Notepad open, which is rarely.
constexpr DWORD kRecheckMs = 1000;
DWORD g_lastCheck{};
FILETIME g_stamp{};
bool g_haveStamp{};

std::string IniPath() {
  char path[MAX_PATH]{};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (char *slash = strrchr(path, '\\')) slash[1] = 0;
  strncat_s(path, "valkyrie-radar.ini", _TRUNCATE);
  return path;
}

// GetPrivateProfileString, but only overwriting `out` when the key is there
// and the value parses. Anything else leaves the default standing and says so.
//
// Colours are written the way a player writes a colour - six or eight hex
// digits, with or without a leading # or 0x. Six digits means fully opaque,
// which is what somebody typing FF00FF expects rather than a marker that is
// invisible because they left the alpha off.
bool ReadColour(const char *section, const char *key, const std::string &file,
                uint32_t &out) {
  char raw[64]{};
  if (!GetPrivateProfileStringA(section, key, "", raw, sizeof raw, file.c_str()))
    return false;
  const char *p = raw;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '#') ++p;
  else if ((p[0] == '0') && (p[1] == 'x' || p[1] == 'X')) p += 2;
  const char *digits = p;
  size_t n = 0;
  while (isxdigit(static_cast<unsigned char>(p[n]))) ++n;
  const char *tail = p + n;
  while (*tail == ' ' || *tail == '\t' || *tail == '\r') ++tail;
  if (*tail || (n != 6 && n != 8)) {
    logfile::Line("radar config: %s=%s is not a colour; six or eight hex "
                  "digits (AARRGGBB), keeping the default",
                  key, raw);
    return false;
  }
  const uint32_t value = uint32_t(strtoul(std::string(digits, n).c_str(), nullptr, 16));
  out = (n == 6) ? (0xFF000000u | value) : value;
  return true;
}

bool ReadFloat(const char *section, const char *key, const std::string &file,
               float low, float high, float &out) {
  char raw[64]{};
  if (!GetPrivateProfileStringA(section, key, "", raw, sizeof raw, file.c_str()))
    return false;
  char *end = nullptr;
  const float value = strtof(raw, &end);
  if (end == raw) {
    logfile::Line("radar config: %s=%s is not a number, keeping the default",
                  key, raw);
    return false;
  }
  if (value < low || value > high) {
    logfile::Line("radar config: %s=%s is outside %g to %g, keeping the default",
                  key, raw, low, high);
    return false;
  }
  out = value;
  return true;
}

bool ReadUint(const char *section, const char *key, const std::string &file,
              uint32_t low, uint32_t high, uint32_t &out) {
  float value = float(out);
  if (!ReadFloat(section, key, file, float(low), float(high), value))
    return false;
  out = uint32_t(value + .5f);
  return true;
}

bool ReadBool(const char *section, const char *key, const std::string &file,
              bool &out) {
  char raw[64]{};
  if (!GetPrivateProfileStringA(section, key, "", raw, sizeof raw, file.c_str()))
    return false;
  const char *p = raw;
  while (*p == ' ' || *p == '	') ++p;
  // Whatever a player is likely to write for yes.
  if (!_strnicmp(p, "1", 1) || !_strnicmp(p, "true", 4) ||
      !_strnicmp(p, "yes", 3) || !_strnicmp(p, "on", 2)) {
    out = true;
    return true;
  }
  if (!_strnicmp(p, "0", 1) || !_strnicmp(p, "false", 5) ||
      !_strnicmp(p, "no", 2) || !_strnicmp(p, "off", 3)) {
    out = false;
    return true;
  }
  logfile::Line("radar config: %s=%s is not yes or no, keeping the default",
                key, raw);
  return false;
}

// The distance units, written the way somebody would write them rather than as
// a yes/no against one of the two.
bool ReadUnits(const char *section, const char *key, const std::string &file,
               bool &metric) {
  char raw[64]{};
  if (!GetPrivateProfileStringA(section, key, "", raw, sizeof raw, file.c_str()))
    return false;
  const char *p = raw;
  while (*p == ' ' || *p == '	') ++p;
  if (!_strnicmp(p, "km", 2) || !_strnicmp(p, "kilomet", 7) ||
      !_strnicmp(p, "metric", 6)) {
    metric = true;
    return true;
  }
  if (!_strnicmp(p, "mi", 2) || !_strnicmp(p, "imperial", 8)) {
    metric = false;
    return true;
  }
  logfile::Line("radar config: %s=%s is not miles or km, keeping the default",
                key, raw);
  return false;
}

// Read the whole file into a fresh Settings, starting from the defaults.
Settings Parse(const std::string &file) {
  Settings s = kDefaults;
  ReadColour("Colours", "GpsMarker", file, s.gpsMarker);
  ReadColour("Colours", "GpsMarkerOutline", file, s.gpsMarkerOutline);
  ReadColour("Colours", "GpsRibbon", file, s.gpsRibbon);
  ReadColour("Colours", "MissionMarker", file, s.missionMarker);
  ReadColour("Colours", "MissionMarkerOutline", file, s.missionMarkerOutline);
  ReadColour("Colours", "MissionRibbon", file, s.missionRibbon);
  ReadColour("Colours", "RibbonOutline", file, s.ribbonOutline);

  ReadFloat("Radar", "RibbonWidth", file, .1f, 4.f, s.ribbonWidth);
  ReadFloat("Radar", "RibbonOutlineWidth", file, .1f, 4.f, s.ribbonOutlineWidth);
  // An outline narrower than the fill it is behind does not show at all, which
  // reads as the setting having done nothing. Keep it the wider of the two.
  if (s.ribbonOutlineWidth <= s.ribbonWidth)
    s.ribbonOutlineWidth = s.ribbonWidth * 1.2f;
  ReadFloat("Radar", "MarkerSize", file, 1.f, 20.f, s.markerSize);
  ReadFloat("Radar", "MarkerOutlineWidth", file, 0.f, 10.f, s.markerOutlineWidth);
  ReadUint("Radar", "CaptureMovingMs", file, 0u, 500u, s.captureMovingMs);
  ReadUint("Radar", "CaptureSettledMs", file, 0u, 2000u, s.captureSettledMs);
  ReadUint("Radar", "NavigationGrade", file, 0x40u, 0xFFu, s.navigationGrade);
  ReadUnits("Radar", "Units", file, s.metricDistance);
  ReadBool("Radar", "ShowMapIcons", file, s.showMapIcons);
  ReadBool("Radar", "FadeMapIcons", file, s.fadeMapIcons);
  ReadUint("Radar", "IconFadeMinAlpha", file, 0u, 255u, s.iconFadeMinAlpha);
  ReadFloat("Radar", "IconEdgeInset", file, 0.f, 40.f, s.iconEdgeInset);
  ReadFloat("Radar", "IconMinScale", file, .1f, 1.f, s.iconMinScale);
  ReadFloat("Radar", "IconWrapRange", file, 0.f, 5000.f, s.iconWrapRange);
  ReadBool("Radar", "ShowOnFoot", file, s.showOnFoot);

  // The panel. Bounded to what can actually be a panel: GTA's HUD space is
  // 640x448, and a box wider than half of it, or one whose left edge puts it
  // off the screen, is not a minimap any more.
  ReadFloat("Panel", "Width", file, 40.f, 320.f, s.panelWidth);
  ReadFloat("Panel", "Height", file, 30.f, 224.f, s.panelHeight);
  ReadFloat("Panel", "Left", file, 0.f, 400.f, s.panelLeft);
  ReadFloat("Panel", "BottomMargin", file, 0.f, 200.f, s.panelBottomMargin);
  if (s.panelLeft + s.panelWidth > 640.f) s.panelLeft = 640.f - s.panelWidth;
  if (s.panelBottomMargin + s.panelHeight > 448.f)
    s.panelBottomMargin = 448.f - s.panelHeight;
  ReadUint("Panel", "CaptureResolution", file, 128u, 2048u, s.captureResolution);
  return s;
}

bool Differ(const Settings &a, const Settings &b) {
  return memcmp(&a, &b, sizeof(Settings)) != 0;
}

// Whether the file has been written since it was last read. A file that is not
// there at all counts as a change exactly once - when it is deleted - so the
// defaults come back rather than the last thing it said before it went.
bool Changed() {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  const std::string path = IniPath();
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) {
    if (!g_haveStamp) return false;
    g_haveStamp = false;
    return true;
  }
  if (g_haveStamp && CompareFileTime(&info.ftLastWriteTime, &g_stamp) == 0)
    return false;
  g_stamp = info.ftLastWriteTime;
  g_haveStamp = true;
  return true;
}

}  // namespace

void Load() {
  const std::string path = IniPath();
  const bool present = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
  const Settings fresh = present ? Parse(path) : kDefaults;
  if (!Differ(fresh, g_settings)) return;
  g_settings = fresh;
  g_generation.fetch_add(1);
  logfile::Line("radar config: %s; gps marker %08X ribbon %08X, mission marker "
                "%08X ribbon %08X; panel %.0fx%.0f at %.0f,%.0f, capture %u, "
                "on foot %s",
                present ? "valkyrie-radar.ini read" : "no ini, defaults in use",
                fresh.gpsMarker, fresh.gpsRibbon, fresh.missionMarker,
                fresh.missionRibbon, fresh.panelWidth, fresh.panelHeight,
                fresh.panelLeft, fresh.panelBottomMargin,
                fresh.captureResolution, fresh.showOnFoot ? "yes" : "no");
}

const Settings &Current() {
  const DWORD now = GetTickCount();
  // Read without the lock first: this runs several times a frame and all but
  // one call in a second has nothing to do.
  if (!g_lastCheck || now - g_lastCheck >= kRecheckMs) {
    std::lock_guard<std::mutex> guard(g_reloadLock);
    if (!g_lastCheck || now - g_lastCheck >= kRecheckMs) {
      g_lastCheck = now ? now : 1;
      if (Changed()) Load();
    }
  }
  return g_settings;
}

uint32_t Generation() { return g_generation.load(); }

}  // namespace radarcfg
