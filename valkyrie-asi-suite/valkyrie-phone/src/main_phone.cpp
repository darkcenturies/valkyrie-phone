// The SP-RP phone for single-player GTA SA, as the 2007 handset.
//
// Like fuel, this is the server's system rewritten against the game itself:
// it needs no server and looks for none. In multiplayer the server already
// gives every player its own phone, so this one stands down there rather than
// put a second phone on the screen.
#include <windows.h>

#include <cstring>
#include <string>

#include "game.h"
#include "game_startup.h"
#include "log.h"
#include "config.h"
#include "phone.h"
#include "scene_capture.h"
#include "radar3d.h"
#include "radar_start.h"
#include "version.h"

namespace {

// The map the phone's Maps app draws, from the radar's tile set - the map
// only, not the radar: the HUD keeps whatever radar it has. When
// valkyrie-radar.asi is installed it draws the map for the phone and this
// copy is not started.
DWORD WINAPI StartRadar(LPVOID) {
    // Every ASI is loaded by now.
    if (!config::Get().features.maps) {
        logfile::Line("maps: the Maps app is off ([Features] Maps=0) - no map drawn");
        return 0;
    }
    // A valkyrie-radar.asi that can draw the phone's map does; one that
    // cannot (a build without the phone's map) is left to its own HUD work,
    // and the phone draws its map itself from the tiles.
    if (HMODULE radar = GetModuleHandleA("valkyrie-radar.asi")) {
        if (GetProcAddress(radar, "ValkyrieRadarPhoneMap")) {
            logfile::Line("maps: valkyrie-radar.asi is installed - it draws the Maps app's map");
            return 0;
        }
        logfile::Line("maps: valkyrie-radar.asi is installed but cannot draw the phone's map - the phone draws it");
    }
    radar3d::RunMapsOnly();
    return 0;
}

DWORD WINAPI Start(LPVOID) {
    logfile::Open("valkyrie-phone.log", "Valkyrie phone");
    logfile::Line("valkyrie-phone %s", VALKYRIE_PHONE_VERSION);

    if (GetModuleHandleA("samp.dll") || GetModuleHandleA("ssmp.dll")) {
        logfile::Line("phone: multiplayer - the server's phone is in charge, standing down");
        return 0;
    }
    if (!game::Init()) {
        logfile::Line("phone: unsupported executable - standing down");
        return 0;
    }

    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) slash[1] = 0;
    phone::Configure(path);
    // RenderWare is initialized and configuration is loaded before map setup.
    StartRadar(nullptr);

    // Logic in the frame hook, the handset where the HUD is painted - the
    // same split fuel uses, for the same reason. The camera also needs a
    // moment before the game's processing (phone::BeforeFrame).
    if (!game::HookFrame(&phone::BeforeFrame, &phone::Frame)) {
        logfile::Line("phone: frame hook unavailable - standing down");
        return 0;
    }
    if (!game::HookHudDraw(&phone::Draw)) {
        logfile::Line("phone: HUD hook unavailable - standing down");
        return 0;
    }
    if (!scene_capture::Register(&phone::CaptureScene)) {
        logfile::Line("phone: pre-HUD scene hook unavailable - live reflections disabled");
    }
    return 0;
}

}  // namespace

// For other mods: valkyrie-inventory's Phone item calls this for its Use.
extern "C" __declspec(dllexport) void __cdecl ValkyriePhoneOpen() { phone::RequestOpen(); }

extern "C" BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        InitializeCriticalSection(&radar3d::g_routeLock);
        if (game::Init()) game_startup::Register(&Start);
    }
    return TRUE;
}
