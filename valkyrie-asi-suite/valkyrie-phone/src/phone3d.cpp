#include "phone3d.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace phone3d {

const char* const kPartTextures[kParts] = {"vp_front", "vp_screen", "vp_back", "vp_chrome", "vp_black", "vp_lens",
                                          "vp_button"};

namespace {

template <class T>
void Release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// --- The model --------------------------------------------------------------

struct Vertex {
    float pos[3], normal[3], uv[2];
};

struct Range {
    UINT first = 0, count = 0;  // indices
};

IDirect3DVertexBuffer9* g_vertices = nullptr;
IDirect3DIndexBuffer9* g_indices = nullptr;
IDirect3DVertexDeclaration9* g_decl = nullptr;
UINT g_vertexCount = 0;
Range g_ranges[kParts];
float g_pivot[3] = {}, g_radius = 0.0f;       // the middle of the whole, and how far it reaches
float g_screenMid[3] = {}, g_screenW = 0.0f;  // the screen's middle and width, in the model's units
float g_lo[3] = {}, g_hi[3] = {};

// A RenderWare clump as build-phone-model.py writes one: one geometry, with
// positions, one set of texture coordinates and normals, its materials named
// by their textures.
bool Parse(const std::vector<uint8_t>& b, std::vector<Vertex>& verts, std::vector<uint16_t> (&byPart)[kParts]) {
    auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, &b[at], 4); return v; };
    auto u16 = [&](size_t at) { uint16_t v; memcpy(&v, &b[at], 2); return v; };
    auto f32 = [&](size_t at) { float v; memcpy(&v, &b[at], 4); return v; };
    size_t geometry = 0, geometryEnd = 0;
    std::vector<std::string> textures;
    // Walk the chunks in the order they are written (the materials' order is
    // what the triangles' material numbers count), into the ones that hold
    // others.
    bool fits = true;
    auto walk = [&](auto& self, size_t at, size_t end) -> void {
        while (at + 12 <= end) {
            const uint32_t type = u32(at), size = u32(at + 4);
            const size_t body = at + 12;
            if (body + size > end) {
                fits = false;
                return;
            }
            if (type == 0x0F) {
                geometry = body;
                geometryEnd = body + size;
            }
            if (type == 0x06) {  // a texture: its struct, then its name
                const size_t name = body + 12 + u32(body + 4);
                if (name + 12 <= body + size && u32(name) == 0x02) {
                    textures.emplace_back(reinterpret_cast<const char*>(&b[name + 12]));
                }
            } else if (type == 0x10 || type == 0x1A || type == 0x0F || type == 0x08 || type == 0x07) {
                self(self, body, body + size);
            }
            at = body + size;
        }
    };
    walk(walk, 0, b.size());
    if (!fits) return false;
    if (!geometry || u32(geometry) != 0x01) return false;
    size_t p = geometry + 12;
    const uint32_t flags = u32(p), tris = u32(p + 4), count = u32(p + 8);
    p += 16;
    if (!(flags & 0x10) || !(flags & 0x04) || count == 0 || count > 65535) return false;
    if (flags & 0x08) p += 4 * count;  // prelit colours
    const uint32_t uvSets = ((flags >> 16) & 0xFF) ? ((flags >> 16) & 0xFF) : 1;
    const size_t uvAt = p;
    p += 8 * count * uvSets;
    const size_t triAt = p;
    p += 8 * tris + 16;  // then the bounding sphere
    if (p + 8 > geometryEnd || !u32(p) || !u32(p + 4)) return false;
    p += 8;
    const size_t posAt = p, normalAt = p + 12 * count;
    if (normalAt + 12 * count > geometryEnd) return false;
    verts.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        for (int k = 0; k < 3; ++k) {
            verts[i].pos[k] = f32(posAt + 12 * i + 4 * k);
            verts[i].normal[k] = f32(normalAt + 12 * i + 4 * k);
        }
        verts[i].uv[0] = f32(uvAt + 8 * i);
        verts[i].uv[1] = f32(uvAt + 8 * i + 4);
    }
    for (uint32_t t = 0; t < tris; ++t) {
        // On disk: vertIndex[1], vertIndex[0], matIndex, vertIndex[2].
        const uint16_t v1 = u16(triAt + 8 * t), v0 = u16(triAt + 8 * t + 2), m = u16(triAt + 8 * t + 4),
                       v2 = u16(triAt + 8 * t + 6);
        if (v0 >= count || v1 >= count || v2 >= count || m >= textures.size()) return false;
        int part = -1;
        for (int k = 0; k < kParts; ++k) {
            if (_stricmp(textures[m].c_str(), kPartTextures[k]) == 0) part = k;
        }
        if (part < 0) return false;
        byPart[part].insert(byPart[part].end(), {v0, v1, v2});
    }
    return true;
}

// --- The drawing ------------------------------------------------------------

using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT,
                                   UINT, ID3DBlob**, ID3DBlob**);

ID3DBlob* Compile(const char* source, const char* profile) {
    static CompileFn compile = [] {
        HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
        return dll ? reinterpret_cast<CompileFn>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    }();
    if (!compile) return nullptr;
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = compile(source, strlen(source), "phone3d", nullptr, nullptr, "main", profile,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) {
        OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        errors->Release();
    }
    return SUCCEEDED(hr) ? code : nullptr;
}

// Each point of the model, turned about its middle and seen from in front of
// the game's screen, a little in perspective: at rest the screen's own plane
// lands exactly on Pose::screen, pixel for pixel.
const char kVertexShader[] = R"(
float4 r0 : register(c0);
float4 r1 : register(c1);
float4 r2 : register(c2);
float4 pivot : register(c3);   // xyz, and pixels per model unit
float4 rel : register(c4);     // the middle from the screen's middle, and the focal length in pixels
float4 centre : register(c5);  // the screen's middle in pixels, and 1 / the target's width and height
float4 depth : register(c6);   // how far in pixels the model can reach from its middle
struct In { float3 pos : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct Out { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; };
Out main(In i) {
    Out o;
    float3 p = i.pos - pivot.xyz;
    float3 r = float3(dot(r0.xyz, p), dot(r1.xyz, p), dot(r2.xyz, p)) + rel.xyz;
    // The model's +x is the viewer's left, +z up, +y toward the viewer.
    float X = -r.x * pivot.w, Y = -r.z * pivot.w, Z = -r.y * pivot.w;
    float w = (rel.w + Z) / rel.w;
    float px = centre.x * w + X, py = centre.y * w + Y;
    // Depth as a true perspective has it, even in 1 / w, so two surfaces a
    // hair apart stay in order across a whole triangle at any angle.
    float wNear = 1 - depth.x / rel.w, wFar = 1 + depth.x / rel.w;
    float zn = saturate((1 / wNear - 1 / w) / (1 / wNear - 1 / wFar));
    o.pos = float4(px * centre.z * 2 - w, w - py * centre.w * 2, zn * w, w);
    float3 n = float3(dot(r0.xyz, i.n), dot(r1.xyz, i.n), dot(r2.xyz, i.n));
    o.n = float3(-n.x, n.z, n.y);
    o.uv = i.uv;
    return o;
}
)";

