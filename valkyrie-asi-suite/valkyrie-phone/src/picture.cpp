#include "picture.h"

#include <windows.h>
#include <wincodec.h>

#include <atomic>
#include <thread>
#include <vector>

#include "log.h"
#include "sprite.h"

namespace picture {
namespace {

IWICImagingFactory* Factory() {
    static IWICImagingFactory* factory = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // The game's thread may have COM set up already, in either model;
        // any of those answers leaves it usable.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) {
            logfile::Line("picture: no image decoder");
            factory = nullptr;
        }
    }
    return factory;
}

}  // namespace

Picture Load(const std::string& path, int side) {
    Picture out;
    IWICImagingFactory* f = Factory();
    if (!f) return out;
    std::wstring wide(path.begin(), path.end());
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT w = 0, h = 0;
    std::vector<uint32_t> pixels(static_cast<size_t>(side) * side);
    const bool ok =
        SUCCEEDED(f->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                               &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h &&
        SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, side, side, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(f->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->CopyPixels(nullptr, side * 4, side * side * 4, reinterpret_cast<BYTE*>(pixels.data())));
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (!ok) {
        logfile::Line("picture: could not read %s", path.c_str());
        return out;
    }
    out.texture = sprite::CreateTexture(side, side, pixels.data(), side);
    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    return out;
}

void Free(Picture& p) {
    sprite::DestroyTexture(p.texture);
    p = {};
}

namespace {

std::atomic<int> g_cropsRunning{0};

// Runs on its own thread, with its own COM and its own factory.
void Crop(std::wstring path, float aspect) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmap* bitmap = nullptr;
    IWICBitmapClipper* clipper = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* out = nullptr;
    IPropertyBag2* options = nullptr;
    UINT w = 0, h = 0;
    const std::wstring temp = path + L".tmp";
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h &&
              // Read all of it now, so the file is free to be replaced.
              SUCCEEDED(f->CreateBitmapFromSource(frame, WICBitmapCacheOnLoad, &bitmap));
    WICRect keep{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    if (ok) {
        const UINT cropW = static_cast<UINT>(h * aspect + 0.5f);
        if (cropW >= w) ok = false;  // already that narrow
        else keep = {static_cast<INT>((w - cropW) / 2), 0, static_cast<INT>(cropW), static_cast<INT>(h)};
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(f->CreateBitmapClipper(&clipper)) && SUCCEEDED(clipper->Initialize(bitmap, &keep)) &&
         SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&out, &options));
    if (ok && options) {
        PROPBAG2 name{};
        name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT quality{};  // zeroed, as VariantInit would (oleaut32 is not linked)
        quality.vt = VT_R4;
        quality.fltVal = 0.92f;
        options->Write(1, &name, &quality);
    }
    ok = ok && SUCCEEDED(out->Initialize(options)) && SUCCEEDED(out->SetSize(keep.Width, keep.Height)) &&
         SUCCEEDED(out->SetPixelFormat(&format)) && SUCCEEDED(out->WriteSource(clipper, nullptr)) &&
         SUCCEEDED(out->Commit()) && SUCCEEDED(encoder->Commit());
    if (options) options->Release();
    if (out) out->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (clipper) clipper->Release();
    if (bitmap) bitmap->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (f) f->Release();
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileW(temp.c_str());
    if (SUCCEEDED(com)) CoUninitialize();
    --g_cropsRunning;
}

}  // namespace

void CropToAspect(const std::string& path, float aspect) {
    if (path.empty() || aspect <= 0.0f) return;
    ++g_cropsRunning;
    try {
        std::thread(Crop, std::wstring(path.begin(), path.end()), aspect).detach();
    } catch (...) {
        --g_cropsRunning;
        logfile::Line("picture: could not start the crop of %s", path.c_str());
    }
}

bool CropDone() { return g_cropsRunning.load() == 0; }

}  // namespace picture
