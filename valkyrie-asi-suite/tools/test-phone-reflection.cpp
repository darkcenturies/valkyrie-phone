#include "scene_capture.h"
#include <cassert>
#include <cstdio>

int frame = 0, captured = -1, hudCalls = 0;
bool hudVisible = false;
void __cdecl Capture() { assert(!hudVisible); captured = frame; }
void __cdecl Hud() { assert(captured == frame); hudVisible = true; ++hudCalls; }

int main() {
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    assert(code);
    const auto address = reinterpret_cast<std::uintptr_t>(code);
    code[0] = 0x90;
    assert(!scene_capture::RegisterAt(address, &Capture));
    code[0] = 0xE8;
    auto relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&Hud) - address - 5);
    std::memcpy(code + 1, &relative, 4);
    code[5] = 0xC3;
    assert(!scene_capture::RegisterAt(address, nullptr));
    assert(scene_capture::RegisterAt(address, &Capture));
    assert(captured == -1 && hudCalls == 0);
    assert(!scene_capture::RegisterAt(address, &Capture));
    const auto invoke = reinterpret_cast<void (__cdecl*)()>(code);
    for (frame = 0; frame < 3; ++frame) {
        hudVisible = false;
        invoke();
        assert(hudVisible && captured == frame && hudCalls == frame + 1);
    }
    VirtualFree(code, 0, MEM_RELEASE);
    std::puts("PASS: each world capture precedes HUD/radar drawing and chains the previous renderer exactly once.");
}
