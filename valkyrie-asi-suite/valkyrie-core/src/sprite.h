// Pictures and text drawn the way the game draws its own interface.
//
// game.h already has flat rectangles and single lines of text for panels. This
// is the next step up for a plugin that brings its own artwork: a texture
// dictionary loaded from a loose .txd file next to the game, textures drawn
// from it with CSprite2d, and text that can be centred, right-aligned, wrapped
// and cut to fit.
//
// Everything here must be called on the game thread, from inside a draw hook.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sprite {

// Load a texture dictionary from a file into a slot of its own. `slotName` is
// the name the game's texture store knows it by and must be unique across
// every plugin. Returns the slot, or -1 when the file is missing or the store
// would not take it. Loading the same slot name twice returns the first slot.
int LoadDictionary(const char* slotName, const char* path);

// A texture in a dictionary this plugin loaded, or 0. The pointer is the
// game's RwTexture and belongs to the game: do not keep it past the frame.
uintptr_t Find(int slot, const char* name);

// A texture made at run time from 32-bit pixels, 0xAARRGGBB, `pitch` pixels
// from one row to the next. It belongs to the caller: DestroyTexture it when
// done. Returns 0 when the game would not make it.
uintptr_t CreateTexture(int width, int height, const uint32_t* pixels, int pitch);
void DestroyTexture(uintptr_t texture);

// Draw a texture into a screen rectangle. Colour multiplies the texture; white
// at full alpha draws it as it is.
void Draw(uintptr_t texture, float left, float top, float right, float bottom,
          uint32_t argb = 0xFFFFFFFF);

// Any four-cornered shape: a texture drawn between two base corners and two
// far corners - a clock hand, a needle. Use a plain white texture and a
// colour for a flat shape.
// Corners go (base left, base right, far left, far right).
struct Corner {
    float x, y;
};
void Quad(uintptr_t texture, Corner baseLeft, Corner baseRight, Corner farLeft, Corner farRight,
          uint32_t argb);

// The game's own four-colour rectangle: one colour per corner, blended. Used
// for gradients that do not need a texture.
void Gradient(float left, float top, float right, float bottom, uint32_t topArgb,
              uint32_t bottomArgb);

// CFont's four faces.
enum class Face : short {
    Gothic = 0,     // the title font
    Subtitles = 1,  // the proportional body font
    Menu = 2,       // the menu's upright sans
    Pricedown = 3,
};

enum class Align : unsigned char {
    Centre = 0,
    Left = 1,
    Right = 2,
};

struct TextStyle {
    Face face = Face::Subtitles;
    float height = 14.0f;  // screen pixels from the top of a capital to the baseline, roughly
    uint32_t argb = 0xFFFFFFFF;
    Align align = Align::Left;
    bool shadow = false;
    int edge = 0;  // CFont's black outline, in its own units: 0 none, 1 or 2 as the game uses
    float widthRatio = 0.5f;  // CFont's x scale over its y scale; scripts set both
    uint32_t shadowArgb = 0xC0000000;
};

// One line at (x, y), where y is the top of the text and x is the left edge,
// the centre, or the right edge depending on the alignment. The text is queued
// in the game's text batch; nothing appears until Flush().
void Text(float x, float y, const char* text, const TextStyle& style);

// Width in pixels of `text` drawn in that style.
float Width(const char* text, const TextStyle& style);

// The distance from one line to the next in that style.
float LineHeight(const TextStyle& style);

// Break `text` into lines no wider than `width`, at spaces where it can and in
// the middle of a word where one word is too long for a line.
std::vector<std::string> Wrap(const char* text, float width, const TextStyle& style);

// `text` cut down with "..." so it fits `width`.
std::string Fit(const char* text, float width, const TextStyle& style);

// Keep drawing inside a screen rectangle, and stop. They nest: one inside
// another is cut to the outer one, and each End puts back what its Begin
// found. Uses the Direct3D scissor,
// which the game's own 2D drawing honours; text still queued when the
// scissor ends is drawn unclipped, so Flush() before EndScissor().
void BeginScissor(float left, float top, float right, float bottom);
void EndScissor();

// Draw the queued text now. Call it between layers so that text belonging to
// something underneath is not drawn over what is on top of it.
void Flush();

// Make a string safe for CFont: '~' starts a formatting tag there, '*' has
// no glyph and hangs it, and anything outside printable ASCII has no glyph.
std::string Printable(const std::string& text);

}  // namespace sprite
