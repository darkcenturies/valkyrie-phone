// Draws the phone the way the game will (src/phone3d.cpp) without the game:
// its own Direct3D 9 device, the model and textures from assets, a picture
// standing in for the phone's screen, and the phone at rest and on its way
// in from the corner, each saved as a PNG.
//
//   phone3d-test.exe <assets folder> <out folder>
//
// Built by tools/phone-model/test-phone3d.ps1.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/phone3d.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

IWICImagingFactory* g_wic = nullptr;

IDirect3DTexture9* LoadPng(IDirect3DDevice9* d, const std::wstring& path) {
    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                &decoder))) {
        fwprintf(stderr, L"cannot read %ls\n", path.c_str());
        return nullptr;
    }
    IWICBitmapFrameDecode* frame = nullptr;
    decoder->GetFrame(0, &frame);
    IWICFormatConverter* conv = nullptr;
    g_wic->CreateFormatConverter(&conv);
    conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                     WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
    conv->Release();
    frame->Release();
    decoder->Release();
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) return nullptr;
    D3DLOCKED_RECT r{};
    tex->LockRect(0, &r, nullptr, 0);
    for (UINT y = 0; y < h; ++y) memcpy(static_cast<BYTE*>(r.pBits) + y * r.Pitch, &pixels[y * w * 4], w * 4);
    tex->UnlockRect(0);
    return tex;
}

