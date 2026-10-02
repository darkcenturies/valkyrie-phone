#pragma once
#include "bundle.h"
#include "log.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>

// SA 1.0 US layouts/entry points documented by plugin-sdk and gta-reversed:
// Animation/AnimManager, AnimBlock, AnimBlendAssociation. Authored clips are
// upper-body associations; they do not replace a ped task or its locomotion.
namespace phone_animation {
struct Association {
    uintptr_t vtable, next, prev;
    uint16_t nodes, group;
    uintptr_t blendNodes, hierarchy;
    float amount, delta, time, speed, step;
    uint16_t id, flags;
};
static_assert(offsetof(Association, delta) == 0x1C);
static_assert(offsetof(Association, flags) == 0x2E);
struct Block {
    char name[16];
    bool loaded;
    uint8_t padding;
    int16_t references;
    int32_t first;
    uint32_t count;
    int32_t group;
};
static_assert(sizeof(Block) == 0x20);
constexpr const char* kBlock = "vp_phone";
constexpr uint32_t kClipCount = 18;
inline bool Bundled(const std::string& file) { return _stricmp(file.c_str(), kBlock) == 0; }

inline bool Ready() {
    auto get = reinterpret_cast<Block*(__cdecl*)(const char*)>(0x4D3940);
    Block* block = get(kBlock);
    if (block && block->loaded) return block->count == kClipCount;
    if (*reinterpret_cast<const uint32_t*>(0xB4EA30) >= 180 ||
        *reinterpret_cast<const int32_t*>(0xB4EA2C) > 2500 - static_cast<int>(kClipCount)) return false;
    const std::string path = bundle::Path("valkyrie-phone.ifp");
    if (path.empty()) return false;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file.tellg();
    if (size < 36 || size > 512 * 1024) return false;
    char header[36]{};
    file.seekg(0); file.read(header, sizeof header);
    uint32_t bytes{}, clips{};
    memcpy(&bytes, header + 4, 4); memcpy(&clips, header + 32, 4);
    if (!file || memcmp(header, "ANP3", 4) || bytes != size - 8 ||
        memcmp(header + 8, kBlock, 9) || clips != kClipCount) return false;
    void* stream = reinterpret_cast<void*(__cdecl*)(int, int, const void*)>(0x7ECEF0)(2, 1, path.c_str());
    if (!stream) return false;
    reinterpret_cast<void(__cdecl*)(void*, bool, const char(*)[32])>(0x4D47F0)(stream, false, nullptr);
    reinterpret_cast<int(__cdecl*)(void*, void*)>(0x7ECE20)(stream, nullptr);
    block = get(kBlock);
    if (!block || !block->loaded || block->count != kClipCount) return false;
    const int index = reinterpret_cast<int(__cdecl*)(const char*)>(0x4D3990)(kBlock);
    // Keep the authored block available across crossfades. Game shutdown owns
    // its lifetime; removing the block while associations fade would dangle.
    if (index >= 0) reinterpret_cast<void(__cdecl*)(int)>(0x4D3FB0)(index);
    logfile::Line("phone: loaded %u authored phone animations", kClipCount);
    return true;
}

inline Association* Play(uintptr_t clump, const std::string& name, bool loop) {
    if (!clump || !Ready()) return nullptr;
    Block* block = reinterpret_cast<Block*(__cdecl*)(const char*)>(0x4D3940)(kBlock);
    void* hierarchy = reinterpret_cast<void*(__cdecl*)(const char*, const Block*)>(0x4D42F0)(name.c_str(), block);
    if (!hierarchy) return nullptr;
    // AddAnimation, rather than BlendAnimation, leaves unrelated associations
    // alone. The caller fades only its previous, verified association.
    auto* a = reinterpret_cast<Association*(__cdecl*)(uintptr_t, void*, int)>(0x4D4330)(
        clump, hierarchy, 0x11 | (loop ? 0x2 : 0));
    if (a) { a->amount = 0.0f; a->delta = 6.0f; a->speed = 1.0f; }
    return a;
}
inline Association* Find(uintptr_t clump, const std::string& name, uintptr_t expected) {
    if (!clump || !expected) return nullptr;
    auto* a = reinterpret_cast<Association*(__cdecl*)(uintptr_t, const char*)>(0x4D6870)(clump, name.c_str());
    return reinterpret_cast<uintptr_t>(a) == expected && a->delta >= 0.0f ? a : nullptr;
}
inline float Progress(const Association* a) {
    if (!a || !a->hierarchy) return 0.0f;
    const float duration = *reinterpret_cast<const float*>(a->hierarchy + 0x10);
    return duration > 0.0f ? a->time / duration : 0.0f;
}
inline void Hold(Association* a, bool seekEnd = false) {
    if (!a) return;
    if (seekEnd && a->hierarchy)
        reinterpret_cast<void(__thiscall*)(Association*, float)>(0x4CEA80)(
            a, *reinterpret_cast<const float*>(a->hierarchy + 0x10) * 0.96f);
    a->flags &= ~0x1; // Pause our association only.
}
} // namespace phone_animation