// The handset's own light model (phone.cpp's kLightingShader), per part:
// Lambert diffuse, a normalised Blinn-Phong highlight and Schlick's Fresnel,
// the surroundings along the reflected ray. The screen gives its own light
// under the same glass as the rest of the front.
const char kPixelShader[] = R"(
sampler2D albedo : register(s0);
sampler2D env : register(s1);
float4 lightDir : register(c0);
float4 sunColour : register(c1);
float4 ambient : register(c2);
float4 envParams : register(c3);  // turn, tilt, strength, live
float4 eye : register(c4);        // the eye: the game's screen's middle, and its distance, in pixels
float4 m : register(c5);          // gloss, smoothness, metal, how much it mirrors
float4 lit : register(c6);        // 1 for the screen, and its brightness
float4 face : register(c7);       // the phone on the picture, in pixels: left, top, width, height
float4 display : register(c8);    // the screen texture's size in pixels, and how strongly its LCD shows
float4 axisRight : register(c9);  // the screen's own right, up and out, as the viewer sees them
float4 axisUp : register(c10);
float4 axisOut : register(c11);
float4 screenOnFront : register(c12);  // where the screen is on the front: left, top, right, bottom
sampler2D glass : register(s2);        // what lies on the glass, premultiplied, over the whole front
sampler2D wear : register(s3);         // scratches on the glass, over the whole front
float4 worn : register(c13);
float4 liveWindow : register(c14);  // the live render's camera: tan of half its view across, up           // on (0: off), how wide the sun catches them, and how bright in the sun and in what the glass mirrors
float3 schlick(float3 f0, float3 f90, float c) { float k = 1 - c, k2 = k * k; return f0 + (f90 - f0) * (k2 * k2 * k); }
// The rain and cracks lying on the front glass, over what is under it
// (lit.z: 1 for the parts that are the front).
float4 OnGlass(float4 c, float2 front) {
    if (lit.z < 0.5) return c;
    float4 g = tex2D(glass, front);
    return float4(c.rgb * (1 - g.a) + g.rgb, 1);
}
// Fine scratches only show where light catches them: across a broad patch
// round the sun's highlight (a groove scatters it wide), and a little where
// the glass mirrors something bright. In the dark, or away from the light,
// the glass looks clean.
float3 Scratches(float2 front, float3 n, float3 h, float ndl, float3 sun, float3 around) {
    if (worn.x <= 0 || lit.w < 0.5) return 0;
    float s = tex2D(wear, front).r;
    float catchSun = pow(saturate(dot(n, h)), worn.y) * ndl;
    float3 seen = sun * catchSun * worn.z + (around + ambient.rgb * 0.3) * worn.w;
    return s * seen;
}
float4 main(float2 uv : TEXCOORD0, float3 n : TEXCOORD1, float2 vpos : VPOS) : COLOR {
    n = normalize(n);
    // The front is one flat sheet of glass, but its mesh's normals are
    // smoothed into the rounded rim, which bent what it mirrors into swirls:
    // the glass takes the phone's own facing instead.
    if (lit.w > 0.5 && dot(n, axisOut.xyz) > 0.3) n = axisOut.xyz;
    float4 a = tex2D(albedo, uv);
    float3 v = normalize(float3(eye.x - vpos.x, vpos.y - eye.y, eye.z));
    float3 l = normalize(lightDir.xyz);
    float3 h = normalize(l + v);
    float ndl = saturate(dot(n, l)), ndv = saturate(dot(n, v));
    float power = exp2(1 + m.g * 12);
    float3 f0 = lerp(float3(0.06, 0.06, 0.06), a.rgb, m.b);
    float3 sun = sunColour.rgb * sunColour.a;
    float3 spec = schlick(f0, 1, saturate(dot(h, v))) * (pow(saturate(dot(n, h)), power) * min((power + 8) / 8, 40) * ndl * m.r);
    float3 r = reflect(-v, n);
    // The studio picture turns with the camera.
    // Live, the reflected ray is looked up in the render the way that render
    // was made: a camera at the game camera looking back past the viewer,
    // so across is -x (it faces the other way) and the picture is laid out
    // by the ray's slope, not its angle - flat glass gives a flat picture,
    // straight edges straight. Rays running on into the phone (its curved
    // steel seen side-on) are held to a shallow slope.
    float2 euv;
    if (envParams.w > 0.5) {
        const float depth = max(r.z, 0.2);
        euv = float2(0.5 - 0.5 * (r.x / depth) / liveWindow.x, 0.5 - 0.5 * (r.y / depth) / liveWindow.y);
    } else {
        euv = float2(0.5 + 0.5 * r.x + envParams.x, 0.5 - 0.5 * r.y + envParams.y);
    }
    float3 around = tex2D(env, euv).rgb * (envParams.w > 0.5 ? float3(1, 1, 1) : ambient.rgb + sun * 0.6);
    if (envParams.w > 0.5) {
        // The live render only saw so far round: a ray past its edge (the
        // curved rim, side-on) would read the picture folded back on itself -
        // mirrored copies of its edges in bright fringes. Past the edge it
        // fades to the picture's overall colour instead.
        float2 past = abs(euv - 0.5) * 2;
        float inside = saturate((1.1 - max(past.x, past.y)) / 0.25);
        float3 overall = (tex2D(env, float2(0.5, 0.5)).rgb + tex2D(env, float2(0.2, 0.3)).rgb + tex2D(env, float2(0.8, 0.3)).rgb +
                          tex2D(env, float2(0.2, 0.7)).rgb + tex2D(env, float2(0.8, 0.7)).rgb) * 0.2;
        around = lerp(overall, tex2D(env, saturate(euv)).rgb, inside);
    }
    around = lerp(ambient.rgb, around, m.g);
    float3 fe = schlick(f0, max(m.g, f0), ndv);
    // The screen and the black print round it are under one sheet of glass,
    // so both mirror the world alike; a lit picture simply outshines it.
    float3 shine = spec * sun + fe * around * (m.a * envParams.z * (1 + (1 - m.b) * 1.5));
    shine = 1 - exp(-shine);
    float2 front = lit.x > 0.5 ? lerp(screenOnFront.xy, screenOnFront.zw, uv) : uv;
    shine += Scratches(front, n, h, ndl, sun, around);
    if (lit.x > 0.5) {
        // Built as the original iPhone is: the display, a hair of air, then
        // the glass, black-printed round the opening. Seen at an angle, the
        // print's edge lies over the display's own a little way in (the gap
        // times the tangent of the angle), so the picture's edge shifts as
        // the phone turns; the gap is always a touch dark. In the display's
        // own points, 320 x 480.
        float3 vl = float3(dot(v, axisRight.xyz), dot(v, axisUp.xyz), dot(v, axisOut.xyz));
        float2 pt = uv * float2(320, 480);
        // The gap is thin: a couple of points at most, even side-on, or
        // the print reads as a black band down the side.
        float2 shift = float2(-vl.x, vl.y) / max(vl.z, 0.5) * 1.2;
        float2 lo = pt - max(shift, 0);
        float2 hi = float2(320, 480) - pt - max(-shift, 0);
        float edge = min(min(lo.x, lo.y), min(hi.x, hi.y));
        float cover = lerp(0.55, 1, saturate(edge / 1.5)) * saturate(edge + 1);
        // The LCD: each pixel three upright stripes, red, green and blue,
        // with a fine dark line between rows. Drawn big (a pixel of the
        // display two or more of the picture), it follows the display's own
        // pixels, each stripe averaged over what a pixel of the picture
        // covers. At the phone's usual size a display pixel is about one of
        // the picture, where a grid of the display's would beat against the
        // picture's own - a shimmer while the phone moves, a line across it
        // once it stops - so there the grid is laid on the picture's own
        // pixels instead: a stripe to each, a row line every third. It
        // cannot beat against itself, and reads as the same fine grille.
        float2 tc = uv * display.xy;
        float per = max(length(ddx(tc)), length(ddy(tc)));
        float big = saturate((0.75 - per) / 0.25);
        float span = max(per * 0.5, 0.001);
        float3 tints[3] = {float3(1, 0.8, 0.8), float3(0.8, 1, 0.8), float3(0.8, 0.8, 1)};
        float3 fine = float3(0, 0, 0);
        for (int k = 0; k < 3; ++k) {
            float2 x = (tc.x + float2(-span, span)) / 3;
            float2 upTo = floor(x) + saturate(frac(x) * 3 - k);
            fine += (upTo.y - upTo.x) / (2 * span) * tints[k];
        }
        float2 y = tc.y + float2(-span, span);
        float2 dark = floor(y) * 0.34 + max(frac(y) - 0.66, 0);
        fine *= 1 - 0.14 * saturate((dark.y - dark.x) / (2 * span));
        float2 px = floor(vpos);
        float column = px.x - 3 * floor(px.x / 3);
        float3 coarse = (column < 0.5 ? tints[0] : column < 1.5 ? tints[1] : tints[2]) * 1.1;
        coarse *= px.y - 3 * floor(px.y / 3) > 1.5 ? 0.86 : 1;
        float3 stripe = lerp(coarse, fine, big);
        float lcd = display.z;
        float3 picture = a.rgb * lit.y * cover * lerp(float3(1, 1, 1), stripe, lcd);
        // A dark display is as black as the print round it - vp_front's 10,10,12 -
        // and takes the day's light the same, so the opening does not show as a hole.
        picture += (1 - fe) * float3(0.039, 0.039, 0.047) * (ambient.rgb + sun * ndl);
        return OnGlass(float4(saturate(picture + shine), 1), front);
    }
    float3 diffuse = (1 - fe) * (1 - m.b) * a.rgb * (ambient.rgb + sun * ndl);
    return OnGlass(float4(saturate(diffuse + shine), 1), uv);
}
)";

