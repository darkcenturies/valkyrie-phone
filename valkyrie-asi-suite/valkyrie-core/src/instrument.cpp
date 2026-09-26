#include "instrument.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"

namespace instrument {
namespace {

// The registry is a fixed-size block in a named mapping. Fixed size because it
// is shared across DLL boundaries and must have exactly one layout that every
// ASI agrees on - a growable structure would need an allocator all of them
// share, which is the thing separate heaps make impossible.
constexpr uint32_t kMagic = 0x564B4931;  // "VKI1"
constexpr uint32_t kVersion = 1;
constexpr size_t kMaxHooks = 64;
constexpr size_t kMaxNotes = 96;
constexpr size_t kOwnerLen = 32;
constexpr size_t kTextLen = 64;

struct HookEntry {
    char owner[kOwnerLen];
    char what[kTextLen];
    uint32_t site;
    uint32_t trampoline;
    volatile LONG fired;
    volatile LONG intact;
    volatile LONG used;  // claimed, so two ASIs cannot take the same slot
};

struct NoteEntry {
    char owner[kOwnerLen];
    char key[kTextLen];
    char value[kTextLen];
    volatile LONG used;
};

struct Block {
    volatile LONG magic;
    volatile LONG version;
    volatile LONG hookCount;
    volatile LONG noteCount;
    HookEntry hooks[kMaxHooks];
    NoteEntry notes[kMaxNotes];
};

HANDLE g_mapping = nullptr;
Block* g_block = nullptr;
bool g_tried = false;

Block* Registry() {
    if (g_block || g_tried) return g_block;
    g_tried = true;

    // Local\ rather than Global\ - this is one game process and its children,
    // not a machine-wide service, and Global needs a privilege we should not
    // be asking for.
    g_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, sizeof(Block),
                                   "Local\\SPRP.Instrument");
    if (!g_mapping) return nullptr;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;

    g_block = static_cast<Block*>(
        MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block)));
    if (!g_block) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return nullptr;
    }

    if (!existed) {
        // First ASI in wins the job of clearing it. A fresh mapping is already
        // zeroed by Windows, so this only stamps the header.
        InterlockedExchange(&g_block->magic, kMagic);
        InterlockedExchange(&g_block->version, kVersion);
    } else if (InterlockedCompareExchange(&g_block->magic, 0, 0) !=
               static_cast<LONG>(kMagic)) {
        // Something else owns a mapping of that name, or an older build left
        // one behind with a different layout. Writing into it would be writing
        // into a structure we do not know, so stand down instead.
        UnmapViewOfFile(g_block);
        CloseHandle(g_mapping);
        g_block = nullptr;
        g_mapping = nullptr;
    }
    return g_block;
}

// Which ASI is calling. Taken from the module this code is running inside
// rather than from a name each ASI passes in, so it cannot be forgotten, and
// cannot be wrong: every ASI links its own copy of this file, so the address
// of this function is inside the caller's own DLL.
const char* Owner() {
    static char name[kOwnerLen] = {};
    if (name[0]) return name;
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Owner), &self) &&
        self) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(self, path, sizeof path)) {
            const char* slash = strrchr(path, '\\');
            const char* file = slash ? slash + 1 : path;
            strncpy_s(name, file, _TRUNCATE);
            if (char* dot = strrchr(name, '.')) *dot = 0;
            return name;
        }
    }
    strncpy_s(name, "unknown", _TRUNCATE);
    return name;
}

// The module an address lives in, or a note that nothing claims it - which is
// what an allocated trampoline looks like, and is itself informative.
const char* ModuleAt(const void* address, char* out, size_t len) {
    HMODULE module = nullptr;
    if (address &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(module, path, sizeof path)) {
            const char* slash = strrchr(path, '\\');
            strncpy_s(out, len, slash ? slash + 1 : path, _TRUNCATE);
            return out;
        }
    }
    strncpy_s(out, len, "no module (an allocated trampoline)", _TRUNCATE);
    return out;
}

bool Readable(const void* address, size_t length = 8) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(address, &mbi, sizeof mbi) == 0 ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
        length > reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize -
                     reinterpret_cast<uintptr_t>(address)) {
        return false;
    }
    const DWORD protection = mbi.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY || protection == PAGE_READONLY ||
           protection == PAGE_READWRITE;
}

