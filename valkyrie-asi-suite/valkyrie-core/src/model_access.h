#pragma once
#include <windows.h>
#include <cstdint>

namespace valkyrie_models {
// Call only after valkyrie_layout::Compatible10(). These are plugin-sdk's
// CModelInfo operands in CStreaming::RequestModelStream, not the stock array.
// Resolve at use time so a late limit-adjuster relocation is respected.
inline void* Find(int id) {
    if (id < 0) return nullptr;
    int count = 0;
    uintptr_t table = 0;
    SIZE_T read = 0;
    const auto process = GetCurrentProcess();
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(0x40CD58), &count, sizeof count, &read)
        || read != sizeof count || id >= count || count > 1000000) return nullptr;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(0x40CD67), &table, sizeof table, &read)
        || read != sizeof table || !table) return nullptr;
    void* model = nullptr;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(table + sizeof(void*) * id),
        &model, sizeof model, &read) || read != sizeof model) return nullptr;
    return model;
}
}
