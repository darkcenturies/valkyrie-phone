#include "DeviceHooks.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
void* table[43]{};
struct FakeDevice { void** vtable = table; } mainDevice, isolatedDevice;
DeviceHooks hooks;
int resets = 0, draws = 0, owned = 0;
HRESULT __stdcall ShimReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS*) {
    // AppHelp resolves its saved target using this exact vtable identity.
    assert(*reinterpret_cast<void***>(device) == table);
    ++resets; return S_FALSE;
}
HRESULT __stdcall OriginalDraw(IDirect3DDevice9*) { ++draws; return S_OK; }
HRESULT __stdcall Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    if (device == hooks.owner) ++owned;
    return hooks.reset(device, pp);
}
HRESULT __stdcall Draw(IDirect3DDevice9* device) {
    if (device == hooks.owner) ++owned;
    return hooks.endScene(device);
}
int main() {
    table[16] = reinterpret_cast<void*>(&ShimReset);
    table[42] = reinterpret_cast<void*>(&OriginalDraw);
    auto* main = reinterpret_cast<IDirect3DDevice9*>(&mainDevice);
    auto* isolated = reinterpret_cast<IDirect3DDevice9*>(&isolatedDevice);
    assert(!hooks.Install(nullptr, Draw, Reset));
    assert(hooks.Install(main, Draw, Reset));
    assert(mainDevice.vtable == table && isolatedDevice.vtable == table);
    assert(!hooks.Install(main, Draw, Reset));
    for (auto* device : {main, isolated}) {
        assert(reinterpret_cast<DeviceHooks::Reset>(table[16])(device, nullptr) == S_FALSE);
        assert(reinterpret_cast<DeviceHooks::EndScene>(table[42])(device) == S_OK);
    }
    assert(resets == 2 && draws == 2 && owned == 2);
    puts("PASS: device vtable identity, existing reset chain, and isolated-device bypass");
}
