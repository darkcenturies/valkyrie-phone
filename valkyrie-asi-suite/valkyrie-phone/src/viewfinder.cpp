#include "viewfinder.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "sprite.h"

namespace viewfinder {
namespace {

// TheCamera, and the mirror camera it keeps beside its own
// (gta-reversed Camera.h; m_pRwCamera at +0x954 is the well-known
// 0xB6F97C, and the layout after it is counted from there).
constexpr uintptr_t kTheCamera = 0xB6F028;
constexpr size_t kMirrorActive = 0x970;      // bool m_bMirrorActive
constexpr size_t kMatMirrorInverse = 0xA94;  // CMatrix m_mMatMirrorInverse
constexpr size_t kMatMirror = 0xADC;         // CMatrix m_mMatMirror

// CMirrors (gta-reversed Mirrors.h).
constexpr uintptr_t kMirrorBuffer = 0xC7C71C;   // RwRaster* pBuffer
constexpr uintptr_t kMirrorZBuffer = 0xC7C720;  // RwRaster* pZBuffer
constexpr uintptr_t kTypeOfMirror = 0xC7C724;   // uint8, 1 a wall
constexpr uintptr_t kMirrorFlags = 0xC7C618;    // uint8
constexpr uintptr_t kD3dRestored = 0xC7C729;    // bool, set after a lost device

// The functions borrowed or held off while the viewfinder is on.
constexpr uintptr_t kDealWithMirror = 0x50B510;  // CCamera::DealWithMirrorBeforeConstructRenderList
constexpr uintptr_t kRenderMirrorBuffer = 0x726090;
constexpr uintptr_t kCreateBuffer = 0x7230A0;
constexpr uintptr_t kShutDown = 0x723050;

// Scene.m_pRwCamera, and the RwCamera and RwRaster fields read here.
constexpr uintptr_t kSceneCamera = 0xC1703C;
constexpr size_t kCameraViewWindow = 0x68;  // RwV2d

constexpr uintptr_t kRwTextureCreate = 0x7F37C0;
// RwCameraSetNearClipPlane, and RwCamera::nearPlane. On foot the game keeps
// its near plane far out; the phone's lens is a hand's width from CJ, so
// for the phone's render it is brought in, and put back after.
constexpr uintptr_t kSetNearClip = 0x7EE1D0;
constexpr uintptr_t kSetFarClip = 0x7EE2A0;  // RwCameraSetFarClipPlane
constexpr size_t kCameraNearPlane = 0x80, kCameraFarPlane = 0x84;
// Not too near: the depth buffer's precision goes by far over near, and a
// near plane a few centimetres out leaves too little for the rest - the road
// shows through a car's underside, one car through another. The far plane is
// kept to what a phone's lens shows, for the same reason.
constexpr float kLensNear = 0.2f, kLensFar = 500.0f;
float g_savedNear = 0.0f, g_savedFar = 0.0f;

// Lens() or Reflection(): how often the world is drawn for it, and how far.
bool g_lens = true;
int g_every = 1;
float g_reflectionFar = 500.0f;
unsigned g_frameCount = 0;

// The mirror's depth buffer is made as the device's own is: multisampled
// when the game runs with anti-aliasing. Its picture is a texture, which
// never is, and Direct3D 9 leaves depth undefined for a pair that differs -
// in the viewfinder things showed through walls, or not at all. The
// viewfinder renders with a depth buffer of its own, made to match the
// picture, instead; RenderWare sets the game's own back at its next render.
constexpr uintptr_t kDevice = 0xC97C28;  // RwD3D9's IDirect3DDevice9*
IDirect3DSurface9* g_depth = nullptr;
UINT g_depthSize[2] = {};
bool g_depthLogged = false;
bool g_depthStencil = false;

void OwnDepth() {
    auto* d = *reinterpret_cast<IDirect3DDevice9**>(kDevice);
    if (!d) return;
    IDirect3DSurface9 *target = nullptr, *depth = nullptr;
    if (FAILED(d->GetRenderTarget(0, &target)) || !target) return;
    D3DSURFACE_DESC t{}, z{};
    target->GetDesc(&t);
    target->Release();
    if (SUCCEEDED(d->GetDepthStencilSurface(&depth)) && depth) {
        depth->GetDesc(&z);
        depth->Release();
    }
    if (!g_depthLogged) {
        g_depthLogged = true;
        logfile::Line("viewfinder: picture %ux%u (samples %d), game's depth %ux%u format %d (samples %d)", t.Width,
                      t.Height, static_cast<int>(t.MultiSampleType), z.Width, z.Height, static_cast<int>(z.Format),
                      static_cast<int>(z.MultiSampleType));
    }
    if (g_depth && (g_depthSize[0] != t.Width || g_depthSize[1] != t.Height)) {
        g_depth->Release();
        g_depth = nullptr;
    }
    if (!g_depth) {
        g_depthStencil = SUCCEEDED(d->CreateDepthStencilSurface(t.Width, t.Height, D3DFMT_D24S8, t.MultiSampleType,
                                                                t.MultiSampleQuality, TRUE, &g_depth, nullptr));
        if (!g_depthStencil &&
            FAILED(d->CreateDepthStencilSurface(t.Width, t.Height, D3DFMT_D24X8, t.MultiSampleType,
                                                t.MultiSampleQuality, TRUE, &g_depth, nullptr))) {
            g_depth = nullptr;
            return;
        }
        g_depthSize[0] = t.Width;
        g_depthSize[1] = t.Height;
    }
    d->SetDepthStencilSurface(g_depth);
    d->Clear(0, nullptr, D3DCLEAR_ZBUFFER | (g_depthStencil ? D3DCLEAR_STENCIL : 0), 0, 1.0f, 0);
}

// The mirror render (CMirrors::BeforeMainRender) clears to black and draws
// only the scene: the game draws its sky before the scene, outside it, so a
// mirror shows none - black, with the far trees fading in over it like
// ghosts. Its call to RenderScene is found and sent through MirrorScene,
// which lays the game's own sky down first.
constexpr uintptr_t kBeforeMainRender = 0x727140;
constexpr uintptr_t kRenderScene = 0x53DF40;
constexpr uintptr_t kRenderSkyPolys = 0x714650;  // CClouds::RenderSkyPolys
constexpr uintptr_t kDefinedState = 0x734650;

// A CMatrix as it sits in memory: right, forward, up, position, each in a
// 16-byte row. The attach pointer after them is left alone.
struct Rows {
    float right[4], forward[4], up[4], pos[4];
};

struct Patch {
    uintptr_t at;
    size_t len;
    uint8_t saved[5];
    bool on;
};

Patch g_deal{kDealWithMirror, 5, {}, false};
Patch g_renderBuffer{kRenderMirrorBuffer, 1, {}, false};
Patch g_shutDown{kShutDown, 1, {}, false};

uintptr_t g_sceneCall = 0;  // the call in BeforeMainRender, once found
bool g_on = false;
bool g_failed = false;  // the rasters would not make; stay off until restarted
uintptr_t g_texture = 0;
int g_framesRendered = 0;
// The game frame the mirror's picture was last drawn on (CTimer's frame
// counter). The picture is only handed out on that frame: on any other the
// game may have freed the raster behind it, and Direct3D would be given a
// texture that no longer exists.
constexpr uintptr_t kFrameCounter = 0xB7CB4C;
unsigned g_renderedFrame = ~0u;

Rows g_matrix{}, g_inverse{};

bool Write(uintptr_t at, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(at), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(at), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
    return true;
}

bool Apply(Patch& p, const uint8_t* bytes) {
    if (p.on) return true;
    memcpy(p.saved, reinterpret_cast<const void*>(p.at), p.len);
    if (!Write(p.at, bytes, p.len)) return false;
    p.on = true;
    return true;
}

void Undo(Patch& p) {
    if (!p.on) return;
    Write(p.at, p.saved, p.len);
    p.on = false;
}

void Store(uintptr_t at, const Rows& m) {
    auto* out = reinterpret_cast<Rows*>(at);
    for (int i = 0; i < 3; ++i) {
        out->right[i] = m.right[i];
        out->forward[i] = m.forward[i];
        out->up[i] = m.up[i];
        out->pos[i] = m.pos[i];
    }
}

// Stands in for CCamera::DealWithMirrorBeforeConstructRenderList while the
// viewfinder is on: whatever mirror the game found, the mirror camera is the
// phone's. A __thiscall taking (bool, CVector, float, CMatrix*) - six words
// on the stack, which __fastcall pops the same way.
void __fastcall DealWithMirror(uint8_t* camera, void*, int, float, float, float, float, void*) {
    // A reflection is not drawn every frame: between, no mirror at all this
    // frame, and the texture keeps the last picture.
    if (!g_lens && g_every > 1 && (g_frameCount++ % static_cast<unsigned>(g_every)) != 0) {
        *reinterpret_cast<uint8_t*>(kTypeOfMirror) = 0;
        camera[kMirrorActive] = 0;
        return;
    }
    *reinterpret_cast<uint8_t*>(kTypeOfMirror) = 1;
    *reinterpret_cast<uint8_t*>(kMirrorFlags) = 0;
    // Outside a mirror room the game frees its buffer every frame - inline,
    // so holding off CMirrors::ShutDown does not stop it. Made again here,
    // just before the render that uses it, and the phone's texture pointed
    // at whichever buffer is current.
    if (!*reinterpret_cast<void**>(kMirrorBuffer) || !*reinterpret_cast<void**>(kMirrorZBuffer)) {
        reinterpret_cast<void(__cdecl*)()>(kCreateBuffer)();
    }
    void* buffer = *reinterpret_cast<void**>(kMirrorBuffer);
    if (!buffer || !*reinterpret_cast<void**>(kMirrorZBuffer)) {
        // No buffer: no mirror this frame, rather than a render into nothing.
        *reinterpret_cast<uint8_t*>(kTypeOfMirror) = 0;
        camera[kMirrorActive] = 0;
        g_framesRendered = 0;
        return;
    }
    if (g_texture) *reinterpret_cast<void**>(g_texture) = buffer;  // RwTexture::raster
    camera[kMirrorActive] = 1;
    Store(reinterpret_cast<uintptr_t>(camera) + kMatMirror, g_matrix);
    Store(reinterpret_cast<uintptr_t>(camera) + kMatMirrorInverse, g_inverse);
    ++g_framesRendered;
    if (const uintptr_t rw = *reinterpret_cast<const uintptr_t*>(kSceneCamera)) {
        // Not put back last time (no mirror render came): the saved one stands.
        if (g_savedNear <= 0.0f) {
            g_savedNear = *reinterpret_cast<const float*>(rw + kCameraNearPlane);
            g_savedFar = *reinterpret_cast<const float*>(rw + kCameraFarPlane);
        }
        // The lens a hand's width from CJ needs its near plane brought in; a
        // reflection is seen from the game camera, whose own is right.
        if (g_lens) reinterpret_cast<void*(__cdecl*)(uintptr_t, float)>(kSetNearClip)(rw, std::min(kLensNear, g_savedNear));
        // A reflection keeps the game's far plane (farPlane 0): the game cuts
        // its own draw list to it too.
        if (g_lens || g_reflectionFar > 0.0f)
            reinterpret_cast<void*(__cdecl*)(uintptr_t, float)>(kSetFarClip)(
                rw, std::min(g_lens ? kLensFar : g_reflectionFar, g_savedFar));
    }
}

void __cdecl MirrorScene() {
    if (g_on) OwnDepth();
    if (g_on) {
        // The sky is laid out round TheCamera's own matrix (CPlaceable's,
        // at +0x14): for these few calls it is the viewfinder's.
        auto* matrix = *reinterpret_cast<Rows**>(kTheCamera + 0x14);
        Rows saved{};
        if (matrix) {
            saved = *matrix;
            Store(reinterpret_cast<uintptr_t>(matrix), g_matrix);
        }
        reinterpret_cast<void(__cdecl*)()>(kRenderSkyPolys)();
        if (matrix) *matrix = saved;
        reinterpret_cast<void(__cdecl*)()>(kDefinedState)();
    }
    reinterpret_cast<void(__cdecl*)()>(kRenderScene)();
    if (g_on) g_renderedFrame = *reinterpret_cast<const unsigned*>(kFrameCounter);
    // No mirror for the game's own picture. RenderScene leaves the clouds
    // and the sun out while CMirrors::TypeOfMirror is set (the game's
    // mirrors are all indoors), and it is set until the next frame's
    // DealWithMirror otherwise - which took the sky's clouds and sun away
    // whenever the phone was out.
    *reinterpret_cast<uint8_t*>(kTypeOfMirror) = 0;
    // The game's own near plane again, before its own render.
    const uintptr_t rw = *reinterpret_cast<const uintptr_t*>(kSceneCamera);
    if (g_on && rw && g_savedNear > 0.0f) {
        reinterpret_cast<void*(__cdecl*)(uintptr_t, float)>(kSetNearClip)(rw, g_savedNear);
        if (g_savedFar > 0.0f) reinterpret_cast<void*(__cdecl*)(uintptr_t, float)>(kSetFarClip)(rw, g_savedFar);
        g_savedNear = g_savedFar = 0.0f;
    }
}

// Point the mirror render's RenderScene call at MirrorScene (or back).
void SkyCall(bool ours) {
    if (!g_sceneCall) {
        static bool looked = false;
        if (looked) return;
        looked = true;
        for (uintptr_t at = kBeforeMainRender; at < kBeforeMainRender + 0x140; ++at) {
            if (*reinterpret_cast<const uint8_t*>(at) != 0xE8) continue;
            const uintptr_t target = at + 5 + *reinterpret_cast<const int32_t*>(at + 1);
            if (target == kRenderScene) {
                g_sceneCall = at;
                break;
            }
        }
        if (!g_sceneCall) {
            logfile::Line("viewfinder: the mirror render's scene call was not found - no sky in it");
            return;
        }
    }
    const uintptr_t target = ours ? reinterpret_cast<uintptr_t>(&MirrorScene) : kRenderScene;
    const int32_t rel = static_cast<int32_t>(target - (g_sceneCall + 5));
    Write(g_sceneCall + 1, &rel, sizeof rel);
}

// The texture that lets CSprite2d draw the game's mirror buffer. It only
// borrows the raster: the raster is taken out again before the texture is
// destroyed, so the game's buffer is never freed by us.
void Release() {
    if (g_texture) {
        *reinterpret_cast<void**>(g_texture) = nullptr;  // RwTexture::raster
        sprite::DestroyTexture(g_texture);
        g_texture = 0;
    }
}

void Stop() {
    if (!g_on) return;
    Release();
    Undo(g_deal);
    Undo(g_renderBuffer);
    Undo(g_shutDown);
    SkyCall(false);
    // The game looks again next frame; outside a mirror room it frees its
    // buffer itself.
    *reinterpret_cast<uint8_t*>(kTypeOfMirror) = 0;
    *reinterpret_cast<uint8_t*>(kTheCamera + kMirrorActive) = 0;
    g_on = false;
    logfile::Line("viewfinder: off");
}

bool Start() {
    if (g_on) return true;
    if (g_failed) return false;
    // The game's own mirror buffer, made the way it makes it for a mirror.
    reinterpret_cast<void(__cdecl*)()>(kCreateBuffer)();
    void* buffer = *reinterpret_cast<void**>(kMirrorBuffer);
    if (!buffer || !*reinterpret_cast<void**>(kMirrorZBuffer)) {
        logfile::Line("viewfinder: the game would not make its mirror buffer");
        g_failed = true;
        return false;
    }
    g_texture = reinterpret_cast<uintptr_t(__cdecl*)(void*)>(kRwTextureCreate)(buffer);
    if (!g_texture) {
        logfile::Line("viewfinder: no texture for the mirror buffer");
        g_failed = true;
        return false;
    }
    g_framesRendered = 0;

    uint8_t jump[5] = {0xE9};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&DealWithMirror)) -
                        static_cast<int32_t>(kDealWithMirror + 5);
    memcpy(jump + 1, &rel, sizeof rel);
    const uint8_t ret = 0xC3;
    g_on = true;
    // ShutDown is held off so the buffer is not freed every frame outside a
    // mirror room; CreateBuffer is left alone (it does nothing while there
    // is one).
    if (!Apply(g_deal, jump) || !Apply(g_renderBuffer, &ret) || !Apply(g_shutDown, &ret)) {
        logfile::Line("viewfinder: could not patch the mirror code");
        Stop();
        g_failed = true;
        return false;
    }
    SkyCall(true);
    logfile::Line("viewfinder: on, %dx%d", *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(buffer) + 0xC),
                  *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(buffer) + 0x10));
    return true;
}

}  // namespace

