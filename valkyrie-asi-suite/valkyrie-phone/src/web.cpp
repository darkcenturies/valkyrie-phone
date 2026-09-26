#include "web.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "log.h"
#include "sprite.h"

namespace web {
namespace {

// A page is cut into tiles of this size, the last ones padded out with the
// page's own edge so nothing shows through when they are filtered.
constexpr int kTileW = 512;
constexpr int kTileH = 512;

struct Entry {
    Page page;
    uint32_t pngAt = 0, pngSize = 0;
};

std::string g_path;
std::vector<Entry> g_pages;
std::vector<std::vector<Link>> g_links;
std::vector<Site> g_sites;
const Page kEmpty{};
const std::vector<Link> kNoLinks;

// The decoder thread and what it hands back.
std::mutex g_mutex;
std::condition_variable g_wake;
std::thread g_thread;
int g_request = kNoPage;    // a page asked for and not yet started
int g_decoding = kNoPage;   // the page the thread is on
int g_decoded = kNoPage;    // the page in g_pixels, waiting to be taken
std::vector<uint32_t> g_pixels;
int g_decodedW = 0, g_decodedH = 0;

// The page on the GPU.
int g_loaded = kNoPage;
std::vector<uintptr_t> g_tiles;
int g_cols = 0, g_rows = 0;

bool ReadAt(FILE* f, uint32_t at, void* out, size_t size) {
    return fseek(f, static_cast<long>(at), SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}

// A PNG to 0xAARRGGBB pixels, with Windows' own decoder.
bool Decode(IWICImagingFactory* factory, const std::vector<uint8_t>& png, std::vector<uint32_t>& out,
            int& w, int& h) {
    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        converter->GetSize(&uw, &uh);
        w = static_cast<int>(uw);
        h = static_cast<int>(uh);
        out.assign(static_cast<size_t>(w) * h, 0);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, uw * 4, uw * uh * 4, reinterpret_cast<BYTE*>(out.data())));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    return ok;
}

void DecoderThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    // The first factory: the one Windows 7 has as well.
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        logfile::Line("web: no image decoder");
        return;
    }
    FILE* f = nullptr;
    fopen_s(&f, g_path.c_str(), "rb");
    for (;;) {
        int page;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return g_request != kNoPage; });
            page = g_request;
            g_request = kNoPage;
            g_decoding = page;
        }
        const Entry& e = g_pages[page];
        std::vector<uint8_t> png(e.pngSize);
        std::vector<uint32_t> pixels;
        int w = 0, h = 0;
        const bool ok = f && ReadAt(f, e.pngAt, png.data(), png.size()) && Decode(factory, png, pixels, w, h);
        if (!ok) logfile::Line("web: could not read %s/%s", e.page.site.c_str(), e.page.name.c_str());
        std::lock_guard<std::mutex> lock(g_mutex);
        g_decoding = kNoPage;
        g_decoded = page;
        g_pixels = std::move(pixels);
        g_decodedW = ok ? w : 0;
        g_decodedH = ok ? h : 0;
    }
}

void Upload(const std::vector<uint32_t>& pixels, int w, int h) {
    g_cols = (w + kTileW - 1) / kTileW;
    g_rows = (h + kTileH - 1) / kTileH;
    std::vector<uint32_t> tile(static_cast<size_t>(kTileW) * kTileH);
    for (int r = 0; r < g_rows; ++r) {
        for (int c = 0; c < g_cols; ++c) {
            for (int y = 0; y < kTileH; ++y) {
                const int sy = std::min(r * kTileH + y, h - 1);
                const uint32_t* src = pixels.data() + static_cast<size_t>(sy) * w;
                uint32_t* dst = tile.data() + static_cast<size_t>(y) * kTileW;
                for (int x = 0; x < kTileW; ++x) dst[x] = src[std::min(c * kTileW + x, w - 1)] | 0xFF000000;
            }
            g_tiles.push_back(sprite::CreateTexture(kTileW, kTileH, tile.data(), kTileW));
        }
    }
}

}  // namespace

