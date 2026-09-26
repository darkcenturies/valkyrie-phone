#include "sprite.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstring>

#include "game.h"
#include "log.h"

namespace sprite {
namespace {

// CTxdStore, v1.0 US. The pool pointer is read through, never indexed at a
// fixed address: the limit adjuster can give the store a bigger pool.
constexpr uintptr_t kTxdFindSlot = 0x731850;
constexpr uintptr_t kTxdAddSlot = 0x731C80;
constexpr uintptr_t kTxdRemoveSlot = 0x731CD0;
constexpr uintptr_t kTxdLoad = 0x7320B0;
constexpr uintptr_t kTxdAddRef = 0x731A00;
constexpr uintptr_t kTxdPool = 0xC8800C;
constexpr uintptr_t kRwFindNamedTexture = 0x7F39F0;

// RenderWare, for textures made at run time.
constexpr uintptr_t kRwRasterCreate = 0x7FB230;   // (width, height, depth, flags)
constexpr uintptr_t kRwRasterLock = 0x7FB2D0;     // (raster, level, lock mode)
constexpr uintptr_t kRwRasterUnlock = 0x7FAEC0;
constexpr uintptr_t kRwTextureCreate = 0x7F37C0;
constexpr uintptr_t kRwTextureDestroy = 0x7F3820;  // destroys the raster with it
constexpr int kRasterTexture8888 = 0x0504;          // rwRASTERTYPETEXTURE | rwRASTERFORMAT8888
constexpr int kRasterLockWrite = 1;
constexpr uintptr_t kRasterStride = 0x18;           // RwRaster::stride
constexpr uintptr_t kTextureFilterAddressing = 0x50;
constexpr uint32_t kLinearClamp = 0x3302;           // rwFILTERLINEAR, clamped in u and v

// CSprite2d.
constexpr uintptr_t kSpriteDraw = 0x7282C0;       // Draw(float x, y, w, h, const CRGBA&)
constexpr uintptr_t kSpriteDrawQuad = 0x728520;   // Draw(float x1, y1, ... x4, y4, const CRGBA&)

// CFont.
constexpr uintptr_t kFontSetScale = 0x719380;
constexpr uintptr_t kFontSetColor = 0x719430;
constexpr uintptr_t kFontSetFontStyle = 0x719490;
constexpr uintptr_t kFontSetWrapx = 0x7194D0;
constexpr uintptr_t kFontSetCentreSize = 0x7194E0;
constexpr uintptr_t kFontSetRightJustifyWrap = 0x7194F0;
constexpr uintptr_t kFontSetDropColor = 0x719510;
constexpr uintptr_t kFontSetDropShadow = 0x719570;
constexpr uintptr_t kFontSetEdge = 0x719590;
constexpr uintptr_t kFontSetProportional = 0x7195B0;
constexpr uintptr_t kFontSetBackground = 0x7195C0;
constexpr uintptr_t kFontSetJustify = 0x719600;
constexpr uintptr_t kFontSetOrientation = 0x719610;
constexpr uintptr_t kFontPrintString = 0x71A700;
constexpr uintptr_t kFontGetStringWidth = 0x71A0E0;

// CFont::GetHeight is scale.y * 18 pixels (32 / 2 + 2). The width scale that
// looks like the game's own text is about half the height scale.
constexpr float kPixelsPerScaleY = 18.0f;

template <typename T>
T Field(uintptr_t at) {
    return *reinterpret_cast<const T*>(at);
}

// CRGBA is red, green, blue, alpha in memory; the game passes it by value to
// CFont and by reference to CSprite2d.
uint32_t Rgba(uint32_t argb) {
    return ((argb & 0x00FF0000) >> 16) | (argb & 0x0000FF00) | ((argb & 0x000000FF) << 16) |
           (argb & 0xFF000000);
}

void Prepare(const TextStyle& s) {
    const float scaleY = s.height / kPixelsPerScaleY;
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetFontStyle)(static_cast<short>(s.face));
    reinterpret_cast<void(__cdecl*)(float, float)>(kFontSetScale)(scaleY * s.widthRatio, scaleY);
    reinterpret_cast<void(__cdecl*)(bool)>(kFontSetProportional)(true);
    reinterpret_cast<void(__cdecl*)(bool, bool)>(kFontSetBackground)(false, false);
    reinterpret_cast<void(__cdecl*)(bool)>(kFontSetJustify)(false);
    reinterpret_cast<void(__cdecl*)(unsigned char)>(kFontSetOrientation)(
        static_cast<unsigned char>(s.align));
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetDropShadow)(s.shadow ? 1 : 0);
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetEdge)(static_cast<short>(s.edge));
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kFontSetDropColor)(Rgba(s.shadowArgb));
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kFontSetColor)(Rgba(s.argb));
    // Wrapping is ours (see Wrap). Keep the game's own well out of the way,
    // in every alignment.
    const float offscreen = game::ScreenSize().x * 4.0f;
    reinterpret_cast<void(__cdecl*)(float)>(kFontSetWrapx)(offscreen);
    reinterpret_cast<void(__cdecl*)(float)>(kFontSetCentreSize)(offscreen);
    reinterpret_cast<void(__cdecl*)(float)>(kFontSetRightJustifyWrap)(0.0f);
}

}  // namespace