// The Valkyrie ink: the trainer's outline (valkyrie-trainer PreviewInk.h),
// ink laid round the outside of the shape, as wide as a slow breathing noise
// makes it. The taps are in pixels and scaled here.
const char kInkShader[] = R"(
sampler2D source : register(s0);
sampler2D taps : register(s1);
float4 params : register(c0);  // the source's width and height, the time, and the ink's scale (0: none)
float4 inkColour : register(c1);
float hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123); }
float noise(float2 p) { float2 i = floor(p), f = frac(p); f = f * f * (3 - 2 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), f.x), lerp(hash(i + float2(0, 1)), hash(i + 1), f.x), f.y); }
float4 main(float2 uv : TEXCOORD0) : COLOR {
    // The phone, its edge softened by the multisampling: premultiplied, as
    // the resolve leaves it (the samples outside it are clear).
    float4 pixel = tex2D(source, uv);
    if (pixel.a > 0.998) return float4(pixel.rgb / pixel.a, 1);
    if (params.w <= 0) return pixel.a > 0 ? float4(pixel.rgb / pixel.a, pixel.a) : 0;
    float distance = 1e6;
    [loop] for (int i = 0; i < 96; ++i) {
        float4 tap = tex2Dlod(taps, float4((i + .5) / 96.0, .5, 0, 0));
        float2 at = uv + tap.xy * params.w / params.xy;
        if (all(at >= 0) && all(at <= 1) && tex2Dlod(source, float4(at, 0, 0)).a > 0.5) distance = min(distance, tap.z * params.w);
    }
    // A pixel the edge only partly covers has ink under it, so the soft edge
    // runs into the ink with nothing showing between them.
    if (pixel.a > 0) distance = 0;
    if (distance > 99999) return 0;
    float2 p = uv * params.xy / params.w;
    float breathe = noise(float2(p.x, params.y / params.w - p.y) / 55 + float2(params.z * .62, -params.z * .37));
    float edge = 5.4 * params.w * (.68 + 1.35 * breathe);
    float t = saturate((distance - (edge - .9 * params.w)) / (1.8 * params.w));
    float ink = 1 - t * t * (3 - 2 * t);
    ink = (ink >= .02 ? ink : 0) * inkColour.a;
    // The phone over the ink.
    float alpha = pixel.a + (1 - pixel.a) * ink;
    if (alpha <= 0) return 0;
    return float4((pixel.rgb + (1 - pixel.a) * ink * inkColour.rgb) / alpha, alpha);
}
)";

IDirect3DVertexShader9* g_vs = nullptr;
IDirect3DPixelShader9* g_ps = nullptr;
IDirect3DPixelShader9* g_ink = nullptr;
IDirect3DTexture9* g_taps = nullptr;
bool g_shadersFailed = false;

// In the device's own memory: given back before a reset.
IDirect3DTexture9* g_screen = nullptr;
int g_screenSize[2] = {};
IDirect3DTexture9* g_glass = nullptr;
int g_glassSize[2] = {};
IDirect3DTexture9* g_target = nullptr;
IDirect3DSurface9* g_depth = nullptr;
// Drawn multisampled, for smooth edges, and resolved into g_target.
IDirect3DSurface9* g_msaa = nullptr;
UINT g_targetSize[2] = {};

IDirect3DSurface9* g_savedTarget = nullptr;
IDirect3DSurface9* g_savedDepth = nullptr;
D3DVIEWPORT9 g_savedViewport{};

float g_bounds[4] = {};
float g_corners[6] = {};
bool g_haveCorners = false;

