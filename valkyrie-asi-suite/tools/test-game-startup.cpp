#include "game_startup.h"
#include <cassert>

int calls = 0, starts = 0;
bool result = true;
bool __cdecl Previous() { ++calls; return result; }
DWORD WINAPI Start(LPVOID) { ++starts; assert(game_startup::pending == nullptr); return 0; }

int main() {
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    assert(code);
    const auto address = reinterpret_cast<std::uintptr_t>(code);
    code[0] = 0x90;
    assert(!game_startup::RegisterAt(address, &Start));
    code[0] = 0xE8;
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&Previous) - address - 5);
    std::memcpy(code + 1, &relative, 4);
    code[5] = 0xC3;
    assert(!game_startup::RegisterAt(address, nullptr));
    assert(game_startup::RegisterAt(address, &Start));
    assert(starts == 0); // Registration cannot run game logic under the loader lock.
    assert(!game_startup::RegisterAt(address, &Start));
    const auto invoke = reinterpret_cast<bool (__cdecl*)()>(code);
    assert(invoke());
    assert(calls == 1 && starts == 1);
    result = false;
    assert(!invoke()); // Keep original return and chain on subsequent initialization.
    assert(calls == 2 && starts == 1);
    VirtualFree(code, 0, MEM_RELEASE);
}