int LoadDictionary(const char* slotName, const char* path) {
    const int existing = reinterpret_cast<int(__cdecl*)(const char*)>(kTxdFindSlot)(slotName);
    if (existing >= 0) {
        return existing;
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        logfile::Line("sprite: %s is not there", path);
        return -1;
    }
    const int slot = reinterpret_cast<int(__cdecl*)(const char*)>(kTxdAddSlot)(slotName);
    if (slot < 0) {
        logfile::Line("sprite: the texture store has no free slot for %s", slotName);
        return -1;
    }
    if (!reinterpret_cast<bool(__cdecl*)(int, const char*)>(kTxdLoad)(slot, path)) {
        logfile::Line("sprite: %s would not load", path);
        reinterpret_cast<void(__cdecl*)(int)>(kTxdRemoveSlot)(slot);
        return -1;
    }
    // Hold a reference so streaming never throws it away under us.
    reinterpret_cast<void(__cdecl*)(int)>(kTxdAddRef)(slot);
    logfile::Line("sprite: loaded %s into slot %d", path, slot);
    return slot;
}

uintptr_t Find(int slot, const char* name) {
    const uintptr_t pool = Field<uintptr_t>(kTxdPool);
    if (slot < 0 || !pool || slot >= Field<int>(pool + 8)) {
        return 0;
    }
    const uintptr_t objects = Field<uintptr_t>(pool);
    const uintptr_t dictionary = objects ? Field<uintptr_t>(objects + slot * 12) : 0;
    if (!dictionary) {
        return 0;
    }
    return reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, const char*)>(kRwFindNamedTexture)(
        dictionary, name);
}

uintptr_t CreateTexture(int width, int height, const uint32_t* pixels, int pitch) {
    void* raster = reinterpret_cast<void*(__cdecl*)(int, int, int, int)>(kRwRasterCreate)(
        width, height, 32, kRasterTexture8888);
    if (!raster) return 0;
    auto* out = reinterpret_cast<uint8_t*(__cdecl*)(void*, unsigned char, int)>(kRwRasterLock)(
        raster, 0, kRasterLockWrite);
    if (out) {
        // D3D's A8R8G8B8 is the same 0xAARRGGBB, little-endian.
        const int stride = Field<int>(reinterpret_cast<uintptr_t>(raster) + kRasterStride);
        for (int y = 0; y < height; ++y) {
            memcpy(out + static_cast<size_t>(y) * stride, pixels + static_cast<size_t>(y) * pitch,
                   static_cast<size_t>(width) * 4);
        }
        reinterpret_cast<void*(__cdecl*)(void*)>(kRwRasterUnlock)(raster);
    }
    const uintptr_t texture = reinterpret_cast<uintptr_t(__cdecl*)(void*)>(kRwTextureCreate)(raster);
    if (texture) *reinterpret_cast<uint32_t*>(texture + kTextureFilterAddressing) = kLinearClamp;
    return texture;
}

void DestroyTexture(uintptr_t texture) {
    if (texture) reinterpret_cast<int(__cdecl*)(uintptr_t)>(kRwTextureDestroy)(texture);
}

void Draw(uintptr_t texture, float left, float top, float right, float bottom, uint32_t argb) {
    if (!texture || right <= left || bottom <= top) {
        return;
    }
    const uint32_t colour = Rgba(argb);
    // A CSprite2d is one texture pointer; this borrows the game's texture for
    // one call and never owns it.
    uintptr_t sprite = texture;
    reinterpret_cast<void(__thiscall*)(void*, float, float, float, float, const uint32_t&)>(
        kSpriteDraw)(&sprite, left, top, right - left, bottom - top, colour);
}

void Quad(uintptr_t texture, Corner baseLeft, Corner baseRight, Corner farLeft, Corner farRight,
          uint32_t argb) {
    if (!texture) {
        return;
    }
    const uint32_t colour = Rgba(argb);
    // SetVertices maps (x3, y3) to the texture's top left, (x4, y4) top right,
    // (x2, y2) bottom right and (x1, y1) bottom left.
    uintptr_t sprite = texture;
    using Fn = void(__thiscall*)(void*, float, float, float, float, float, float, float, float,
                                 const uint32_t&);
    reinterpret_cast<Fn>(kSpriteDrawQuad)(&sprite, baseLeft.x, baseLeft.y, baseRight.x,
                                          baseRight.y, farLeft.x, farLeft.y, farRight.x,
                                          farRight.y, colour);
}