bool Open(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    struct Header {
        char magic[4];
        uint32_t version, pages, links, linksAt, stringsAt;
    } hdr{};
    struct Row {
        uint32_t key, title;
        uint16_t w, h;
        uint32_t pngAt, pngSize, firstLink, linkCount;
    };
    struct RawLink {
        int32_t target;
        uint16_t x, y, w, h;
    };
    static_assert(sizeof(Row) == 28 && sizeof(RawLink) == 12, "pack layout");
    bool ok = fread(&hdr, sizeof hdr, 1, f) == 1 && memcmp(hdr.magic, "VWEB", 4) == 0 &&
              (hdr.version == 1 || hdr.version == 2);
    std::vector<Row> rows(ok ? hdr.pages : 0);
    std::vector<RawLink> links(ok ? hdr.links : 0);
    std::vector<char> strings;
    if (ok) {
        ok = fread(rows.data(), sizeof(Row), rows.size(), f) == rows.size() &&
             ReadAt(f, hdr.linksAt, links.data(), links.size() * sizeof(RawLink));
        const uint32_t firstPng = rows.empty() ? hdr.stringsAt : rows.front().pngAt;
        strings.resize(firstPng - hdr.stringsAt + 1, 0);
        ok = ok && ReadAt(f, hdr.stringsAt, strings.data(), strings.size() - 1);
    }
    fclose(f);
    if (!ok) {
        logfile::Line("web: %s is not a page pack", path.c_str());
        return false;
    }
    auto str = [&](uint32_t at) { return at < strings.size() ? std::string(strings.data() + at) : std::string(); };

    g_path = path;
    g_pages.clear();
    g_links.clear();
    for (const Row& r : rows) {
        Entry e;
        const std::string key = str(r.key);
        const size_t slash = key.find('/');
        e.page.site = key.substr(0, slash);
        e.page.name = slash == std::string::npos ? "index" : key.substr(slash + 1);
        e.page.title = str(r.title);
        e.page.width = r.w;
        e.page.height = r.h;
        e.pngAt = r.pngAt;
        e.pngSize = r.pngSize;
        g_pages.push_back(e);
        std::vector<Link> pageLinks;
        for (uint32_t i = r.firstLink; i < r.firstLink + r.linkCount && i < links.size(); ++i) {
            const RawLink& l = links[i];
            // Version 2: a target below kBack is a link out, its address a
            // string at -(target) - 3.
            const bool out = hdr.version >= 2 && l.target <= kExternal;
            pageLinks.push_back({out ? kExternal : l.target, static_cast<float>(l.x), static_cast<float>(l.y),
                                 static_cast<float>(l.w), static_cast<float>(l.h),
                                 out ? str(static_cast<uint32_t>(-(l.target + 3))) : std::string()});
        }
        g_links.push_back(std::move(pageLinks));
    }

    // The sites, by name without the "www.", each opened at its index page.
    g_sites.clear();
    for (int i = 0; i < static_cast<int>(g_pages.size()); ++i) {
        const Page& p = g_pages[i].page;
        if (p.site == "hidden") continue;  // the internet café's own screens
        auto it = std::find_if(g_sites.begin(), g_sites.end(), [&](const Site& s) { return s.name == p.site; });
        if (it == g_sites.end()) {
            g_sites.push_back({p.site, p.title, i});
            it = g_sites.end() - 1;
        }
        if (p.name == "index") {
            it->home = i;
            if (!p.title.empty()) it->title = p.title;
        }
    }
    auto bare = [](const std::string& s) { return s.compare(0, 4, "www.") == 0 ? s.substr(4) : s; };
    std::sort(g_sites.begin(), g_sites.end(), [&](const Site& a, const Site& b) { return bare(a.name) < bare(b.name); });

    if (!g_thread.joinable()) {
        g_thread = std::thread(DecoderThread);
        g_thread.detach();
    }
    logfile::Line("web: %zu pages on %zu sites", g_pages.size(), g_sites.size());
    return true;
}

bool Available() { return !g_pages.empty(); }
int PageCount() { return static_cast<int>(g_pages.size()); }

const Page& Info(int page) {
    return page >= 0 && page < PageCount() ? g_pages[page].page : kEmpty;
}

int Find(const std::string& site, const std::string& name) {
    for (int i = 0; i < PageCount(); ++i) {
        if (g_pages[i].page.site == site && g_pages[i].page.name == name) return i;
    }
    return kNoPage;
}

const std::vector<Link>& Links(int page) {
    return page >= 0 && page < PageCount() ? g_links[page] : kNoLinks;
}

const std::vector<Site>& Sites() { return g_sites; }

bool Load(int page) {
    if (page < 0 || page >= PageCount()) return false;
    if (g_loaded == page) return true;
    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_decoded != page) {
            if (g_decoding != page && g_request != page) {
                g_request = page;
                g_wake.notify_one();
            }
            return false;
        }
        pixels = std::move(g_pixels);
        w = g_decodedW;
        h = g_decodedH;
        g_decoded = kNoPage;
    }
    Release();
    if (w > 0 && h > 0) Upload(pixels, w, h);
    g_loaded = page;
    return true;
}

void Draw(float left, float top, float scale, float viewTop, float viewBottom) {
    for (int r = 0; r < g_rows; ++r) {
        const float y0 = top + r * kTileH * scale, y1 = y0 + kTileH * scale;
        if (y1 < viewTop || y0 > viewBottom) continue;
        for (int c = 0; c < g_cols; ++c) {
            const float x0 = left + c * kTileW * scale;
            sprite::Draw(g_tiles[static_cast<size_t>(r) * g_cols + c], x0, y0, x0 + kTileW * scale, y1);
        }
    }
}

void Release() {
    for (uintptr_t t : g_tiles) sprite::DestroyTexture(t);
    g_tiles.clear();
    g_cols = g_rows = 0;
    g_loaded = kNoPage;
}

}  // namespace web
