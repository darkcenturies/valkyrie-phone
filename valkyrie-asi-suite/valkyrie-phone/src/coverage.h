// The phone's signal, from the radio masts standing on the map.
//
// tools/signal-coverage works out, for every 100 x 100 cell of the world,
// how much of a 900 MHz signal is lost on its way from the nearest masts
// (Okumura-Hata path loss and the hills in between), for GTA SA's
// three masts. The phone carries that as
// valkyrie-signal.bin and reads where CJ is standing from it: less indoors
// and in a car, more high up, none under water, drifting a little as a real
// signal does. The bars, the browser's speed and whether a call gets
// through or holds all follow it.
#pragma once

#include <string>

namespace coverage {

// Read the stock San Andreas grid from valkyrie-signal.bin.
// False when there is none (the signal is then
// always full).
bool Load(const std::string& path);

// A frame on: where CJ is now. `enabled` is [Features] Signal.
void Update(bool enabled);

// 0 (No Service) to 5, as the status bar shows it.
int Bars();
bool HasService();
// How good the connection is, 0 (none) to 1 (full): the data rate and a
// call's odds follow it.
float Quality();

}  // namespace coverage
