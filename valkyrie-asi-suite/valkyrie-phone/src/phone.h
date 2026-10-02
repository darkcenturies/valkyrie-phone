// The phone: a single-player port of the SP-RP server's phone, drawn as the
// 2007 handset.
#pragma once

#include <string>

namespace phone {

// Read valkyrie-phone.ini. gameDir ends in a backslash.
void Configure(const std::string& gameDir);

// Game thread, once a frame, before the game's own processing: the camera
// app turns CJ and places the game's camera here, so both are in place for
// the frame that follows.
void BeforeFrame();

// Game thread, once a frame, outside the draw: the hotkey, loading, the
// services queue, and letting go of the mouse whenever the phone is not being
// drawn.
void Frame();

// Game thread, from the HUD draw.
void Draw();

// Completed world frame, before the game's 2D/HUD pass adds any overlays.
void CaptureScene();

// Any thread: take the phone out as soon as the player can use it (asked by
// another mod - the inventory's Use on its Phone - while its menu is up).
void RequestOpen();

}  // namespace phone