bool Shaders(IDirect3DDevice9* d) {
    if (g_vs && g_ps && g_ink && g_taps) return true;
    if (g_shadersFailed) return false;
    g_shadersFailed = true;
    ID3DBlob* vs = Compile(kVertexShader, "vs_3_0");
    ID3DBlob* ps = Compile(kPixelShader, "ps_3_0");
    ID3DBlob* ink = Compile(kInkShader, "ps_3_0");
    bool ok = vs && ps && ink &&
              SUCCEEDED(d->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()), &g_vs)) &&
              SUCCEEDED(d->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()), &g_ps)) &&
              SUCCEEDED(d->CreatePixelShader(static_cast<const DWORD*>(ink->GetBufferPointer()), &g_ink));
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (ink) ink->Release();
    // The taps, as the trainer lays them: sixteen directions, six steps out,
    // weighted a little toward the top left.
    if (ok && SUCCEEDED(d->CreateTexture(96, 1, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &g_taps, nullptr))) {
        float taps[96][4]{};
        int at = 0;
        for (int i = 0; i < 16; ++i) {
            const float a = i * 0.3926991f, dx = std::cos(a), dy = std::sin(a);
            const float weight = std::max(0.25f, 1 + 0.30f * (dx * -0.55f + dy * -0.83f));
            for (int r = 1; r <= 6; ++r) {
                const float t = r / 6.0f, px = 1 + (5.4f * 2.15f - 1) * t * t;
                taps[at][0] = std::floor(0.5f + dx * px);
                taps[at][1] = std::floor(0.5f - dy * px);
                taps[at++][2] = px / weight;
            }
        }
        D3DLOCKED_RECT lock{};
        if (SUCCEEDED(g_taps->LockRect(0, &lock, nullptr, 0))) {
            memcpy(lock.pBits, taps, sizeof taps);
            g_taps->UnlockRect(0);
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }
    if (!ok) {
        Release(g_vs);
        Release(g_ps);
        Release(g_ink);
        Release(g_taps);
        return false;
    }
    g_shadersFailed = false;
    return true;
}

// The front glass's wear, over the whole front as body.png lays it out
// (512 x 1024): fine scratches and pits, most of them round the corners
// where a phone is held and set down, a few along the ends from pockets
// and a few anywhere, with a faint haze in each corner. The same seed
// always gives the same glass. Kept in managed memory, so a reset keeps it.
IDirect3DTexture9* g_wear = nullptr;
uint32_t g_wearMade = 0;
bool g_wearStrong = false;
float g_wearAmount = -1;

// Strong: plenty of it, plain to see in the sun; subtle: fewer, finer and
// fainter, only just there.
bool Wear(IDirect3DDevice9* d, float amount, uint32_t seed, bool strong) {
    if (g_wear && g_wearMade == seed && g_wearAmount == amount && g_wearStrong == strong) return true;
    Release(g_wear);
    constexpr int W = 512, H = 1024, kLevels = 4;
    if (FAILED(d->CreateTexture(W, H, kLevels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_wear, nullptr))) return false;
    g_wearMade = seed;
    g_wearAmount = amount;
    g_wearStrong = strong;
    uint32_t state = seed ? seed : 1;
    auto rnd = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (state & 0xFFFFFF) / 16777216.0f;
    };
    std::vector<float> v(W * H, 0.0f);
    // The glass: inside the steel rim, its corners rounded as the rim is.
    auto onGlass = [&](float x, float y) {
        const float in = 7, r = 58;
        const float cx = std::clamp(x, in + r, W - in - r), cy = std::clamp(y, in + r, H - in - r);
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r && x > in && y > in && x < W - in && y < H - in;
    };
    auto put = [&](float x, float y, float a) {
        if (!onGlass(x, y)) return;
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
        const float fx = x - ix, fy = y - iy;
        const float w4[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        for (int k = 0; k < 4; ++k) {
            const int px = ix + (k & 1), py = iy + (k >> 1);
            if (px < 0 || py < 0 || px >= W || py >= H) continue;
            float& t = v[py * W + px];
            t = std::max(t, a * w4[k] * (strong ? 1.6f : 1.0f));
        }
    };
    const float corner[4][2] = {{40, 40}, {W - 40.0f, 40}, {40, H - 40.0f}, {W - 40.0f, H - 40.0f}};
    const int scratches = static_cast<int>((strong ? 240 : 150) * amount);
    for (int i = 0; i < scratches; ++i) {
        float x, y;
        const float where = rnd();
        if (where < 0.62f) {
            // Round a corner, thinning out away from it.
            const int c = static_cast<int>(rnd() * 4) & 3;
            const float dist = -std::log(std::max(rnd(), 1e-4f)) * 48, ang = rnd() * 1.5707963f;
            x = corner[c][0] + (c & 1 ? -1 : 1) * std::cos(ang) * dist;
            y = corner[c][1] + (c & 2 ? -1 : 1) * std::sin(ang) * dist;
        } else if (where < 0.78f) {
            // Along the top and bottom ends.
            x = 20 + rnd() * (W - 40);
            const float dist = -std::log(std::max(rnd(), 1e-4f)) * 30;
            y = rnd() < 0.5f ? 20 + dist : H - 20 - dist;
        } else {
            x = rnd() * W;
            y = rnd() * H;
        }
        // Mostly tiny; now and then a long faint one.
        const bool longOne = rnd() < (strong ? 0.06f : 0.04f);
        const float length = strong ? (longOne ? 50 + rnd() * 110 : 2.5f + 34 * std::pow(rnd(), 2.4f))
                                   : (longOne ? 40 + rnd() * 80 : 2 + 22 * std::pow(rnd(), 2.6f));
        const float strength = strong ? (longOne ? 0.22f : 0.35f) + rnd() * (longOne ? 0.25f : 0.65f)
                                      : (longOne ? 0.15f : 0.25f) + rnd() * (longOne ? 0.2f : 0.55f);
        float ang = rnd() * 6.2831853f;
        const float bend = (rnd() - 0.5f) * 0.012f;
        for (float s = 0; s <= length; s += 0.5f) {
            const float t = s / std::max(length, 1.0f);
            put(x, y, strength * std::sqrt(std::sin(3.1415927f * t)) * (0.75f + 0.25f * rnd()));
            x += std::cos(ang) * 0.5f;
            y += std::sin(ang) * 0.5f;
            ang += bend;
        }
    }
    // Pits, nearly all in the corners.
    for (int i = 0, n = static_cast<int>((strong ? 160 : 110) * amount); i < n; ++i) {
        const int c = static_cast<int>(rnd() * 4) & 3;
        const float dist = -std::log(std::max(rnd(), 1e-4f)) * 36, ang = rnd() * 1.5707963f;
        put(corner[c][0] + (c & 1 ? -1 : 1) * std::cos(ang) * dist,
            corner[c][1] + (c & 2 ? -1 : 1) * std::sin(ang) * dist, strong ? 0.25f + rnd() * 0.45f : 0.2f + rnd() * 0.35f);
    }
    // A faint haze of wear in each corner.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!onGlass(x + 0.5f, y + 0.5f)) continue;
            float closest = 1e9f;
            for (const auto& c : corner) closest = std::min(closest, std::hypot(x - c[0], y - c[1]));
            float& t = v[y * W + x];
            t = std::max(t, (strong ? 0.07f : 0.05f) * std::min(amount, 1.0f) * std::exp(-closest / 55));
        }
    }
    // Each level half the one before, averaged, so they fade as the phone
    // grows small rather than shimmer.
    int w = W, h = H;
    for (int level = 0; level < kLevels; ++level) {
        D3DLOCKED_RECT lock{};
        if (FAILED(g_wear->LockRect(level, &lock, nullptr, 0))) {
            Release(g_wear);
            return false;
        }
        for (int y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lock.pBits) + y * lock.Pitch);
            for (int x = 0; x < w; ++x) {
                const uint32_t b = static_cast<uint32_t>(std::clamp(v[y * w + x], 0.0f, 1.0f) * 255 + 0.5f);
                row[x] = b << 24 | b << 16 | b << 8 | b;
            }
        }
        g_wear->UnlockRect(level);
        if (level + 1 == kLevels) break;
        std::vector<float> half((w / 2) * (h / 2));
        for (int y = 0; y < h / 2; ++y) {
            for (int x = 0; x < w / 2; ++x) {
                const float* p = &v[(y * 2) * w + x * 2];
                half[y * (w / 2) + x] = (p[0] + p[1] + p[w] + p[w + 1]) * 0.25f;
            }
        }
        v.swap(half);
        w /= 2;
        h /= 2;
    }
    return true;
}

