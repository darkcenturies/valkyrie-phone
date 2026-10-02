// A texture that is initially unavailable must be retried on a later draw.
#include "ui.h"
#include "app_icon.h"
#include <cassert>
#include <cstdio>

namespace {
int lookups = 0, draws = 0;
bool available = false;
}
namespace sprite {
uintptr_t Find(int, const char*) { ++lookups; return available ? 42 : 0; }
void Draw(uintptr_t texture, float, float, float, float, uint32_t) {
    assert(texture == 42);
    ++draws;
}
void Quad(uintptr_t, Corner, Corner, Corner, Corner, uint32_t) { assert(false); }
void Gradient(float, float, float, float, uint32_t, uint32_t) { assert(false); }
void Text(float, float, const char*, const TextStyle&) { assert(false); }
float Width(const char*, const TextStyle&) { assert(false); return 0; }
std::string Fit(const char*, float, const TextStyle&) { assert(false); return {}; }
void Flush() { assert(false); }
}
int main() {
    assert(phone_icons::Builtin("Maps") == "app_maps");
    assert(phone_icons::Builtin("Camera") == "app_photos");
    ui::SetScreen(0, 0, 1);
    assert(!ui::GameImage(1, "radar_centre", 0, 0, 32, 32));
    assert(lookups == 1 && draws == 0);
    available = true;
    assert(ui::GameImage(1, "radar_centre", 0, 0, 32, 32));
    assert(lookups == 2 && draws == 1);
    assert(ui::GameImage(1, "radar_centre", 0, 0, 32, 32));
    assert(lookups == 2 && draws == 2);
    puts("PASS: HUD icons recover after a failed first lookup and successful results are cached");
}
