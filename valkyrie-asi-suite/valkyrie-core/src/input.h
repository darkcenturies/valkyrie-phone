// Taking the mouse and keyboard off the game for a while.
//
// A plugin that puts something clickable on screen - the phone, say - needs
// three things the game does not give it: a cursor while the game is running
// (the game only has one in its menu), the keys typed while that thing has
// focus, and the game NOT acting on any of it. Moving the mouse must not turn
// the camera and clicking must not fire the gun; typing a text message must
// not also walk the player about and change the radio.
//
// While captured:
//
//   * CPad::UpdateMouse still reads the mouse, and its movement drives a
//     cursor of ours instead of the camera. The game's copy of the mouse state
//     is then cleared, so it sees a mouse that is not moving and not clicked.
//     The movement comes from the same DirectInput read the game makes, so the
//     cursor follows the player's mouse settings and works in full screen.
//   * Key presses and typed characters go to us and not to the game. Key
//     releases always go through: a key held down when the capture started
//     has to be let go of in the game as well, or the player keeps walking.
//
// Each plugin that links this installs its own copy, chained onto whatever
// already holds the call site and the window procedure, the same way game.cpp
// chains the frame hook - so two of them can coexist.
#pragma once

#include <cstdint>

namespace input {

// Hook the mouse read and subclass the game window. Call from the game thread
// (a frame callback) - the window has to exist. Safe to call every frame
// until it returns true.
bool Install();

// Start or stop taking input from the game.
void Capture(bool on);
bool Captured();

// Where our cursor is, in screen pixels. Starts in the middle of the screen
// and is kept on it.
struct Point {
    float x, y;
};
Point Cursor();
void SetCursor(Point at);

// The left and right buttons: held now, and went down since the last frame.
bool LeftHeld();
bool LeftPressed();
bool LeftReleased();
bool RightPressed();

// The right button kept from the game while the mouse is otherwise still its
// own (not captured) - for raising something held at the side. While
// claimed the game never sees it pressed; RightClaimPressed is true once
// when it goes down - ask it once a frame.
void ClaimRight(bool on);
bool RightClaimPressed();

// Wheel steps since the last frame: positive is away from the player.
int Wheel();

// Call once per frame, on the game thread, before reading any of the above.
// It moves the per-frame edges along.
void BeginFrame();

// A keyboard event while captured. `ch` is a typed character (already
// translated for the keyboard layout and shift); `vk` is a key that is not a
// character - Enter, Backspace, Escape, the arrows, Delete, Tab.
struct Key {
    int vk;
    char ch;
};

// Take the next keyboard event, oldest first. False when there are none.
bool NextKey(Key& out);

// Whether a key is physically down right now, whatever the capture - for a
// hotkey that opens the thing that then captures.
bool KeyDown(int vk);

// True on the frame the key went down. Tracked for the few keys a caller asks
// about through this; call it every frame for a key you care about.
bool KeyPressed(int vk);

}  // namespace input