void Aim(const Vec& eye, const Vec& target) {
    float f[3] = {target.x - eye.x, target.y - eye.y, target.z - eye.z};
    float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len < 1e-4f) return;
    for (float& v : f) v /= len;
    // As CMirrors::BuildCamMatrix makes one: right is world-up across
    // forward, up is forward across right.
    float r[3] = {-f[1], f[0], 0.0f};
    len = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    if (len < 1e-4f) {
        r[0] = 1.0f;
        r[1] = 0.0f;
    } else {
        r[0] /= len;
        r[1] /= len;
    }
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    const float p[3] = {eye.x, eye.y, eye.z};
    Rows m{};
    for (int i = 0; i < 3; ++i) {
        m.right[i] = r[i];
        m.forward[i] = f[i];
        m.up[i] = u[i];
        m.pos[i] = p[i];
    }
    // The inverse of a rotation and a move: the rotation turned over, and
    // the move undone in the turned frame.
    Rows inv{};
    for (int i = 0; i < 3; ++i) {
        inv.right[i] = (i == 0 ? r : i == 1 ? f : u)[0];
        inv.forward[i] = (i == 0 ? r : i == 1 ? f : u)[1];
        inv.up[i] = (i == 0 ? r : i == 1 ? f : u)[2];
    }
    inv.pos[0] = -(p[0] * r[0] + p[1] * r[1] + p[2] * r[2]);
    inv.pos[1] = -(p[0] * f[0] + p[1] * f[1] + p[2] * f[2]);
    inv.pos[2] = -(p[0] * u[0] + p[1] * u[1] + p[2] * u[2]);
    g_matrix = m;
    g_inverse = inv;
}

