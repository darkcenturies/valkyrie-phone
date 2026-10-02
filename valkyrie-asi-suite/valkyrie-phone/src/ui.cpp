#include "ui.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace ui {
namespace {

float g_left = 0.0f, g_top = 0.0f, g_ppp = 1.0f;
int g_slot = -1;
std::unordered_map<std::string, uintptr_t> g_textures;

bool g_clipped = false;
float g_clipTop = 0.0f, g_clipBottom = kScreenH;

// Touch, in points.
float g_cursorX = 0.0f, g_cursorY = 0.0f;
bool g_pressing = false, g_dragging = false;
float g_pressX = 0.0f, g_pressY = 0.0f, g_lastX = 0.0f, g_lastY = 0.0f;
bool g_tap = false, g_tapTaken = false;
bool g_blocked = false;  // BlockTouch: the rest of this frame is not touched
float g_tapX = 0.0f, g_tapY = 0.0f;
float g_scrollDelta = 0.0f, g_scrollDeltaX = 0.0f;

constexpr float kDragThreshold = 7.0f;  // points
constexpr float kWheelStep = 44.0f;     // one list row

// Clamp a rect to the clip band. False when nothing of it is left.
bool ClipRect(float& y, float& h) {
    if (!g_clipped) {
        return h > 0.0f;
    }
    const float top = std::max(y, g_clipTop);
    const float bottom = std::min(y + h, g_clipBottom);
    if (bottom <= top) {
        return false;
    }
    y = top;
    h = bottom - top;
    return true;
}

bool InClip(float y) {
    return !g_clipped || (y >= g_clipTop && y < g_clipBottom);
}

sprite::TextStyle Style(const Font& f) {
    sprite::TextStyle s;
    s.face = f.face;
    s.height = f.size * g_ppp;
    s.edge = f.edge;
    s.argb = f.argb;
    s.align = f.align;
    s.shadow = f.shadow;
    s.shadowArgb = f.shadowArgb;
    return s;
}

}  // namespace

void SetScreen(float left, float top, float pixelsPerPoint) {
    g_left = left;
    g_top = top;
    g_ppp = pixelsPerPoint;
}

float ToPixelX(float x) { return g_left + x * g_ppp; }
float ToPixelY(float y) { return g_top + y * g_ppp; }
float PixelsPerPoint() { return g_ppp; }

void SetDictionary(int slot) {
    if (slot != g_slot) {
        g_slot = slot;
        g_textures.clear();
    }
}

uintptr_t Tex(const char* name) {
    const std::string selected = config::Get().iconSize == 64 ? std::string(name) + "_64" : name;
    auto it = g_textures.find(selected);
    if (it != g_textures.end() && it->second) {
        return it->second;
    }
    uintptr_t t = sprite::Find(g_slot, selected.c_str());
    if (!t && selected != name) t = sprite::Find(g_slot, name);
    g_textures[selected] = t;
    return t;
}

void Clip(float top, float bottom) {
    g_clipped = true;
    g_clipTop = top;
    g_clipBottom = bottom;
}

void NoClip() {
    g_clipped = false;
}

void Image(const char* texture, float x, float y, float w, float h, uint32_t argb) {
    if (!ClipRect(y, h)) return;
    sprite::Draw(Tex(texture), ToPixelX(x), ToPixelY(y), ToPixelX(x + w), ToPixelY(y + h), argb);
}

bool GameImage(int slot, const char* texture, float x, float y, float w, float h, uint32_t argb) {
    static std::unordered_map<std::string, uintptr_t> cache;
    const std::string key = std::to_string(slot) + ":" + texture;
    auto it = cache.find(key);
    uintptr_t t = it != cache.end() ? it->second : 0;
    if (!t) {
        t = sprite::Find(slot, texture);
        if (t) cache[key] = t;
    }
    if (!t) return false;
    if (!ClipRect(y, h)) return true;
    sprite::Draw(t, ToPixelX(x), ToPixelY(y), ToPixelX(x + w), ToPixelY(y + h), argb);
    return true;
}

void Frame(float x, float y, float w, float h, float t, uint32_t argb) {
    t = std::min(std::max(t, 3.0f), std::min(w, h) * 0.5f);
    Fill(x, y, w, t, argb);
    Fill(x, y + h - t, w, t, argb);
    Fill(x, y + t, t, h - 2 * t, argb);
    Fill(x + w - t, y + t, t, h - 2 * t, argb);
}

void Fill(float x, float y, float w, float h, uint32_t argb) {
    Gradient(x, y, w, h, argb, argb);
}

