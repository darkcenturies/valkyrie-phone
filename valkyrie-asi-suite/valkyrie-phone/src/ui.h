// Drawing and touching the phone's screen.
//
// Everything is laid out in points on a 320 x 480 screen, which is what the
// 2007 phone's interface was designed on, and turned into pixels here - so the
// layout reads like the real thing and scales to any resolution. Positions
// passed to these functions are points, measured from the top left of the
// lit screen.
#pragma once

#include <cstdint>
#include <string>

#include "sprite.h"

namespace ui {

constexpr float kScreenW = 320.0f;
constexpr float kScreenH = 480.0f;

struct Rect {
    float x, y, w, h;
    bool Contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Where the screen is this frame, in pixels, and how many pixels a point is.
void SetScreen(float left, float top, float pixelsPerPoint);
float ToPixelX(float x);
float ToPixelY(float y);
float PixelsPerPoint();

// The texture dictionary and a lookup by name, cached after the first time.
void SetDictionary(int slot);
uintptr_t Tex(const char* name);

// Anything outside this band of the screen is not drawn (and cannot be
// tapped). Content that scrolls sets it to the space between its bars.
void Clip(float top, float bottom);
void NoClip();

void Image(const char* texture, float x, float y, float w, float h, uint32_t argb = 0xFFFFFFFF);
void Fill(float x, float y, float w, float h, uint32_t argb);
void Gradient(float x, float y, float w, float h, uint32_t topArgb, uint32_t bottomArgb);
// A flat line of the given thickness, from its base to its tip, rotated.
void Needle(float cx, float cy, float angleRadians, float length, float tail, float width,
            uint32_t argb);

struct Font {
    float size = 17.0f;  // points
    uint32_t argb = 0xFF000000;
    sprite::Align align = sprite::Align::Left;
    bool shadow = false;
    uint32_t shadowArgb = 0xC0000000;
    int edge = 0;  // the game's black text outline
    sprite::Face face = sprite::Face::Subtitles;
};

// Draw a texture from a dictionary other than the phone's own - the game's
// radar icons and mouse cursor. False when that texture is not there, so a
// caller can draw its own instead.
bool GameImage(int slot, const char* texture, float x, float y, float w, float h,
               uint32_t argb = 0xFFFFFFFF);

// A rectangle's outline, `thickness` points wide, drawn inside it.
void Frame(float x, float y, float w, float h, float thickness, uint32_t argb);

// Text at (x, y), y being the top of the line. Nothing is drawn if the line
// falls outside the clip band.
void Label(float x, float y, const std::string& text, const Font& font);
float TextWidth(const std::string& text, const Font& font);  // in points
std::string Fit(const std::string& text, float width, const Font& font);

// Draw the queued text now, so it stays under what is drawn next.
void Flush();

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

// Feed this frame's mouse in: the cursor in pixels, and the button.
void BeginTouch(float cursorX, float cursorY, bool held, bool pressed, bool released, int wheel);

// Forget any press and tap: nothing on the screen is touched this frame, and
// a press already down does not become a tap when it is let go.
void CancelTouch();

// Nothing drawn after this is touched this frame, but a press in progress
// is kept, so whatever was touched before it (an overlay taking its own
// touches first) goes on being dragged next frame.
void BlockTouch();

// A tap is a press and release without the cursor moving far in between -
// a drag scrolls instead. True once, on the release, for the rect the press
// started in.
bool Tapped(const Rect& r);
bool Tapped(float x, float y, float w, float h);
// Whether the finger is down on this rect right now, for highlighting.
bool Pressing(const Rect& r);
// Whether any tap happened this frame (so a screen can tell a tap on nothing
// from no tap at all).
bool AnyTap();

// The cursor in points, and whether the press is being dragged.
float CursorX();
float CursorY();
bool Dragging();
// How far the press has been dragged vertically this frame, in points, plus
// the wheel converted to points. Screens add it to their scroll.
float ScrollDelta();
// The same sideways, for content wider than the screen. Dragging only.
float ScrollDeltaX();
// Tapped(), and where in the rect the tap was.
bool TappedAt(const Rect& r, float& x, float& y);

}  // namespace ui