void Gradient(float left, float top, float right, float bottom, uint32_t topArgb,
              uint32_t bottomArgb) {
    // Drawn as flat bands a couple of pixels high. The game's four-colour
    // rectangle split its colours across the diagonal instead of top to
    // bottom when tried, so this uses only the plain rectangle, which is known
    // to be right.
    if (topArgb == bottomArgb) {
        game::DrawRect(left, top, right, bottom, topArgb);
        return;
    }
    const int bands = std::clamp(static_cast<int>((bottom - top) / 2.0f), 1, 96);
    for (int i = 0; i < bands; ++i) {
        const float t0 = static_cast<float>(i) / bands;
        const float t1 = static_cast<float>(i + 1) / bands;
        const float tm = (t0 + t1) * 0.5f;
        uint32_t c = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const float a = static_cast<float>((topArgb >> shift) & 0xFF);
            const float b = static_cast<float>((bottomArgb >> shift) & 0xFF);
            c |= static_cast<uint32_t>(a + (b - a) * tm + 0.5f) << shift;
        }
        game::DrawRect(left, top + (bottom - top) * t0, right, top + (bottom - top) * t1, c);
    }
}

void Text(float x, float y, const char* text, const TextStyle& style) {
    if (!text || !*text) {
        return;
    }
    Prepare(style);
    const std::string safe = Printable(text);
    reinterpret_cast<void(__cdecl*)(float, float, const char*)>(kFontPrintString)(x, y,
                                                                                  safe.c_str());
}

float Width(const char* text, const TextStyle& style) {
    if (!text || !*text) {
        return 0.0f;
    }
    Prepare(style);
    const std::string safe = Printable(text);
    return reinterpret_cast<float(__cdecl*)(const char*, bool, bool)>(kFontGetStringWidth)(
        safe.c_str(), true, false);
}

float LineHeight(const TextStyle& style) {
    return style.height * 1.3f;
}

std::vector<std::string> Wrap(const char* text, float width, const TextStyle& style) {
    std::vector<std::string> lines;
    std::string line;
    std::string word;
    auto fits = [&](const std::string& s) { return Width(s.c_str(), style) <= width; };
    auto pushWord = [&]() {
        if (word.empty()) {
            return;
        }
        const std::string joined = line.empty() ? word : line + " " + word;
        if (fits(joined)) {
            line = joined;
        } else {
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            // A word longer than the whole line is broken where it has to be.
            while (!fits(word)) {
                size_t cut = word.size() - 1;
                while (cut > 1 && !fits(word.substr(0, cut))) {
                    --cut;
                }
                lines.push_back(word.substr(0, cut));
                word.erase(0, cut);
            }
            line = word;
        }
        word.clear();
    };
    for (const char* p = text ? text : ""; *p; ++p) {
        if (*p == ' ' || *p == '\n') {
            pushWord();
            if (*p == '\n') {
                lines.push_back(line);
                line.clear();
            }
        } else {
            word.push_back(*p);
        }
    }
    pushWord();
    if (!line.empty() || lines.empty()) {
        lines.push_back(line);
    }
    return lines;
}

std::string Fit(const char* text, float width, const TextStyle& style) {
    std::string s = text ? text : "";
    if (Width(s.c_str(), style) <= width) {
        return s;
    }
    while (!s.empty() && Width((s + "...").c_str(), style) > width) {
        s.pop_back();
    }
    return s + "...";
}

// The game's Direct3D device, as RenderWare's D3D9 driver keeps it.
constexpr uintptr_t kD3DDevice = 0xC97C28;
// What each open BeginScissor found, so they can nest: an inner one is cut
// to the outer one, and each End puts back exactly what its Begin found.
struct ScissorWas {
    bool on;
    RECT rect;
};
std::vector<ScissorWas> g_scissors;

void BeginScissor(float left, float top, float right, float bottom) {
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(kD3DDevice);
    if (!device) return;
    DWORD on = FALSE;
    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &on);
    RECT was{};
    device->GetScissorRect(&was);
    g_scissors.push_back({on != FALSE, was});
    RECT r{static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right),
           static_cast<LONG>(bottom)};
    if (on) {
        r.left = std::max(r.left, was.left);
        r.top = std::max(r.top, was.top);
        r.right = std::min(r.right, was.right);
        r.bottom = std::min(r.bottom, was.bottom);
        if (r.right < r.left) r.right = r.left;
        if (r.bottom < r.top) r.bottom = r.top;
    }
    device->SetScissorRect(&r);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
}

void EndScissor() {
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(kD3DDevice);
    if (!device || g_scissors.empty()) return;
    const ScissorWas was = g_scissors.back();
    g_scissors.pop_back();
    device->SetScissorRect(&was.rect);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, was.on ? TRUE : FALSE);
}

void Flush() {
    game::FlushText();
}

std::string Printable(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if (c == '~') {
            out.push_back('-');
        } else if (c == '*') {
            // The game's font has no '*', and asking it for one hangs the
            // text code; an 'x' stands in.
            out.push_back('x');
        } else if (c >= 32 && c < 127) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

}  // namespace sprite