// Compare with this module's file, not with a guessed instruction prologue.
// Relative branches are relocation invariant. Absolute operands in a rebased
// module are deliberately reported as unverified instead of a detour.
int DiskMatch(uintptr_t address, size_t length) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module)) return -1;
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(module, path, MAX_PATH)) return -1;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") || !file) return -1;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    bool ok = fread(&dos, 1, sizeof dos, file) == sizeof dos &&
              dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
              fseek(file, dos.e_lfanew, SEEK_SET) == 0 &&
              fread(&nt, 1, sizeof nt, file) == sizeof nt &&
              nt.Signature == IMAGE_NT_SIGNATURE &&
              nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
              nt.FileHeader.NumberOfSections < 96;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (!ok || base != nt.OptionalHeader.ImageBase || length > 16) {
        fclose(file); return -1;
    }
    const uintptr_t rva = address - base;
    fseek(file, dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader, SEEK_SET);
    int result = -1;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (fread(&section, 1, sizeof section, file) != sizeof section) break;
        if (rva < section.VirtualAddress) continue;
        const uintptr_t offset = rva - section.VirtualAddress;
        if (offset > section.SizeOfRawData || length > section.SizeOfRawData - offset) continue;
        unsigned char bytes[16]{};
        if (fseek(file, static_cast<long>(section.PointerToRawData + offset), SEEK_SET) == 0 &&
            fread(bytes, 1, length, file) == length) {
            result = memcmp(bytes, reinterpret_cast<const void*>(address), length) == 0 ? 1 : 0;
        }
        break;
    }
    fclose(file);
    return result;
}

struct Probe { uintptr_t site; void* code; int id; };
std::vector<Probe> g_probes;

// The render path, in the order the game walks it. This is the list that would
// have answered the fuel gauge in one run instead of four.
const Subject kRenderPath[] = {
    {0x53E230, "Render2dStuff (the whole 2D pass)"},
    {0x53EB12, "Idle's call to Render2dStuff"},
    {0x53E4FF, "Render2dStuff's call to CHud::Draw"},
    {0x53EBA2, "Idle's call to DrawAfterFade"},
    {0x53EBB1, "Idle's final text flush (LIVE)"},
    {0x53E55F, "old fuel site (outside live path)"},
    {0x58FAE0, "CHud::Draw"},
    {0x58D490, "CHud::DrawAfterFade"},
    {0x71A210, "CFont::DrawFonts (the text flush)"},
    {0x71A700, "CFont::PrintString"},
    {0x727B60, "CSprite2d::DrawRect"},
    {0x586880, "CRadar::DrawRadarMap"},
    {0x53E981, "Idle's call to CGame::Process"},
};

}  // namespace

int WatchCallSite(const char* what, uintptr_t site) {
    for (const auto& probe : g_probes) if (probe.site == site) return probe.id;
    auto* call = reinterpret_cast<unsigned char*>(site);
    if (!Readable(call, 5) || call[0] != 0xE8 || g_probes.size() >= 16) return kNoHook;
    int32_t displacement = 0;
    memcpy(&displacement, call + 1, 4);
    const uintptr_t target = site + 5 + displacement;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(target), &mbi, sizeof mbi) ||
        mbi.State != MEM_COMMIT || !(mbi.Protect &
            (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return kNoHook;
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32,
                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code) return kNoHook;
    const int id = RegisterHook(what, site, code);
    if (id == kNoHook) { VirtualFree(code, 0, MEM_RELEASE); return id; }
    // pushfd; lock inc dword ptr [counter]; popfd; jmp original.
    // No general registers, x87 or SSE state are touched; arguments and the
    // original return address remain exactly where the callee expects them.
    code[0] = 0x9C; code[1] = 0xF0; code[2] = 0xFF; code[3] = 0x05;
    const uintptr_t counter = reinterpret_cast<uintptr_t>(&g_block->hooks[id].fired);
    memcpy(code + 4, &counter, 4);
    code[8] = 0x9D; code[9] = 0xE9;
    const uint32_t jump = static_cast<uint32_t>(target - reinterpret_cast<uintptr_t>(code + 14));
    memcpy(code + 10, &jump, 4);
    DWORD old = 0;
    if (!VirtualProtect(code, 32, PAGE_EXECUTE_READ, &old)) {
        SetHookIntact(id, false); VirtualFree(code, 0, MEM_RELEASE); return kNoHook;
    }
    FlushInstructionCache(GetCurrentProcess(), code, 32);
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) {
        SetHookIntact(id, false); VirtualFree(code, 0, MEM_RELEASE); return kNoHook;
    }
    const uint32_t relative = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(code) - site - 5);
    memcpy(call + 1, &relative, 4);
    DWORD ignored = 0;
    VirtualProtect(call, 5, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    g_probes.push_back({site, code, id});
    return id;
}