struct Frame {
    float r[3][3];
    float pivot[4], rel[4], centre[4], depth[4];
};

Frame MakeFrame(const Pose& pose, float targetW, float targetH) {
    Frame f{};
    const float cy = std::cos(pose.yaw), sy = std::sin(pose.yaw);
    const float cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
    const float cr = std::cos(pose.roll), sr = std::sin(pose.roll);
    // Turned about its up (yaw), then tipped about its width (pitch), then
    // spun in the picture (roll, about the axis toward the viewer).
    const float yaw[3][3] = {{cy, -sy, 0}, {sy, cy, 0}, {0, 0, 1}};
    const float pitch[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
    const float roll[3][3] = {{cr, 0, sr}, {0, 1, 0}, {-sr, 0, cr}};
    float pr[3][3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) pr[i][j] += pitch[i][k] * yaw[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) f.r[i][j] += roll[i][k] * pr[k][j];
    const float scale = g_screenW > 0 ? pose.screen[2] / g_screenW : 1.0f;
    f.pivot[0] = g_pivot[0];
    f.pivot[1] = g_pivot[1];
    f.pivot[2] = g_pivot[2];
    f.pivot[3] = scale;
    for (int k = 0; k < 3; ++k) f.rel[k] = g_pivot[k] - g_screenMid[k];
    f.rel[3] = pose.screen[3] * 3.0f;  // the focal length: a gentle perspective
    f.centre[0] = pose.screen[0] + pose.screen[2] * 0.5f + pose.offset[0] - 0.5f;
    f.centre[1] = pose.screen[1] + pose.screen[3] * 0.5f + pose.offset[1] - 0.5f;
    f.centre[2] = 1.0f / targetW;
    f.centre[3] = 1.0f / targetH;
    f.depth[0] = std::min(f.rel[3] * 0.8f, std::max(1.0f, g_radius * scale * 1.5f));
    return f;
}

// The same as the vertex shader, for the corners of the model's box.
void Project(const Frame& f, const float p[3], float& x, float& y) {
    const float d[3] = {p[0] - f.pivot[0], p[1] - f.pivot[1], p[2] - f.pivot[2]};
    float r[3];
    for (int i = 0; i < 3; ++i) r[i] = f.r[i][0] * d[0] + f.r[i][1] * d[1] + f.r[i][2] * d[2] + f.rel[i];
    const float X = -r[0] * f.pivot[3], Y = -r[2] * f.pivot[3], Z = -r[1] * f.pivot[3];
    const float w = (f.rel[3] + Z) / f.rel[3];
    x = f.centre[0] + X / w;
    y = f.centre[1] + Y / w;
}

// The most samples the card takes for both the colour and the depth, up to
// eight; none if it takes none.
D3DMULTISAMPLE_TYPE Samples(IDirect3DDevice9* d, D3DFORMAT depth) {
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS created{};
    if (FAILED(d->GetDirect3D(&d3d)) || FAILED(d->GetCreationParameters(&created))) {
        if (d3d) d3d->Release();
        return D3DMULTISAMPLE_NONE;
    }
    D3DMULTISAMPLE_TYPE found = D3DMULTISAMPLE_NONE;
    for (D3DMULTISAMPLE_TYPE t : {D3DMULTISAMPLE_8_SAMPLES, D3DMULTISAMPLE_4_SAMPLES, D3DMULTISAMPLE_2_SAMPLES}) {
        if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(created.AdapterOrdinal, created.DeviceType, D3DFMT_A8R8G8B8,
                                                      TRUE, t, nullptr)) &&
            SUCCEEDED(d3d->CheckDeviceMultiSampleType(created.AdapterOrdinal, created.DeviceType, depth, TRUE, t,
                                                      nullptr))) {
            found = t;
            break;
        }
    }
    d3d->Release();
    return found;
}

bool Targets(IDirect3DDevice9* d, UINT w, UINT h) {
    if (g_target && (g_targetSize[0] != w || g_targetSize[1] != h)) {
        Release(g_target);
        Release(g_depth);
        Release(g_msaa);
    }
    if (!g_target) {
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_target,
                                    nullptr)))
            return false;
        // Multisampled if the card can, else drawn straight into the texture.
        for (D3DFORMAT format : {D3DFMT_D24X8, D3DFMT_D16}) {
            const D3DMULTISAMPLE_TYPE samples = Samples(d, format);
            if (samples != D3DMULTISAMPLE_NONE &&
                SUCCEEDED(d->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, samples, 0, FALSE, &g_msaa, nullptr)) &&
                SUCCEEDED(d->CreateDepthStencilSurface(w, h, format, samples, 0, TRUE, &g_depth, nullptr)))
                break;
            Release(g_msaa);
            if (SUCCEEDED(d->CreateDepthStencilSurface(w, h, format, D3DMULTISAMPLE_NONE, 0, TRUE, &g_depth, nullptr)))
                break;
        }
        if (!g_depth) {
            Release(g_target);
            Release(g_msaa);
            return false;
        }
        g_targetSize[0] = w;
        g_targetSize[1] = h;
    }
    return true;
}

// How each part takes the light: gloss, smoothness, metal, how much it mirrors.
const float kMaterial[kParts][4] = {
    {1.0f, 0.95f, 0.0f, 0.6f},   // the glass front, black-printed, with the home button
    {1.0f, 0.95f, 0.0f, 0.6f},   // the glass over the screen
    {0.6f, 0.55f, 1.0f, 0.55f},  // brushed aluminium
    {1.0f, 0.9f, 1.0f, 1.0f},    // polished steel
    {0.35f, 0.4f, 0.0f, 0.15f},  // black plastic
    {1.0f, 0.95f, 0.0f, 0.8f},   // the lens
    {1.0f, 0.9f, 1.0f, 1.0f},    // the side buttons: the rim's polished steel, in colour
};

}  // namespace

