#pragma once
#include <windows.h>
#include <d3d9.h>

// Windows compatibility shims find the real Reset by the device's vtable
// address. Preserve that address and chain its existing entries in place.
// The slots are shared: callbacks must bypass devices other than owner.
struct DeviceHooks {
    using EndScene = HRESULT (__stdcall*)(IDirect3DDevice9*);
    using Reset = HRESULT (__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    IDirect3DDevice9* owner = nullptr;
    EndScene endScene = nullptr;
    Reset reset = nullptr;

    bool Install(IDirect3DDevice9* device, EndScene draw, Reset recover) {
        if (owner || !device || !draw || !recover) return false;
        auto** table = *reinterpret_cast<void***>(device);
        if (!table || !table[16] || !table[42]) return false;
        DWORD previous{};
        constexpr SIZE_T bytes = (42 - 16 + 1) * sizeof(void*);
        if (!VirtualProtect(&table[16], bytes, PAGE_EXECUTE_READWRITE, &previous)) return false;
        endScene = reinterpret_cast<EndScene>(table[42]);
        reset = reinterpret_cast<Reset>(table[16]);
        owner = device;
        table[42] = reinterpret_cast<void*>(draw);
        table[16] = reinterpret_cast<void*>(recover);
        DWORD ignored{};
        VirtualProtect(&table[16], bytes, previous, &ignored);
        return true;
    }
};
