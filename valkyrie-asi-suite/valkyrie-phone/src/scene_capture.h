#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

// SA 1.0 US Idle calls Render2dStuff at 0x53EB12. Copy the world before
// entering that entire pass, including radar replacements and other HUD mods.
// Keep the existing target so their render callbacks still run normally.
namespace scene_capture {
using Callback = void (__cdecl*)();
inline Callback original = nullptr;
inline Callback capture = nullptr;

inline void __cdecl Dispatch() {
    if (capture) capture();
    if (original) original();
}

inline bool RegisterAt(std::uintptr_t address, Callback callback) {
    if (!callback || original) return false;
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &page, sizeof(page)) ||
        page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        address + 5 > reinterpret_cast<std::uintptr_t>(page.BaseAddress) + page.RegionSize)
        return false;
    auto* site = reinterpret_cast<unsigned char*>(address);
    if (*site != 0xE8) return false;
    std::int32_t displacement{};
    std::memcpy(&displacement, site + 1, sizeof(displacement));
    const auto target = address + 5 + displacement;
    MEMORY_BASIC_INFORMATION code{};
    if (!VirtualQuery(reinterpret_cast<void*>(target), &code, sizeof(code)) ||
        code.State != MEM_COMMIT || (code.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(code.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        target == reinterpret_cast<std::uintptr_t>(&Dispatch)) return false;
    DWORD previous{};
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &previous)) return false;
    original = reinterpret_cast<Callback>(target);
    capture = callback;
    const auto replacement = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&Dispatch) - address - 5);
    std::memcpy(site + 1, &replacement, sizeof(replacement));
    DWORD ignored{};
    VirtualProtect(site, 5, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

inline bool Register(Callback callback) { return RegisterAt(0x53EB12, callback); }
}
