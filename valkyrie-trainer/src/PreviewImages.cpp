#include <plugin.h>
#include <common.h>
#include <d3d9.h>
#include <windows.h>
#include <map>
#include <string>
#include <stb/stb_image.h>
#include "PreviewImages.h"

using namespace plugin;

namespace
{
    constexpr int kFrameCount = 12;

    struct CacheEntry
    {
        IDirect3DTexture9 *frames[kFrameCount] = {};
        bool attempted[kFrameCount] = {};
    };

    std::map<std::string, CacheEntry> g_cache;

    const std::string &ExeDir()
    {
        static std::string dir = [] {
            char exePath[MAX_PATH];
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            std::string path(exePath);
            size_t slash = path.find_last_of("\\/");
            return slash != std::string::npos ? path.substr(0, slash) : path;
        }();
        return dir;
    }

    IDirect3DTexture9 *LoadPng(const std::string &path)
    {
        IDirect3DDevice9 *device = reinterpret_cast<IDirect3DDevice9 *>(GetD3DDevice());
        if (!device) return nullptr;

        int width, height, channels;
        unsigned char *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!pixels) return nullptr;

        IDirect3DTexture9 *texture = nullptr;
        if (FAILED(device->CreateTexture(static_cast<UINT>(width), static_cast<UINT>(height), 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
        {
            stbi_image_free(pixels);
            return nullptr;
        }

        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            for (int y = 0; y < height; y++)
            {
                unsigned char *dst = reinterpret_cast<unsigned char *>(locked.pBits) + y * locked.Pitch;
                unsigned char *src = pixels + static_cast<size_t>(y) * width * 4;
                for (int x = 0; x < width; x++)
                {
                    // stb decodes RGBA; D3DFMT_A8R8G8B8 is byte-order BGRA.
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                }
            }
            texture->UnlockRect(0);
        }

        stbi_image_free(pixels);
        return texture;
    }
}

namespace PreviewImages
{
    void *GetFrame(const char *kind, int modelId, int frameIndex)
    {
        if (frameIndex < 0 || frameIndex >= kFrameCount) return nullptr;

        std::string key = std::string(kind) + "_" + std::to_string(modelId);
        CacheEntry &entry = g_cache[key];

        if (!entry.attempted[frameIndex])
        {
            entry.attempted[frameIndex] = true;
            char frameSuffix[8];
            _snprintf_s(frameSuffix, sizeof(frameSuffix), "%02d", frameIndex);
            std::string path = ExeDir() + "\\previews\\" + kind + "\\" + kind + "_"
                + std::to_string(modelId) + "_" + frameSuffix + ".png";
            entry.frames[frameIndex] = LoadPng(path);
        }
        return entry.frames[frameIndex];
    }

    void Trim()
    {
        if (g_cache.size() <= 64) return;
        for (auto &pair : g_cache)
        {
            for (auto *tex : pair.second.frames)
            {
                if (tex) tex->Release();
            }
        }
        g_cache.clear();
    }
}