bool Load(IDirect3DDevice9* d, const std::string& dff) {
    Release(g_vertices);
    Release(g_indices);
    std::ifstream in(dff, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Vertex> verts;
    std::vector<uint16_t> byPart[kParts];
    if (bytes.size() < 12 || !Parse(bytes, verts, byPart) || byPart[kScreen].empty()) return false;
    // The side buttons: the only steel and black plastic standing out past
    // the sides of the body, which the back spans exactly.
    if (!byPart[kBack].empty()) {
        float left = 1e9f, right = -1e9f;
        for (uint16_t i : byPart[kBack]) {
            left = std::min(left, verts[i].pos[0]);
            right = std::max(right, verts[i].pos[0]);
        }
        const float margin = (right - left) * 0.004f;
        std::vector<bool> ring(verts.size(), false);
        for (int from : {kChrome, kBlack}) {
            std::vector<uint16_t> kept;
            for (size_t t = 0; t + 2 < byPart[from].size(); t += 3) {
                bool out = false;
                for (size_t c = 0; c < 3; ++c) {
                    const float x = verts[byPart[from][t + c]].pos[0];
                    out = out || x < left - margin || x > right + margin;
                }
                auto& to = out ? byPart[kButtons] : kept;
                to.insert(to.end(), byPart[from].begin() + t, byPart[from].begin() + t + 3);
                if (out && from == kBlack) {
                    for (size_t c = 0; c < 3; ++c) ring[byPart[from][t + c]] = true;
                }
            }
            byPart[from].swap(kept);
        }
        // Each its own colour, from its own quarter of vp_button: the sleep
        // button red, as a power button is; volume up a bright blue and down
        // a dark one; the ring switch orange, as the first iPhone's shows
        // when it is set to silent. The ring switch's side is the volume's;
        // the other is the sleep button's. Up is the model's +z.
        const float middle = (left + right) * 0.5f;
        float ringX = 0.0f, zLo = 1e9f, zHi = -1e9f;
        int ringCount = 0;
        for (size_t i = 0; i < verts.size(); ++i) {
            if (ring[i]) ringX += verts[i].pos[0], ++ringCount;
        }
        const bool volumeLow = ringCount && ringX / ringCount < middle;
        auto volumeSide = [&](float x) { return (x < middle) == volumeLow; };
        for (uint16_t i : byPart[kButtons]) {
            if (!ring[i] && volumeSide(verts[i].pos[0])) {
                zLo = std::min(zLo, verts[i].pos[2]);
                zHi = std::max(zHi, verts[i].pos[2]);
            }
        }
        for (size_t t = 0; t + 2 < byPart[kButtons].size(); t += 3) {
            const uint16_t* v = &byPart[kButtons][t];
            const float x = (verts[v[0]].pos[0] + verts[v[1]].pos[0] + verts[v[2]].pos[0]) / 3.0f;
            const float z = (verts[v[0]].pos[2] + verts[v[1]].pos[2] + verts[v[2]].pos[2]) / 3.0f;
            const int cell = ring[v[0]] ? 3 : !volumeSide(x) ? 0 : z > (zLo + zHi) * 0.5f ? 1 : 2;
            for (int c = 0; c < 3; ++c) {
                verts[v[c]].uv[0] = (cell + 0.5f) / 4.0f;
                verts[v[c]].uv[1] = 0.5f;
            }
        }
        // Stood a millimetre proud, as the model has them, they are lost at
        // the size the phone is on screen: out to a millimetre and a half.
        std::vector<bool> moved(verts.size(), false);
        for (uint16_t i : byPart[kButtons]) {
            float& x = verts[i].pos[0];
            if (moved[i]) continue;
            moved[i] = true;
            if (x < left) x = left - (left - x) * 1.5f;
            if (x > right) x = right + (x - right) * 1.5f;
        }
    }
    std::vector<uint16_t> indices;
    for (int k = 0; k < kParts; ++k) {
        g_ranges[k].first = static_cast<UINT>(indices.size());
        g_ranges[k].count = static_cast<UINT>(byPart[k].size());
        indices.insert(indices.end(), byPart[k].begin(), byPart[k].end());
    }
    // Its measures: the middle it turns about, and the screen it is placed by.
    float slo[3] = {1e9f, 1e9f, 1e9f}, shi[3] = {-1e9f, -1e9f, -1e9f};
    for (int k = 0; k < 3; ++k) {
        g_lo[k] = 1e9f;
        g_hi[k] = -1e9f;
    }
    for (const Vertex& v : verts) {
        for (int k = 0; k < 3; ++k) {
            g_lo[k] = std::min(g_lo[k], v.pos[k]);
            g_hi[k] = std::max(g_hi[k], v.pos[k]);
        }
    }
    for (uint16_t i : byPart[kScreen]) {
        for (int k = 0; k < 3; ++k) {
            slo[k] = std::min(slo[k], verts[i].pos[k]);
            shi[k] = std::max(shi[k], verts[i].pos[k]);
        }
    }
    g_radius = 0.0f;
    for (int k = 0; k < 3; ++k) {
        g_pivot[k] = (g_lo[k] + g_hi[k]) * 0.5f;
        g_screenMid[k] = (slo[k] + shi[k]) * 0.5f;
        g_radius += (g_hi[k] - g_lo[k]) * (g_hi[k] - g_lo[k]) * 0.25f;
    }
    g_radius = std::sqrt(g_radius);
    g_screenW = shi[0] - slo[0];
    if (g_screenW <= 0) return false;
    void* data = nullptr;
    if (FAILED(d->CreateVertexBuffer(static_cast<UINT>(verts.size() * sizeof(Vertex)), D3DUSAGE_WRITEONLY, 0,
                                     D3DPOOL_MANAGED, &g_vertices, nullptr)) ||
        FAILED(g_vertices->Lock(0, 0, &data, 0)))
        return false;
    memcpy(data, verts.data(), verts.size() * sizeof(Vertex));
    g_vertices->Unlock();
    if (FAILED(d->CreateIndexBuffer(static_cast<UINT>(indices.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                    D3DPOOL_MANAGED, &g_indices, nullptr)) ||
        FAILED(g_indices->Lock(0, 0, &data, 0))) {
        Release(g_vertices);
        return false;
    }
    memcpy(data, indices.data(), indices.size() * 2);
    g_indices->Unlock();
    g_vertexCount = static_cast<UINT>(verts.size());
    if (!g_decl) {
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
            {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()};
        if (FAILED(d->CreateVertexDeclaration(elements, &g_decl))) return false;
    }
    return true;
}

bool Loaded() { return g_vertices && g_indices && g_decl; }

bool BeginScreen(IDirect3DDevice9* d, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (g_screen && (g_screenSize[0] != w || g_screenSize[1] != h)) Release(g_screen);
    if (!g_screen) {
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_screen,
                                    nullptr)))
            return false;
        g_screenSize[0] = w;
        g_screenSize[1] = h;
    }
    IDirect3DSurface9* surface = nullptr;
    if (FAILED(g_screen->GetSurfaceLevel(0, &surface))) return false;
    Release(g_savedTarget);
    Release(g_savedDepth);
    d->GetRenderTarget(0, &g_savedTarget);
    d->GetDepthStencilSurface(&g_savedDepth);
    d->GetViewport(&g_savedViewport);
    const HRESULT hr = d->SetRenderTarget(0, surface);
    surface->Release();
    if (FAILED(hr)) {
        Release(g_savedTarget);
        Release(g_savedDepth);
        return false;
    }
    // Nothing on the screen is depth tested, and the game's own depth buffer
    // may be multisampled where this texture is not, which the device will
    // not draw with.
    d->SetDepthStencilSurface(nullptr);
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    return true;
}

