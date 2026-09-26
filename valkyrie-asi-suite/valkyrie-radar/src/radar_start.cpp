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


// Project Eagle's Lua HUD draws a complete second radar after the native game
// path: its own tilted map, circular gradient ring and gang-area polygons.
// Legacy stock-PE rewrite ONLY. Custom HUDs own their callbacks and must never
// be rewritten automatically. Existing installations are not reverted here:
// their backup may predate intentional user changes.
void PatchProjectEagleHud() {
    char gamePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, gamePath, MAX_PATH);
    char* slash = std::strrchr(gamePath, '\\');
    if (!slash) return;
    slash[1] = '\0';
    const std::string root(gamePath);
    if (!GetPrivateProfileIntA("Compatibility", "AllowLegacyPeHudRewrite", 0,
                              (root + "valkyrie-radar.ini").c_str())) {
        logfile::Line("PE HUD preserved: legacy on-disk rewrite disabled");
        return;
    }
    const std::string hud = root + "hud.lua";
    if (GetFileAttributesA((root + "PECore.asi").c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(hud.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    std::ifstream input(hud, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    constexpr const char* marker = "-- Valkyrie Radar compatibility begin";
    if (text.empty() || text.find(marker) != std::string::npos) return;
    const size_t start = text.find("local RadarAnimTween = quickTween(MAP_ANIM_MS);");
    const size_t finish = text.find("Hud.AfterRender = function()", start);
    if (start == std::string::npos || finish == std::string::npos) return;

    static const char replacement[] = R"HUD(-- Valkyrie Radar compatibility begin
Hud.RenderRadar = function()
	if ExitEnterState() == 1 or ExitEnterState() == 2 or Hud.RadarMode() == RADAR_OFF
		or ((Hud.ItemToFlash() == ITEM_RADAR) and EachFrames(8)) then return; end
	local player = PlayerPed(0);
	local drawInterior = (CurrentInterior() ~= 0) and (player.AreaCode ~= 0);
	local left, top = mapOffset.x, mapOffset.y;
	local right, bottom = left + mapSize.x, top + mapSize.y;
	local sw, sh = ScreenWidth(), ScreenHeight();
	local masks = {
		{ {0,0}, {sw,0}, {sw,top}, {0,top} },
		{ {0,bottom}, {sw,bottom}, {sw,sh}, {0,sh} },
		{ {0,top}, {left,top}, {left,bottom}, {0,bottom} },
		{ {right,top}, {sw,top}, {sw,bottom}, {right,bottom} }
	};
	Hud.DrawRadar({ zoomMult=1.0,
		tiles=function() if drawInterior then return 1; end end,
		masks=masks,
		TransformRadarPointToScreenSpace=TransformRadarPointToScreenSpace });
	-- Gang areas stay on the pause map, not across the live driving panel.
	Hud.DrawBlips();
end
Hud.BeforeRender = function()
	hud_position.x = ScreenStretchFromRight(HUD_POSITION[1]);
	hud_position.y = ScreenStretchY(HUD_POSITION[2]);
	mapSize.x = ScreenStretchX(106.0);
	mapSize.y = ScreenStretchY(82.0);
	mapOffset.x = ScreenStretchX(8.0);
	mapOffset.y = ScreenHeight() - ScreenStretchY(9.0) - mapSize.y;
end
-- Valkyrie Radar compatibility end

)HUD";

    const std::string backup = hud + ".before-valkyrie-radar";
    if (GetFileAttributesA(backup.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyFileA(hud.c_str(), backup.c_str(), TRUE);
    text.replace(start, finish - start, replacement);
    const std::string pending = hud + ".valkyrie.tmp";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (output) {
        MoveFileExA(pending.c_str(), hud.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else {
        DeleteFileA(pending.c_str());
    }
}

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
    PatchProjectEagleHud();
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
    if (!game::InstallProjectEagleGpsClipPatch()) {
        logfile::Line("radar3d: PECore route bridge unavailable");
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