void SavePng(IDirect3DDevice9* d, const std::wstring& path) {
    IDirect3DSurface9 *target = nullptr, *copy = nullptr;
    d->GetRenderTarget(0, &target);
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
    d->GetRenderTargetData(target, copy);
    D3DLOCKED_RECT r{};
    copy->LockRect(&r, nullptr, D3DLOCK_READONLY);
    IWICStream* stream = nullptr;
    g_wic->CreateStream(&stream);
    stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    IWICBitmapEncoder* enc = nullptr;
    g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* frame = nullptr;
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(desc.Width, desc.Height);
    // As 32-bit BGRA, which PNG takes as it is, every pixel opaque.
    std::vector<BYTE> rows(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        memcpy(&rows[static_cast<size_t>(y) * desc.Width * 4], static_cast<BYTE*>(r.pBits) + y * r.Pitch, desc.Width * 4);
        for (UINT x = 0; x < desc.Width; ++x) rows[(static_cast<size_t>(y) * desc.Width + x) * 4 + 3] = 255;
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (fmt != GUID_WICPixelFormat32bppBGRA) fprintf(stderr, "the PNG encoder will not take BGRA\n");
    frame->WritePixels(desc.Height, desc.Width * 4, static_cast<UINT>(rows.size()), rows.data());
    frame->Commit();
    enc->Commit();
    frame->Release();
    enc->Release();
    stream->Release();
    copy->UnlockRect();
    copy->Release();
    target->Release();
}

void Quad(IDirect3DDevice9* d, IDirect3DTexture9* tex, float x, float y, float w, float h) {
    struct V {
        float x, y, z, rhw, u, v;
    } q[4] = {{x - .5f, y - .5f, 0, 1, 0, 0}, {x + w - .5f, y - .5f, 0, 1, 1, 0},
              {x - .5f, y + h - .5f, 0, 1, 0, 1}, {x + w - .5f, y + h - .5f, 0, 1, 1, 1}};
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetTexture(0, tex);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fprintf(stderr, "phone3d-test <assets folder> <out folder>\n");
        return 2;
    }
    const std::wstring assets = argv[1], out = argv[2];
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    const int W = 1600, H = 900;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"phone3d-test";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"phone3d", WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr,
                              wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = hwnd;
    IDirect3DDevice9* d = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d))) {
        fprintf(stderr, "no Direct3D 9 device\n");
        return 1;
    }
    std::string dff(assets.begin(), assets.end());
    dff += "\\model\\valkyrie-phone-model.dff";
    if (!phone3d::Load(d, dff)) {
        fprintf(stderr, "the model would not load\n");
        return 1;
    }
    IDirect3DTexture9* parts[phone3d::kParts] = {};
    for (int k = 0; k < phone3d::kParts; ++k) {
        std::string name = phone3d::kPartTextures[k];
        parts[k] = LoadPng(d, assets + L"\\model\\textures\\" + std::wstring(name.begin(), name.end()) + L".png");
    }
    IDirect3DTexture9* picture = LoadPng(d, assets + L"\\model\\textures\\vp_screen.png");
    // A third argument: another picture to reflect (a grid shows any bending).
    IDirect3DTexture9* env = LoadPng(d, argc > 3 ? std::wstring(argv[3]) : assets + L"\\generated\\phone_env.png");
    IDirect3DTexture9* backdrop = LoadPng(d, assets + L"\\generated\\wall_12.png");
    if (!picture || !env) return 1;

    // Where the flat phone puts its screen at this size (phone.cpp's Draw,
    // Height 0.78, on the right).
    const float bodyH = H * 0.78f, w = bodyH / (2.0f * 0.89729f), margin = H * 0.03f;
    const float x = W - w - margin * 1.5f, y = H - bodyH - margin;
    const float screen[4] = {x + w * 0.08140f, y + 2 * w * 0.12791f, w * (0.91860f - 0.08140f),
                             2 * w * (0.75581f - 0.12791f)};

    struct Shot {
        const wchar_t* name;
        float slide;
        bool worn = false;  // the screen off and the sun on the glass, for its scratches
        bool subtle = false;
        bool tilt = false;
        bool live = false;  // the environment picture taken as the live reflection  // the most the random rest tilt gives, and the home button marked
    } shots[] = {{L"rest", 1.0f}, {L"coming", 0.6f}, {L"corner", 0.3f}, {L"lowered", 0.42f}, {L"worn", 1.0f, true}, {L"worn-subtle", 1.0f, true, true}, {L"buttons", 1.0f, false, false, true}, {L"live", 1.0f, false, false, false, true}};
    for (const Shot& s : shots) {
        d->BeginScene();
        // The screen's own picture first, into its texture.
        const int sw = static_cast<int>(std::lround(screen[2])), sh = static_cast<int>(std::lround(screen[3]));
        const bool began = phone3d::BeginScreen(d, sw, sh);
        if (!began) wprintf(L"the screen texture would not take drawing\n");
        if (began) {
            Quad(d, picture, 0, 0, static_cast<float>(sw), static_cast<float>(sh));
            phone3d::EndScreen(d);
        }
        d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF5A6470, 1.0f, 0);
        phone3d::Pose pose{};
        memcpy(pose.screen, screen, sizeof screen);
        const float away = 1.0f - s.slide;
        pose.offset[0] = away * w * 0.9f;
        pose.offset[1] = away * bodyH * 1.1f;
        pose.yaw = away * 0.9f;
        pose.pitch = away * 0.35f;
        pose.roll = -away * 0.45f;
        if (s.tilt) {
            pose.yaw += 0.044f;
            pose.pitch += 0.044f;
            pose.roll += 0.026f;
        }
        pose.brightness = s.worn || s.live ? 0.0f : 1.0f;
        pose.wear = 1.0f;
        pose.wearStrong = !s.subtle;
        phone3d::Light light{};
        light.dir[0] = -0.4f;
        light.dir[1] = 0.6f;
        light.dir[2] = 0.7f;
        light.sun[0] = 1.0f;
        light.sun[1] = 0.95f;
        light.sun[2] = 0.86f;
        light.sun[3] = 0.8f;
        light.ambient[0] = light.ambient[1] = light.ambient[2] = 0.45f;
        light.env[2] = 1.0f;
        light.liveEnv = s.live;
        if (s.worn) {
            light.dir[0] = -0.25f;
            light.dir[1] = 0.1f;
            light.dir[2] = 0.96f;
            light.sun[3] = 1.0f;
        }
        // Two marks on the glass layer: one on the black print above the
        // screen, one in the middle - both should land on the front.
        const float frontW = w * (0.97287f - 0.02713f), frontH = 2 * w * 0.89729f;
        if (phone3d::BeginGlass(d, static_cast<int>(frontW), static_cast<int>(frontH))) {
            Quad(d, parts[phone3d::kLens], frontW * 0.5f - 20, frontH * 0.07f, 40, 40);
            Quad(d, parts[phone3d::kLens], frontW * 0.5f - 20, frontH * 0.5f - 20, 40, 40);
            phone3d::EndGlass(d);
            pose.glass = true;
            pose.screenOnFront[0] = (0.08140f - 0.02713f) / (0.97287f - 0.02713f);
            pose.screenOnFront[1] = 0.12791f / 0.89729f;
            pose.screenOnFront[2] = (0.91860f - 0.02713f) / (0.97287f - 0.02713f);
            pose.screenOnFront[3] = 0.75581f / 0.89729f;
        }
        const bool ok = phone3d::Draw(d, pose, light, parts, env, 1.0f, 3.0f);
        if (s.tilt) {
            // Where the phone's hit test puts the home button (the flat
            // layout's kHomeX, kHomeY and kHomeRadius): its middle and its edge.
            const float hx = x + w * 0.5f, hy = y + 2 * w * 0.82171f, hr = w * 0.08140f;
            Quad(d, parts[phone3d::kLens], hx - 3, hy - 3, 6, 6);
            Quad(d, parts[phone3d::kLens], hx - hr - 2, hy - 2, 4, 4);
            Quad(d, parts[phone3d::kLens], hx + hr - 2, hy - 2, 4, 4);
            Quad(d, parts[phone3d::kLens], hx - 2, hy - hr - 2, 4, 4);
            Quad(d, parts[phone3d::kLens], hx - 2, hy + hr - 2, 4, 4);
            float c[6];
            if (phone3d::ScreenCorners(c))
                wprintf(L"screen corners %.1f,%.1f %.1f,%.1f %.1f,%.1f vs flat %.1f,%.1f\n", c[0], c[1], c[2], c[3],
                        c[4], c[5], screen[0], screen[1]);
        }
        // The screen's own texture, flat in the corner, to compare.
        Quad(d, phone3d::ScreenTexture(), 10, 10, screen[2] * 0.5f, screen[3] * 0.5f);
        d->EndScene();
        SavePng(d, out + L"\\phone3d_" + s.name + L".png");
        if (IDirect3DTexture9* t = phone3d::DebugTarget()) {
            // The picture the ink is laid round, alpha and all.
            IDirect3DSurface9 *src = nullptr, *copy = nullptr;
            t->GetSurfaceLevel(0, &src);
            D3DSURFACE_DESC dd{};
            src->GetDesc(&dd);
            d->CreateOffscreenPlainSurface(dd.Width, dd.Height, dd.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
            d->GetRenderTargetData(src, copy);
            D3DLOCKED_RECT lr{};
            copy->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            FILE* f = _wfopen((out + L"\\target_" + s.name + L".raw").c_str(), L"wb");
            for (UINT yy = 0; yy < dd.Height; ++yy) fwrite(static_cast<BYTE*>(lr.pBits) + yy * lr.Pitch, 1, dd.Width * 4, f);
            fclose(f);
            copy->UnlockRect();
            copy->Release();
            src->Release();
        }
        float bounds[4];
        phone3d::Bounds(bounds);
        wprintf(L"%ls: %ls, at %.0f %.0f - %.0f %.0f (screen %.0f %.0f %.0f %.0f)\n", s.name,
                ok ? L"drawn" : L"NOT DRAWN", bounds[0], bounds[1], bounds[2], bounds[3], screen[0], screen[1],
                screen[2], screen[3]);
    }
    return 0;
}
