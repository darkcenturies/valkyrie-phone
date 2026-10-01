#include "coverage.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#include "game.h"
#include "log.h"

namespace coverage {
namespace {

// CVector FindPlayerCoors(int player), through a hidden pointer.
constexpr uintptr_t kFindPlayerCoors = 0x56E010;
constexpr uintptr_t kCurrArea = 0xB72914;       // CGame::currArea: an interior
constexpr uintptr_t kUnderWaterness = 0xC8132C;  // CWeather::UnderWaterness

// Path loss, in dB, at which the phone shows 5 to 1 bars; past the last it
// has no service (150 dB, a usual GSM limit).
constexpr float kBars[5] = {115.0f, 123.0f, 131.0f, 140.0f, 150.0f};
constexpr float kIndoors = 16.0f;  // through walls
constexpr float kInCar = 6.0f;     // through a car's body
constexpr float kHysteresis = 1.5f;

struct Grid {
    int x0 = 0, y1 = 0, res = 100, w = 0, h = 0;
    std::vector<uint8_t> loss;  // dB, 255 where no mast reaches
};
Grid g_grid;
bool g_loaded = false;

float g_level = 100.0f;  // the loss where CJ is, with the fading
int g_bars = 5;
float g_fade = 0.0f, g_fadeTo = 0.0f;
ULONGLONG g_fadeAt = 0, g_last = 0;
std::mt19937 g_rng(GetTickCount());

float Cell(int ix, int iy) {
    if (ix < 0 || iy < 0 || ix >= g_grid.w || iy >= g_grid.h) return 255.0f;
    return g_grid.loss[static_cast<size_t>(iy) * g_grid.w + ix];
}

// The loss at a point, between the four cells round it.
float LossAt(float x, float y) {
    const float fx = (x - g_grid.x0) / g_grid.res - 0.5f, fy = (g_grid.y1 - y) / g_grid.res - 0.5f;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
    const float tx = fx - ix, ty = fy - iy;
    const float a = Cell(ix, iy), b = Cell(ix + 1, iy), c = Cell(ix, iy + 1), d = Cell(ix + 1, iy + 1);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

int BarsFor(float level) {
    for (int i = 0; i < 5; ++i) {
        if (level <= kBars[i]) return 5 - i;
    }
    return 0;
}

}  // namespace

bool Load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto i32 = [&](size_t at) {
        int32_t v = 0;
        if (at + 4 <= b.size()) memcpy(&v, &b[at], 4);
        return v;
    };
    if (b.size() < 32 || memcmp(b.data(), "VSIG", 4) != 0 || i32(4) != 1) {
        logfile::Line("signal: no valkyrie-signal.bin - full signal everywhere");
        return false;
    }
    Grid g;
    g.x0 = i32(8);
    g.y1 = i32(12);
    g.res = std::max(1, i32(16));
    g.w = i32(20);
    g.h = i32(24);
    const int count = i32(28);
    const size_t cells = static_cast<size_t>(g.w) * g.h;
    size_t at = 32;
    const char* want = "stock";
    for (int n = 0; n < count && at + 8 + cells <= b.size(); ++n, at += 8 + cells) {
        if (strncmp(reinterpret_cast<const char*>(&b[at]), want, 8) != 0) continue;
        g.loss.assign(b.begin() + at + 8, b.begin() + at + 8 + cells);
        g_grid = g;
        g_loaded = true;
        logfile::Line("signal: %s masts, %d x %d cells", want, g.w, g.h);
        return true;
    }
    logfile::Line("signal: no %s grid in valkyrie-signal.bin - full signal everywhere", want);
    return false;
}

void Update(bool enabled) {
    const ULONGLONG now = GetTickCount64();
    const float dt = g_last ? std::min(0.25f, (now - g_last) / 1000.0f) : 0.0f;
    g_last = now;
    if (!enabled || !g_loaded) {
        g_level = 100.0f;
        g_bars = 5;
        return;
    }
    game::Vector at{};
    reinterpret_cast<game::Vector*(__cdecl*)(game::Vector*, int)>(kFindPlayerCoors)(&at, -1);
    float level = LossAt(at.x, at.y);
    if (*reinterpret_cast<const int*>(kCurrArea) != 0) level += kIndoors;
    game::VehicleState vehicle{};
    if (game::PlayerVehicleState(vehicle)) level += kInCar;
    // High up - a rooftop, a plane - the hills are no longer in the way.
    level -= std::clamp((at.z - 80.0f) / 15.0f, 0.0f, 12.0f);
    if (*reinterpret_cast<const float*>(kUnderWaterness) > 0.0f) level = 255.0f;
    // The signal drifts: a new lean every second or two, eased into, now
    // and then a deeper dip.
    if (now >= g_fadeAt) {
        std::normal_distribution<float> drift(0.0f, 2.2f);
        g_fadeTo = std::clamp(drift(g_rng), -5.0f, 5.0f);
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng) < 0.06f) g_fadeTo += 9.0f;
        g_fadeAt = now + static_cast<ULONGLONG>(std::uniform_real_distribution<float>(800.0f, 2200.0f)(g_rng));
    }
    g_fade += (g_fadeTo - g_fade) * std::min(1.0f, dt / 1.2f);
    g_level = level + g_fade;
    // Bars move only once the level is clearly past a step.
    const int up = BarsFor(g_level + kHysteresis), down = BarsFor(g_level - kHysteresis);
    if (up > g_bars) g_bars = up;
    else if (down < g_bars) g_bars = down;
}

int Bars() { return g_bars; }
bool HasService() { return g_bars > 0; }
float Quality() { return g_bars > 0 ? std::clamp((150.0f - g_level) / 35.0f, 0.05f, 1.0f) : 0.0f; }

}  // namespace coverage