int RegisterHook(const char* what, uintptr_t site, const void* trampoline) {
    Block* block = Registry();
    if (!block) return kNoHook;

    for (size_t i = 0; i < kMaxHooks; ++i) {
        HookEntry& entry = block->hooks[i];
        if (InterlockedCompareExchange(&entry.used, 1, 0) != 0) continue;
        strncpy_s(entry.owner, Owner(), _TRUNCATE);
        strncpy_s(entry.what, what ? what : "?", _TRUNCATE);
        entry.site = static_cast<uint32_t>(site);
        entry.trampoline = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(trampoline));
        InterlockedExchange(&entry.fired, 0);
        InterlockedExchange(&entry.intact, 1);
        InterlockedIncrement(&block->hookCount);
        return static_cast<int>(i);
    }
    return kNoHook;
}

void HookFired(int id) {
    if (id < 0 || id >= static_cast<int>(kMaxHooks) || !g_block) return;
    InterlockedIncrement(&g_block->hooks[id].fired);
}

long HookExecutions(int id) {
    if (id < 0 || id >= static_cast<int>(kMaxHooks) || !g_block) return -1;
    return InterlockedCompareExchange(&g_block->hooks[id].fired, 0, 0);
}

void SetHookIntact(int id, bool intact) {
    if (id < 0 || id >= static_cast<int>(kMaxHooks) || !g_block) return;
    InterlockedExchange(&g_block->hooks[id].intact, intact ? 1 : 0);
}

void Note(const char* key, const char* value) {
    Block* block = Registry();
    if (!block || !key) return;
    const char* owner = Owner();

    // Replace this owner's existing note for the key, so a live figure stays
    // one line rather than becoming a history.
    for (size_t i = 0; i < kMaxNotes; ++i) {
        NoteEntry& note = block->notes[i];
        if (InterlockedCompareExchange(&note.used, 1, 1) != 1) continue;
        if (_stricmp(note.owner, owner) == 0 && _stricmp(note.key, key) == 0) {
            strncpy_s(note.value, value ? value : "", _TRUNCATE);
            return;
        }
    }
    for (size_t i = 0; i < kMaxNotes; ++i) {
        NoteEntry& note = block->notes[i];
        if (InterlockedCompareExchange(&note.used, 1, 0) != 0) continue;
        strncpy_s(note.owner, owner, _TRUNCATE);
        strncpy_s(note.key, key, _TRUNCATE);
        strncpy_s(note.value, value ? value : "", _TRUNCATE);
        InterlockedIncrement(&block->noteCount);
        return;
    }
}

void NoteNumber(const char* key, double value) {
    char text[kTextLen];
    sprintf_s(text, "%.2f", value);
    Note(key, text);
}

const Subject* RenderPath(size_t& count) {
    count = sizeof(kRenderPath) / sizeof(kRenderPath[0]);
    return kRenderPath;
}

