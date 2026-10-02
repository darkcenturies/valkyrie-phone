// A texture that is initially unavailable must be retried on a later draw.
#include "ui.h"
#include "app_icon.h"
#include "config.h"
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
int lookups = 0, draws = 0;
bool available = false;
struct Band { float top, bottom; uint32_t colour; };
std::vector<Band> bands;
config::Config settings;
std::string lastTexture;
}
namespace config { const Config& Get() { return settings; } }
namespace sprite {
uintptr_t Find(int, const char* name) { ++lookups; lastTexture = name; return available ? 42 : 0; }
void Draw(uintptr_t texture, float, float, float, float, uint32_t) {
    assert(texture == 42);
    ++draws;
}
void Quad(uintptr_t, Corner, Corner, Corner, Corner, uint32_t) { assert(false); }
void Gradient(float, float top, float, float bottom, uint32_t a, uint32_t b) {
    assert(a == b);
    bands.push_back({top, bottom, a});
}
void Text(float, float, const char*, const TextStyle&) { assert(false); }
float Width(const char*, const TextStyle&) { assert(false); return 0; }
std::string Fit(const char*, float, const TextStyle&) { assert(false); return {}; }
void Flush() { assert(false); }
}
int main() {
    assert(phone_icons::Builtin("Maps") == "app_maps");
    assert(phone_icons::Builtin("Camera") == "app_camera");
    assert(phone_icons::Builtin("Photos") == "app_photos");
    assert(phone_icons::UpgradeDefault("Camera", "app_photos") == "app_camera");
    assert(phone_icons::UpgradeDefault("Photos", "hud:radar_1hourphoto") == "app_photos");
    assert(phone_icons::UpgradeDefault("Settings", "hud:radar_modGarage") == "hud:radar_modGarage");
    assert(phone_icons::UpgradeDefault("Settings", "custom_wrench") == "custom_wrench");
    assert(phone_icons::UpgradeDefault("Photos", "custom_gallery") == "custom_gallery");
    ui::SetScreen(0, 0, 1);
    assert(!ui::GameImage(1, "radar_centre", 0, 0, 32, 32));
    assert(lookups == 1 && draws == 0);
    available = true;
    assert(ui::GameImage(1, "radar_centre", 0, 0, 32, 32));
    assert(lookups == 2 && draws == 1);
    assert(ui::GameImage(1, "radar_centre", 0, 0, 32, 32));
    assert(lookups == 2 && draws == 2);
    ui::SetDictionary(3);
    assert(ui::Tex("app_photos") == 42 && lastTexture == "app_photos");
    settings.iconSize = 64;
    assert(ui::Tex("app_photos") == 42 && lastTexture == "app_photos_64");
    assert(ui::Tex("g_camera") == 42 && lastTexture == "g_camera_64");
    settings.iconSize = 16;
    const int cached = lookups;
    assert(ui::Tex("app_photos") == 42 && lookups == cached);
    ui::Gradient(0, 0, 40, 40, 0xFF000000, 0xFFFFFFFF);
    assert(bands.size() == 4 && bands[0].colour == 0xFF000000 && bands[3].colour == 0xFFFFFFFF);
    bands.clear();
    ui::Clip(12, 28);
    ui::Gradient(0, 0, 40, 40, 0xFF000000, 0xFFFFFFFF);
    assert(bands.size() == 2);
    assert(bands[0].top == 12 && bands[0].bottom == 20 && bands[0].colour == 0xFF555555);
    assert(bands[1].top == 20 && bands[1].bottom == 28 && bands[1].colour == 0xFFAAAAAA);
    bands.clear();
    ui::NoClip();
    ui::Fill(0, 0, 40, 40, 0xFF112233);
    assert(bands.size() == 1 && bands[0].colour == 0xFF112233);
    puts("PASS: HUD icons recover after a failed first lookup and successful results are cached");
}
