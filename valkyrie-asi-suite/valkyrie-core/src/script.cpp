#include "script.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace script {
namespace {

// CRunningScript, v1.0 US: 0xE0 bytes. Init resets it; ProcessOneCommand reads
// the opcode at m_pCurrentIP (+0x14) and its parameters after it.
constexpr size_t kRunningScriptSize = 0xE0;
constexpr size_t kName = 0x08;
constexpr size_t kBaseIP = 0x10;
constexpr size_t kCurrentIP = 0x14;
constexpr size_t kLocals = 0x3C;  // 32 of them; 32@ and 33@, the timers, follow
constexpr size_t kCondResult = 0xC5;
constexpr size_t kUseMissionCleanup = 0xC6;
constexpr size_t kNotFlag = 0xD2;
constexpr size_t kIsMission = 0xDC;
constexpr uintptr_t kInit = 0x4648E0;
constexpr uintptr_t kProcessOneCommand = 0x469EB0;
constexpr uintptr_t kGetPedRef = 0x54FF60;

// Parameter type bytes in compiled script.
constexpr uint8_t kParamEnd = 0;
constexpr uint8_t kParamInt32 = 1;
constexpr uint8_t kParamLocal = 3;  // followed by the variable's index, 16 bits
constexpr uint8_t kParamFloat = 6;
constexpr uint8_t kParamText = 0x0E;  // a length byte, then that many characters

}  // namespace

bool Command(uint16_t opcode, std::initializer_list<Arg> args, Locals* out) {
    std::vector<uint8_t> code;
    auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        code.insert(code.end(), b, b + n);
    };
    put(&opcode, 2);
    for (const Arg& a : args) {
        if (a.kind == Arg::Kind::Local) {
            code.push_back(kParamLocal);
            const uint16_t index = static_cast<uint16_t>(a.i);
            put(&index, 2);
        } else if (a.kind == Arg::Kind::Text) {
            const size_t n = a.text ? std::min<size_t>(strlen(a.text), 255) : 0;
            code.push_back(kParamText);
            code.push_back(static_cast<uint8_t>(n));
            put(a.text, n);
        } else if (a.kind == Arg::Kind::Int) {
            code.push_back(kParamInt32);
            put(&a.i, 4);
        } else {
            code.push_back(kParamFloat);
            put(&a.f, 4);
        }
    }
    // Commands with a variable number of arguments stop at this; the rest
    // never read it.
    code.push_back(kParamEnd);

    alignas(4) uint8_t running[kRunningScriptSize];
    memset(running, 0, sizeof running);
    reinterpret_cast<void(__thiscall*)(void*)>(kInit)(running);
    memcpy(running + kName, "valkyri", 8);
    *reinterpret_cast<bool*>(running + kIsMission) = false;
    *reinterpret_cast<bool*>(running + kUseMissionCleanup) = false;
    *reinterpret_cast<bool*>(running + kNotFlag) = (opcode & 0x8000) != 0;
    *reinterpret_cast<uint8_t**>(running + kBaseIP) = code.data();
    *reinterpret_cast<uint8_t**>(running + kCurrentIP) = code.data();
    reinterpret_cast<void(__thiscall*)(void*)>(kProcessOneCommand)(running);
    if (out) memcpy(out->raw, running + kLocals, sizeof out->raw);
    return *reinterpret_cast<bool*>(running + kCondResult);
}

int PedHandle(uintptr_t ped) {
    return reinterpret_cast<int(__cdecl*)(uintptr_t)>(kGetPedRef)(ped);
}

}  // namespace script