void ReportOwnership(const Subject* subjects, size_t count) {
    if (!subjects) {
        subjects = RenderPath(count);
    }
    logfile::Line("--- who owns the game's code ---");
    for (size_t i = 0; i < count; ++i) {
        const Subject& subject = subjects[i];
        const auto* code = reinterpret_cast<const uint8_t*>(subject.address);
        if (!Readable(code)) {
            logfile::Line("  %-34s %08X  unreadable", subject.name,
                          subject.address);
            continue;
        }

        // A detour is a jump planted over the first bytes. Following it and
        // naming the module it lands in is what turns "my hook never runs"
        // into "PECore owns this function".
        const void* destination = nullptr;
        const int disk = DiskMatch(subject.address, 6);
        const char* kind = disk == 1 ? "matches file" :
                           disk == 0 ? "CHANGED from file" : "baseline unavailable";
        if (code[0] == 0xE9) {
            const int32_t rel = *reinterpret_cast<const int32_t*>(code + 1);
            destination = reinterpret_cast<const void*>(subject.address + 5 + rel);
            kind = disk == 1 ? "shipped jump, to" : disk == 0 ? "PATCHED jump, to" : "unverified jump, to";
        } else if (code[0] == 0xE8) {
            const int32_t rel = *reinterpret_cast<const int32_t*>(code + 1);
            destination = reinterpret_cast<const void*>(subject.address + 5 + rel);
            kind = disk == 1 ? "shipped call, to" : disk == 0 ? "PATCHED call, to" : "unverified call, to";
        } else if (code[0] == 0xFF && code[1] == 0x25) {
            const auto slot = *reinterpret_cast<const uintptr_t*>(code + 2);
            if (Readable(reinterpret_cast<const void*>(slot))) {
                destination = *reinterpret_cast<void* const*>(slot);
                kind = disk == 1 ? "shipped indirect jump, to" : "indirect jump (check baseline), to";
            }
        } else if (code[0] == 0xEB) {
            destination = reinterpret_cast<const void*>(
                subject.address + 2 + static_cast<int8_t>(code[1]));
            kind = disk == 1 ? "shipped short jump, to" : "short jump (check baseline), to";
        }

        char owner[MAX_PATH]{};
        if (destination) {
            logfile::Line("  %-34s %08X  %s %p in %s", subject.name,
                          subject.address, kind, destination,
                          ModuleAt(destination, owner, sizeof owner));
        } else {
            logfile::Line("  %-34s %08X  %s (%02X %02X %02X %02X %02X)",
                          subject.name, subject.address, kind, code[0], code[1],
                          code[2], code[3], code[4]);
        }
    }
}

bool AnySilentHooks() {
    Block* block = Registry();
    if (!block) return false;
    for (size_t i = 0; i < kMaxHooks; ++i) {
        const HookEntry& entry = block->hooks[i];
        if (InterlockedCompareExchange(
                const_cast<volatile LONG*>(&entry.used), 1, 1) != 1) {
            continue;
        }
        if (InterlockedCompareExchange(
                const_cast<volatile LONG*>(&entry.fired), 0, 0) == 0) {
            return true;
        }
    }
    return false;
}

void Report() {
    Block* block = Registry();
    if (!block) {
        logfile::Line("instruments: the shared registry is unavailable");
        return;
    }

    logfile::Line("--- hooks ---");
    bool anySilent = false;
    for (size_t i = 0; i < kMaxHooks; ++i) {
        const HookEntry& entry = block->hooks[i];
        if (InterlockedCompareExchange(
                const_cast<volatile LONG*>(&entry.used), 1, 1) != 1) {
            continue;
        }
        const LONG fired = InterlockedCompareExchange(
            const_cast<volatile LONG*>(&entry.fired), 0, 0);
        const LONG intact = InterlockedCompareExchange(
            const_cast<volatile LONG*>(&entry.intact), 0, 0);

        // The two states worth spelling out rather than leaving to be read off
        // a pair of numbers. "Installed but never ran" is the one that hides.
        const char* verdict = "ok";
        if (fired == 0 && intact) {
            verdict = "NO EXECUTIONS OBSERVED - inspect probes and game state; cause not established";
            anySilent = true;
        } else if (fired == 0) {
            verdict = "never ran; chain head changed or installation failed";
        } else if (!intact) {
            verdict = "has run; chain head changed (may still be chained)";
        }

        logfile::Line("  %-18s %-30s site %08X  ran %ld times  %s",
                      entry.owner, entry.what, entry.site, fired, verdict);
    }
    if (!anySilent) {
        logfile::Line("  (every registered hook has run at least once)");
    }

    if (InterlockedCompareExchange(&block->noteCount, 0, 0) > 0) {
        logfile::Line("--- what each part reports ---");
        for (size_t i = 0; i < kMaxNotes; ++i) {
            const NoteEntry& note = block->notes[i];
            if (InterlockedCompareExchange(
                    const_cast<volatile LONG*>(&note.used), 1, 1) != 1) {
                continue;
            }
            logfile::Line("  %-18s %-24s %s", note.owner, note.key, note.value);
        }
    }

    ReportOwnership(nullptr, 0);
}

}  // namespace instrument