bool BeginGlass(IDirect3DDevice9* d, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (g_glass && (g_glassSize[0] != w || g_glassSize[1] != h)) Release(g_glass);
    if (!g_glass) {
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_glass,
                                    nullptr)))
            return false;
        g_glassSize[0] = w;
        g_glassSize[1] = h;
    }
    IDirect3DSurface9* surface = nullptr;
    if (FAILED(g_glass->GetSurfaceLevel(0, &surface))) return false;
    Release(g_savedTarget);
    Release(g_savedDepth);
    d->GetRenderTarget(0, &g_savedTarget);
    d->GetDepthStencilSurface(&g_savedDepth);
    d->GetViewport(&g_savedViewport);
    const HRESULT hr = d->SetRenderTarget(0, surface);
    surface->Release();
    if (FAILED(hr)) {
        Release(g_savedTarget);
        Release(g_savedDepth);
        return false;
    }
    d->SetDepthStencilSurface(nullptr);
    // Clear glass: what is drawn on it is laid over the phone as it is. Its
    // alpha is built up as coverage (one plus under), so the layer holds each
    // stroke premultiplied, as the phone's shader lays it on.
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    return true;
}

void EndGlass(IDirect3DDevice9* d) {
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    EndScreen(d);
}

void EndScreen(IDirect3DDevice9* d) {
    if (!g_savedTarget) return;
    d->SetRenderTarget(0, g_savedTarget);
    d->SetDepthStencilSurface(g_savedDepth);
    d->SetViewport(&g_savedViewport);
    Release(g_savedTarget);
    Release(g_savedDepth);
}

IDirect3DTexture9* ScreenTexture() { return g_screen; }
IDirect3DTexture9* DebugTarget() { return g_target; }

void Bounds(float out[4]) { memcpy(out, g_bounds, sizeof g_bounds); }

bool ScreenCorners(float out[6]) {
    memcpy(out, g_corners, sizeof g_corners);
    return g_haveCorners;
}

bool Prepare(IDirect3DDevice9* d, const Pose& pose) {
    if (!Shaders(d)) return false;
    if (pose.wear > 0) Wear(d, std::min(pose.wear, 3.0f), pose.wearSeed, pose.wearStrong);
    return true;
}

void ReleaseDeviceObjects() {
    Release(g_screen);
    Release(g_glass);
    Release(g_target);
    Release(g_depth);
    Release(g_msaa);
    Release(g_savedTarget);
    Release(g_savedDepth);
}

