// The live 3D radar's start, as its own ASI and the phone both make it.
#pragma once

namespace radar3d {

// Waits for the game's graphics, then puts the radar in: its hooks, the
// Project Eagle HUD bridge, the route. Run on a thread of its own, once; the
// route lock (radar3d::g_routeLock) must be made first.
void Run();

// True once Run has put the radar in (it can stand down: an unsupported
// game, turned off in Atmosphere, no tiles).
bool Running();

// What the phone carries: the map its Maps app draws, from the same tiles,
// and none of the radar - the HUD's radar is left to the game, or to
// valkyrie-radar.asi. Call once on the game thread after RenderWare initialization.
void RunMapsOnly();

}  // namespace radar3d
