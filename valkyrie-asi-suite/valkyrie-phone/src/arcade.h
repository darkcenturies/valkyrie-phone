// What San Andreas' arcade machines need to run on the phone.
//
// The machines in the game are mission scripts drawing on the 640 x 448
// screen that script sprites and text use, from the game's own texture
// dictionaries (models\txd\LD_*.txd). The phone ports each script's game as
// it is; this is the screen they draw on, fitted into the phone turned on its
// side, and the joypad they read, taken from the keyboard.
#pragma once

#include <cstdint>
#include <string>

#include "sprite.h"

namespace arcade {

constexpr float kWidth = 448.0f;
constexpr float kHeight = 672.0f;

// The joypad as the scripts read it, held this frame.
struct Pad {
    bool left, right, up, down;
    bool cross;     // PED_SPRINT: Space, or Enter
    bool circle;    // PED_FIREWEAPON: the left mouse button, or Ctrl
    bool triangle;  // VEHICLE_ENTER_EXIT: Backspace
    bool square;    // PED_JUMPING: Shift
    bool l1, r1;    // Q and E
};
Pad ReadPad();

// The canvas: script coordinates to screen pixels.
void SetViewport(float left, float top, float width, float height);
float X(float x);
float Y(float y);
float Scale();  // pixels per script unit

// Load one of the game's LD_ dictionaries from the player's own files.
int Dictionary(const char* name);
uintptr_t Texture(int dictionary, const char* name);

// draw_sprite: centred at (x, y); a negative width or height mirrors it, as
// the scripts use to draw one corner four ways. Colour is 0xAARRGGBB.
void Sprite(uintptr_t texture, float x, float y, float w, float h, uint32_t argb);
// The same, turned `degrees` clockwise on screen about its centre.
void SpriteRotated(uintptr_t texture, float x, float y, float w, float h, float degrees,
                   uint32_t argb);
// draw_rect: a flat box, centred.
void Box(float x, float y, float w, float h, uint32_t argb);
// draw_window: the help box's black panel between two corners.
void Window(float left, float top, float right, float bottom);

// display_text, after the script has set its text up. `align` is the
// script's centre / justify / right_justify; `scaleX`, `scaleY` are its
// set_text_scale; `edge` its set_text_edge.
struct TextStyle {
    sprite::Face face = sprite::Face::Menu;
    float scaleX = 1.0f, scaleY = 3.0f;
    uint32_t argb = 0xFFB4B4B4;
    sprite::Align align = sprite::Align::Centre;
    int edge = 0;
    bool shadow = true;
};
void Text(float x, float y, const std::string& text, const TextStyle& style);
// Several lines of help-box text from the top left, wrapped at `wrapX`.
void Paragraph(float x, float y, float wrapX, const std::string& text, const TextStyle& style);

// The scripts' help box in the top left: lines split by ~n~.
void Help(const std::string& text);

}  // namespace arcade
