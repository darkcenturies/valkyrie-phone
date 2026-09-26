#include "arcade.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "input.h"
#include "log.h"

namespace arcade {
namespace {

float g_left = 0.0f, g_top = 0.0f, g_scale = 1.0f;
std::string g_gameDir;
std::map<std::string, int> g_dictionaries;

}  // namespace

Pad ReadPad() {
    auto down = [](int vk) { return input::KeyDown(vk); };
    Pad p{};
    p.left = down(VK_LEFT) || down('A');
    p.right = down(VK_RIGHT) || down('D');
    p.up = down(VK_UP) || down('W');
    p.down = down(VK_DOWN) || down('S');
    p.cross = down(VK_SPACE) || down(VK_RETURN);
    p.circle = input::LeftHeld() || down(VK_CONTROL);
    p.triangle = down(VK_BACK);
    p.square = down(VK_SHIFT);
    p.l1 = down('Q');
    p.r1 = down('E');
    return p;
}

void SetViewport(float left, float top, float width, float height) {
    g_scale = std::min(width / kWidth, height / kHeight);
    g_left = left + (width - kWidth * g_scale) * 0.5f;
    g_top = top + (height - kHeight * g_scale) * 0.5f;
}

float X(float x) { return g_left + x * g_scale; }
float Y(float y) { return g_top + y * g_scale; }
float Scale() { return g_scale; }

int Dictionary(const char* name) {
    auto it = g_dictionaries.find(name);
    if (it != g_dictionaries.end()) return it->second;
    if (g_gameDir.empty()) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) slash[1] = 0;
        g_gameDir = path;
    }
    const std::string slot = std::string("valkyrie_phone_") + name;
    const std::string file = g_gameDir + "models\\txd\\" + name + ".txd";
    const int d = sprite::LoadDictionary(slot.c_str(), file.c_str());
    g_dictionaries[name] = d;
    return d;
}

uintptr_t Texture(int dictionary, const char* name) {
    return sprite::Find(dictionary, name);
}

void Sprite(uintptr_t texture, float x, float y, float w, float h, uint32_t argb) {
    if (!texture) return;
    // A mirrored sprite is the same quad with its corners swapped.
    const float l = X(x - w * 0.5f), r = X(x + w * 0.5f);
    const float t = Y(y - h * 0.5f), b = Y(y + h * 0.5f);
    sprite::Quad(texture, {l, b}, {r, b}, {l, t}, {r, t}, argb);
}

void SpriteRotated(uintptr_t texture, float x, float y, float w, float h, float degrees,
                   uint32_t argb) {
    if (!texture) return;
    const float a = degrees * 3.14159265f / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    auto corner = [&](float dx, float dy) {
        // Clockwise on a screen whose y runs down.
        return sprite::Corner{X(x + dx * c - dy * s), Y(y + dx * s + dy * c)};
    };
    const float hw = w * 0.5f, hh = h * 0.5f;
    sprite::Quad(texture, corner(-hw, hh), corner(hw, hh), corner(-hw, -hh), corner(hw, -hh), argb);
}

void Box(float x, float y, float w, float h, uint32_t argb) {
    sprite::Gradient(X(x - w * 0.5f), Y(y - h * 0.5f), X(x + w * 0.5f), Y(y + h * 0.5f), argb, argb);
}

void Window(float left, float top, float right, float bottom) {
    // The help box: black, a little see-through.
    sprite::Gradient(X(left), Y(top), X(right), Y(bottom), 0xB4000000, 0xB4000000);
}

namespace {
sprite::TextStyle Style(const TextStyle& t) {
    sprite::TextStyle s;
    s.face = t.face;
    // Script text is drawn at half its y scale (CTheScripts::DrawScriptText),
    // and CFont is 18 units tall per unit of scale: 9 script units per unit.
    // The x scale is not halved, so the width ratio doubles. The rows of the
    // arcade high-score tables, 28.6 units apart at scale 3.0, fit exactly.
    s.height = 9.0f * t.scaleY * g_scale;
    s.widthRatio = t.scaleY > 0.0f ? 2.0f * t.scaleX / t.scaleY : 0.5f;
    s.argb = t.argb;
    s.align = t.align;
    s.edge = t.edge;
    s.shadow = t.shadow && t.edge == 0;
    s.shadowArgb = 0xB4000000;
    return s;
}
}  // namespace

void Text(float x, float y, const std::string& text, const TextStyle& style) {
    sprite::TextStyle s = Style(style);
    // The machines' text was sized for their wide screen; on the upright one
    // a line too long for the room it has (from x to the edge it runs
    // towards, a little in) is drawn smaller, so none is cut off.
    constexpr float kMargin = 8.0f;
    const float room = (style.align == sprite::Align::Left    ? kWidth - x - kMargin
                        : style.align == sprite::Align::Right ? x - kMargin
                                                              : 2.0f * (std::min(x, kWidth - x) - kMargin)) *
                       g_scale;
    const float wide = sprite::Width(text.c_str(), s);
    if (room > 0.0f && wide > room) s.height *= room / wide;
    sprite::Text(X(x), Y(y), text.c_str(), s);
}

void Help(const std::string& text) {
    int lines = 1;
    for (size_t p = text.find("~n~"); p != std::string::npos; p = text.find("~n~", p + 3)) ++lines;
    Window(33.8609f, 18.1114f, 230.0f, 18.1114f + 8.0f + lines * 17.0f);
    TextStyle s;
    s.face = sprite::Face::Subtitles;
    s.scaleX = 0.5014f;
    s.scaleY = 1.8889f;
    s.argb = 0xFFE1E1E1;
    s.align = sprite::Align::Left;
    s.shadow = false;
    float y = 20.4681f;
    size_t p = 0;
    while (p <= text.size()) {
        const size_t n = text.find("~n~", p);
        Text(38.1753f, y, text.substr(p, n == std::string::npos ? std::string::npos : n - p), s);
        y += 17.0f;
        if (n == std::string::npos) break;
        p = n + 3;
    }
}

void Paragraph(float x, float y, float wrapX, const std::string& text, const TextStyle& style) {
    const sprite::TextStyle s = Style(style);
    float ly = Y(y);
    for (const auto& line : sprite::Wrap(text.c_str(), (wrapX - x) * g_scale, s)) {
        sprite::Text(X(x), ly, line.c_str(), s);
        ly += sprite::LineHeight(s);
    }
}

}  // namespace arcade