void Gradient(float x, float y, float w, float h, uint32_t topArgb, uint32_t bottomArgb) {
    if (w <= 0.0f || h <= 0.0f) return;
    auto mix = [](uint32_t a, uint32_t b, float t) {
        uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const float ca = static_cast<float>((a >> shift) & 0xFF);
            const float cb = static_cast<float>((b >> shift) & 0xFF);
            out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) << shift;
        }
        return out;
    };
    // Four flat colour bands reuse the coarse icon shading throughout the UI.
    // Clip each original band separately so scrolling never shifts its colours.
    const int bands = topArgb == bottomArgb ? 1 : 4;
    for (int i = 0; i < bands; ++i) {
        float bandY = y + h * i / bands;
        float bandH = h / bands;
        if (!ClipRect(bandY, bandH)) continue;
        const uint32_t colour = bands == 1 ? topArgb : mix(topArgb, bottomArgb, i / 3.0f);
        sprite::Gradient(ToPixelX(x), ToPixelY(bandY), ToPixelX(x + w), ToPixelY(bandY + bandH), colour, colour);
    }
}

void Needle(float cx, float cy, float angle, float length, float tail, float width, uint32_t argb) {
    // Angle 0 points up the screen and turns clockwise, like a clock.
    const float dx = std::sin(angle), dy = -std::cos(angle);
    const float nx = -dy * width * 0.5f, ny = dx * width * 0.5f;
    const float bx = cx - dx * tail, by = cy - dy * tail;
    const float tx = cx + dx * length, ty = cy + dy * length;
    auto P = [](float x, float y) { return sprite::Corner{ToPixelX(x), ToPixelY(y)}; };
    sprite::Quad(Tex("white"), P(bx - nx, by - ny), P(bx + nx, by + ny), P(tx - nx, ty - ny),
                 P(tx + nx, ty + ny), argb);
}

void Label(float x, float y, const std::string& text, const Font& font) {
    if (text.empty() || !InClip(y) || !InClip(y + font.size)) return;
    sprite::Text(ToPixelX(x), ToPixelY(y), text.c_str(), Style(font));
}

float TextWidth(const std::string& text, const Font& font) {
    return sprite::Width(text.c_str(), Style(font)) / g_ppp;
}

std::string Fit(const std::string& text, float width, const Font& font) {
    return sprite::Fit(text.c_str(), width * g_ppp, Style(font));
}

void Flush() {
    sprite::Flush();
}

void BeginTouch(float cursorX, float cursorY, bool held, bool pressed, bool released, int wheel) {
    g_cursorX = (cursorX - g_left) / g_ppp;
    g_cursorY = (cursorY - g_top) / g_ppp;
    g_tap = false;
    g_tapTaken = false;
    g_blocked = false;
    g_scrollDelta = -static_cast<float>(wheel) * kWheelStep;
    g_scrollDeltaX = 0.0f;

    if (pressed) {
        g_pressing = true;
        g_dragging = false;
        g_pressX = g_cursorX;
        g_pressY = g_cursorY;
        g_lastX = g_cursorX;
        g_lastY = g_cursorY;
    }
    if (g_pressing && held) {
        if (!g_dragging && std::hypot(g_cursorX - g_pressX, g_cursorY - g_pressY) > kDragThreshold) {
            g_dragging = true;
        }
        if (g_dragging) {
            g_scrollDelta += g_lastY - g_cursorY;
            g_scrollDeltaX += g_lastX - g_cursorX;
        }
        g_lastX = g_cursorX;
        g_lastY = g_cursorY;
    }
    if (g_pressing && (released || !held)) {
        g_pressing = false;
        if (!g_dragging) {
            g_tap = true;
            g_tapX = g_pressX;
            g_tapY = g_pressY;
        }
        g_dragging = false;
    }
}

void BlockTouch() { g_blocked = true; }

void CancelTouch() {
    g_pressing = false;
    g_dragging = false;
    g_tap = false;
    g_scrollDelta = g_scrollDeltaX = 0.0f;
    g_cursorX = g_cursorY = -10000.0f;
}

bool Tapped(const Rect& r) {
    if (g_blocked || !g_tap || g_tapTaken || !InClip(g_tapY) || !r.Contains(g_tapX, g_tapY)) {
        return false;
    }
    g_tapTaken = true;
    return true;
}

bool Tapped(float x, float y, float w, float h) {
    return Tapped(Rect{x, y, w, h});
}

bool Pressing(const Rect& r) {
    return !g_blocked && g_pressing && !g_dragging && InClip(g_pressY) && r.Contains(g_pressX, g_pressY) &&
           r.Contains(g_cursorX, g_cursorY);
}

bool AnyTap() {
    return !g_blocked && g_tap && !g_tapTaken;
}

float CursorX() { return g_cursorX; }
float CursorY() { return g_cursorY; }
bool Dragging() { return !g_blocked && g_dragging; }
float ScrollDelta() { return g_blocked ? 0.0f : g_scrollDelta; }
float ScrollDeltaX() { return g_blocked ? 0.0f : g_scrollDeltaX; }

bool TappedAt(const Rect& r, float& x, float& y) {
    if (!Tapped(r)) return false;
    x = g_tapX;
    y = g_tapY;
    return true;
}

}  // namespace ui
