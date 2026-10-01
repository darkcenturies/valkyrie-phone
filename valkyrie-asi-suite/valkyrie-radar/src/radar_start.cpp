// The live 3D radar's start: shared by valkyrie-radar.asi and by the phone,
// which carries the radar in itself (see radar_start.h).
#include <windows.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "game.h"
#include "log.h"
#include "radar3d.h"
#include "radar_start.h"
#include "switches.h"
#include "version.h"

namespace radar3d {
namespace {

volatile bool g_running = false;


}  // namespace

void Run() {
    char executable[MAX_PATH]{};
    GetModuleFileNameA(nullptr,executable,MAX_PATH);
    if(char* separator=std::strrchr(executable,'\\')) {
        separator[1]='\0';
        const std::string atmosphere=std::string(executable)+"valkyrie-atmosphere.ini";
        if(GetPrivateProfileIntA("inventory","Enabled",0,atmosphere.c_str()) &&
           !GetPrivateProfileIntA("Radar","Enabled",1,atmosphere.c_str())) {
            logfile::Line("Radar disabled explicitly by Atmosphere [Radar] Enabled=0; no hooks installed");
            return;
        }
    }
    logfile::Line("Valkyrie Radar %s (experimental; network-free)",
                  VALKYRIE_RADAR_VERSION);

    // This crashed on the very first real call it made - RwRasterCreate,
    // called from DllMain's own thread before the game's graphics device
    // exists yet. main_map.cpp already knew this and sleeps first; this
    // component needs exactly the same wait, not a special exemption.
    Sleep(5000);
    if (!game::Init()) {
        logfile::Line("radar3d: unsupported executable - standing down");
        return;
    }
    if (!radar3d::Init()) {
        logfile::Line("radar3d: not active this session");
        return;
    }
    g_running = true;
    switches::ReadMap();
    if (!game::InstallRadar3DBackgroundPatch()) {
        logfile::Line("radar3d: background bridge unavailable");
        return;
    }
    if (!game::InstallDrivingRadarPresentationPatch()) {
        logfile::Line("radar3d: player/zone presentation unavailable");
    }
    if (!game::InstallRadarBoxTransformPatch()) {
        logfile::Line("radar3d: rectangular transform unavailable");
    } else if (!game::InstallRadarBoxPerimeterPatch()) {
        logfile::Line("radar3d: rectangular perimeter unavailable");
    } else if (!game::InstallDrivingBlipFilterPatch()) {
        logfile::Line("radar3d: driving blip filter unavailable");
    } else if (!game::InstallRadarBoxHudFramePatch()) {
        logfile::Line("radar3d: rectangular frame unavailable");
    }
    if (!switches::RadarShapeOff() && !game::InstallSquareRadarPatch()) {
        logfile::Line("radar3d: rectangular mask unavailable");
    }
    // Optional: lets the mission ribbon take the objective marker's own
    // colour instead of the configured one. Standing down is not a fault.
    if (!game::InstallObjectiveColourPatch()) {
        logfile::Line("radar3d: objective marker colour unavailable - the "
                      "mission ribbon uses the configured colour");
    }
    // The coord-blip draw hook. sprp-blips installs this too, for the colour
    // it paints server markers in, and it chains rather than fighting - but
    // that install is in sprp-blips' own copy of game.cpp, with its own
    // statics, so it does nothing for this one. Without this call here the
    // icon fade and the objective marker the mission ribbon follows both hang
    // off a replacement that is never reached, which is exactly what they did.
    if (!game::InstallBlipTintPatch()) {
        logfile::Line("radar3d: coord blip hook unavailable - map icons will "
                      "not fade and single-player mission objectives will not "
                      "be followed");
    }
    return;
}

bool Running() { return g_running; }

void RunMapsOnly() {
    // Called by the phone after RenderWare initialization, on the game thread.
    if (!game::Init()) {
        logfile::Line("maps: unsupported executable - no map");
        return;
    }
    if (radar3d::InitForMaps()) g_running = true;
}

}  // namespace radar3d
