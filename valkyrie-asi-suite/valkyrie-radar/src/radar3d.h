// Lightweight 3D radar. It draws the website's stripped, vertex-colour world
// tiles through an independent Direct3D target. It never changes GTA's camera,
// visibility lists, streaming state or RenderScene pipeline.
//
// Off by default. Requires a marker file rather than the usual switches.h
// pattern of a file to turn something off - this is the one thing in the
// suite that should need to be asked for, not opted out of.
//
//   valkyrie-radar.asi  installs and enables the radar; removing it disables it
#pragma once
#include <vector>
#include <windows.h>

namespace radar3d {

// Guards the built GPS route geometry, which the network thread replaces and
// the render thread reads. Exposed for DllMain init/cleanup.
extern CRITICAL_SECTION g_routeLock;

// Checks the executable version (independently of game::Init - this stays
// fully separable from the rest of the suite) and the opt-in marker file,
// checks for converted tiles and installs the post-HUD draw hook described in
// radar3d.cpp. Everything after this is self-driven - there is
// nothing to call once a frame from outside, unlike the rest of this suite.
// False means nothing here runs for the rest of the session.
bool Init();
// For the phone's Maps app alone: the tile set and the device the map is
// drawn with, and nothing of the radar itself - no HUD hooks, no routes, no
// waypoint watching.
bool InitForMaps();

}  // namespace radar3d