void Update(bool on) {
    if (!on) {
        Stop();
        return;
    }
    if (!Start()) return;
    // A lost device: the game rebuilds its mirror buffer, so this starts
    // over with the new one.
    if (*reinterpret_cast<const bool*>(kD3dRestored)) {
        logfile::Line("viewfinder: device restored, starting again");
        Stop();
        reinterpret_cast<void(__cdecl*)()>(kShutDown)();
        Start();
    }
}

void Lens() {
    g_lens = true;
    g_every = 1;
}

void Reflection(int every, float farPlane) {
    g_lens = false;
    g_every = std::max(1, every);
    g_reflectionFar = farPlane;
}

uintptr_t Texture() {
    // The first frame's picture is whatever the memory held.
    const bool drawnThisFrame = g_renderedFrame == *reinterpret_cast<const unsigned*>(kFrameCounter);
    return g_on && g_framesRendered > 1 && drawnThisFrame ? g_texture : 0;
}

void ReleaseDeviceObjects() {
    if (g_depth) {
        g_depth->Release();
        g_depth = nullptr;
    }
}

bool ViewWindow(float out[2]) {
    if (const uintptr_t camera = *reinterpret_cast<const uintptr_t*>(kSceneCamera)) {
        const float* window = reinterpret_cast<const float*>(camera + kCameraViewWindow);
        if (window[0] > 0.01f && window[1] > 0.01f && window[0] < 10.0f && window[1] < 10.0f) {
            out[0] = window[0];
            out[1] = window[1];
            return true;
        }
    }
    return false;
}

float Aspect() {
    if (const uintptr_t camera = *reinterpret_cast<const uintptr_t*>(kSceneCamera)) {
        const float* window = reinterpret_cast<const float*>(camera + kCameraViewWindow);
        if (window[0] > 0.0f && window[1] > 0.0f) {
            const float a = window[0] / window[1];
            if (a > 0.8f && a < 4.0f) return a;
        }
    }
    return 16.0f / 9.0f;
}

}  // namespace viewfinder