bool Draw(IDirect3DDevice9* d, const Pose& pose, const Light& light, IDirect3DTexture9* const parts[kParts],
          IDirect3DTexture9* env, float inkScale, float inkTime) {
    if (!Loaded() || !env || !Shaders(d)) return false;
    for (int k = 0; k < kParts; ++k) {
        if (!(k == kScreen ? g_screen : parts[k])) return false;
    }
    IDirect3DSurface9* backTarget = nullptr;
    IDirect3DSurface9* backDepth = nullptr;
    if (FAILED(d->GetRenderTarget(0, &backTarget))) return false;
    D3DSURFACE_DESC desc{};
    backTarget->GetDesc(&desc);
    d->GetDepthStencilSurface(&backDepth);
    D3DVIEWPORT9 viewport{};
    d->GetViewport(&viewport);
    struct Held {
        IDirect3DSurface9 *a, *b;
        ~Held() {
            if (a) a->Release();
            if (b) b->Release();
        }
    } held{backTarget, backDepth};
    if (!Targets(d, desc.Width, desc.Height)) return false;
    IDirect3DStateBlock9* state = nullptr;
    if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &state))) return false;
    state->Capture();

    const Frame f = MakeFrame(pose, static_cast<float>(desc.Width), static_cast<float>(desc.Height));
    // Where it lands, from the corners of its box, with room for the ink.
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {c & 1 ? g_hi[0] : g_lo[0], c & 2 ? g_hi[1] : g_lo[1], c & 4 ? g_hi[2] : g_lo[2]};
        float x, y;
        Project(f, p, x, y);
        lo[0] = std::min(lo[0], x);
        lo[1] = std::min(lo[1], y);
        hi[0] = std::max(hi[0], x);
        hi[1] = std::max(hi[1], y);
    }
    // The screen's corners (its plane, 2:3 about its middle; the model's +x
    // is the viewer's left, +z up), for the cursor.
    {
        const float hw = g_screenW * 0.5f, hh = g_screenW * 0.75f;
        const float tl[3] = {g_screenMid[0] + hw, g_screenMid[1], g_screenMid[2] + hh};
        const float tr[3] = {g_screenMid[0] - hw, g_screenMid[1], g_screenMid[2] + hh};
        const float bl[3] = {g_screenMid[0] + hw, g_screenMid[1], g_screenMid[2] - hh};
        Project(f, tl, g_corners[0], g_corners[1]);
        Project(f, tr, g_corners[2], g_corners[3]);
        Project(f, bl, g_corners[4], g_corners[5]);
        for (float& v : g_corners) v += 0.5f;  // Project works in half-pixel-shifted pixels
        g_haveCorners = true;
    }
    const float margin = 14.0f * inkScale + 2.0f;
    g_bounds[0] = std::clamp(std::floor(lo[0] - margin), 0.0f, static_cast<float>(desc.Width));
    g_bounds[1] = std::clamp(std::floor(lo[1] - margin), 0.0f, static_cast<float>(desc.Height));
    g_bounds[2] = std::clamp(std::ceil(hi[0] + margin), 0.0f, static_cast<float>(desc.Width));
    g_bounds[3] = std::clamp(std::ceil(hi[1] + margin), 0.0f, static_cast<float>(desc.Height));
    bool ok = g_bounds[2] > g_bounds[0] && g_bounds[3] > g_bounds[1];

    IDirect3DSurface9* surface = nullptr;
    if (ok && SUCCEEDED(g_target->GetSurfaceLevel(0, &surface))) {
        d->SetRenderTarget(0, g_msaa ? g_msaa : surface);
        d->SetDepthStencilSurface(g_depth);
        // All of it: the ink looks past the phone's own box for its edge, and
        // must find nothing left there from an earlier frame.
        d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);

        // The model.
        d->SetVertexShader(g_vs);
        d->SetPixelShader(g_ps);
        d->SetVertexDeclaration(g_decl);
        d->SetStreamSource(0, g_vertices, 0, sizeof(Vertex));
        d->SetIndices(g_indices);
        d->SetRenderState(D3DRS_ZENABLE, TRUE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        const float rows[3][4] = {{f.r[0][0], f.r[0][1], f.r[0][2], 0},
                                  {f.r[1][0], f.r[1][1], f.r[1][2], 0},
                                  {f.r[2][0], f.r[2][1], f.r[2][2], 0}};
        d->SetVertexShaderConstantF(0, &rows[0][0], 3);
        d->SetVertexShaderConstantF(3, f.pivot, 1);
        d->SetVertexShaderConstantF(4, f.rel, 1);
        d->SetVertexShaderConstantF(5, f.centre, 1);
        d->SetVertexShaderConstantF(6, f.depth, 1);
        const float dir[4] = {light.dir[0], light.dir[1], light.dir[2], 0};
        const float amb[4] = {light.ambient[0], light.ambient[1], light.ambient[2], 1};
        const float envp[4] = {light.env[0], light.env[1], light.env[2], light.liveEnv ? 1.0f : 0.0f};
        const float window[4] = {std::max(0.05f, light.liveWindow[0]), std::max(0.05f, light.liveWindow[1]), 0, 0};
        const float eye[4] = {desc.Width * 0.5f, desc.Height * 0.5f, desc.Height * 1.2f, 0};
        d->SetPixelShaderConstantF(0, dir, 1);
        d->SetPixelShaderConstantF(1, light.sun, 1);
        d->SetPixelShaderConstantF(2, amb, 1);
        d->SetPixelShaderConstantF(3, envp, 1);
        d->SetPixelShaderConstantF(14, window, 1);
        d->SetPixelShaderConstantF(4, eye, 1);
        const float face[4] = {lo[0], lo[1], std::max(1.0f, hi[0] - lo[0]), std::max(1.0f, hi[1] - lo[1])};
        d->SetPixelShaderConstantF(7, face, 1);
        const float display[4] = {static_cast<float>(g_screenSize[0]), static_cast<float>(g_screenSize[1]), pose.lcd, 0};
        d->SetPixelShaderConstantF(8, display, 1);
        // The screen's own axes as the viewer sees them: its right is the
        // model's -x, its up +z, its face +y; the viewer's x is right, y up,
        // z toward the viewer.
        auto seen = [&](float mx, float my, float mz, float out[4]) {
            float r[3];
            for (int i = 0; i < 3; ++i) r[i] = f.r[i][0] * mx + f.r[i][1] * my + f.r[i][2] * mz;
            out[0] = -r[0];
            out[1] = r[2];
            out[2] = r[1];
            out[3] = 0;
        };
        float axes[3][4];
        seen(-1, 0, 0, axes[0]);
        seen(0, 0, 1, axes[1]);
        seen(0, 1, 0, axes[2]);
        d->SetPixelShaderConstantF(9, &axes[0][0], 3);
        d->SetPixelShaderConstantF(12, pose.screenOnFront, 1);
        const bool glassOn = pose.glass && g_glass;
        d->SetTexture(2, glassOn ? g_glass : nullptr);
        const bool wornOn = pose.wear > 0 && Wear(d, std::min(pose.wear, 3.0f), pose.wearSeed, pose.wearStrong);
        const float worn[4] = {wornOn ? 1.0f : 0.0f, pose.wearStrong ? 6.0f : 16.0f, pose.wearStrong ? 1.3f : 0.6f,
                               pose.wearStrong ? 0.35f : 0.07f};
        d->SetPixelShaderConstantF(13, worn, 1);
        d->SetTexture(3, wornOn ? g_wear : nullptr);
        d->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(3, D3DSAMP_MAXMIPLEVEL, 0);
        d->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(3, D3DSAMP_SRGBTEXTURE, FALSE);
        d->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, FALSE);
        for (DWORD s = 0; s < 2; ++s) {
            d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
            d->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
        }
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        // The live picture is looked up past its edges wherever the surface
        // faces well away from the viewer - the rounded steel, the sides.
        // Clamped, those looks repeat its edge pixels into long streaks;
        // mirrored, they carry on into a reflected copy of it, which on a
        // curve reads as the world wrapping round.
        d->SetSamplerState(1, D3DSAMP_ADDRESSU, light.liveEnv ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP);
        d->SetSamplerState(1, D3DSAMP_ADDRESSV, light.liveEnv ? D3DTADDRESS_MIRROR : D3DTADDRESS_CLAMP);
        d->SetTexture(1, env);
        for (int k = 0; k < kParts; ++k) {
            if (!g_ranges[k].count) continue;
            d->SetTexture(0, k == kScreen ? g_screen : parts[k]);
            const float lit[4] = {k == kScreen ? 1.0f : 0.0f, pose.brightness,
                                  glassOn && (k == kScreen || k == kFront) ? 1.0f : 0.0f,
                                  k == kScreen || k == kFront ? 1.0f : 0.0f};
            d->SetPixelShaderConstantF(5, kMaterial[k], 1);
            d->SetPixelShaderConstantF(6, lit, 1);
            if (FAILED(d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, g_vertexCount, g_ranges[k].first,
                                               g_ranges[k].count / 3)))
                ok = false;
        }

        // The samples averaged into the texture the ink reads.
        if (g_msaa && FAILED(d->StretchRect(g_msaa, nullptr, surface, nullptr, D3DTEXF_NONE))) ok = false;
        surface->Release();

        // Laid onto the picture with its ink round it.
        d->SetRenderTarget(0, backTarget);
        d->SetDepthStencilSurface(backDepth);
        d->SetViewport(&viewport);
        d->SetVertexShader(nullptr);
        d->SetPixelShader(g_ink);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        d->SetTexture(0, g_target);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetTexture(1, g_taps);
        d->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        const float params[4] = {static_cast<float>(desc.Width), static_cast<float>(desc.Height), inkTime, inkScale};
        d->SetPixelShaderConstantF(0, params, 1);
        const float inkColour[4] = {((pose.ink >> 16) & 255) / 255.0f, ((pose.ink >> 8) & 255) / 255.0f,
                                    (pose.ink & 255) / 255.0f, ((pose.ink >> 24) & 255) / 255.0f};
        d->SetPixelShaderConstantF(1, inkColour, 1);
        struct Quad {
            float x, y, z, w, u, v;
        };
        const float l = g_bounds[0], t = g_bounds[1], r = g_bounds[2], b = g_bounds[3];
        const float iw = 1.0f / desc.Width, ih = 1.0f / desc.Height;
        const Quad quad[4] = {{l - 0.5f, t - 0.5f, 0, 1, l * iw, t * ih},
                              {r - 0.5f, t - 0.5f, 0, 1, r * iw, t * ih},
                              {l - 0.5f, b - 0.5f, 0, 1, l * iw, b * ih},
                              {r - 0.5f, b - 0.5f, 0, 1, r * iw, b * ih}};
        if (FAILED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Quad)))) ok = false;
    } else {
        ok = false;
    }
    d->SetRenderTarget(0, backTarget);
    d->SetDepthStencilSurface(backDepth);
    state->Apply();
    state->Release();
    d->SetViewport(&viewport);
    return ok;
}

}  // namespace phone3d
