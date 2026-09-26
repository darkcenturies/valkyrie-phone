// Run one of the game's own script commands.
//
// Mission scripts do a great deal through opcodes that have no simple
// function behind them - a task handed to a ped goes through the event queue,
// not straight into a task slot, and getting that wrong crashes the game. So
// rather than rebuilding what an opcode does, this hands the opcode to the
// script interpreter itself (CRunningScript::ProcessOneCommand), exactly as a
// line of main.scm would run. It is the same approach as plugin-sdk's
// Command<>.
//
// Game thread only.
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace script {

struct Arg {
    enum class Kind { Int, Float, Local, Text } kind;
    int32_t i;
    float f;
    const char* text = nullptr;
    Arg(int v) : kind(Kind::Int), i(v), f(0.0f) {}
    Arg(bool v) : kind(Kind::Int), i(v ? 1 : 0), f(0.0f) {}
    Arg(float v) : kind(Kind::Float), i(0), f(v) {}
    // A string, as an animation or its file is named. Up to 255 characters.
    Arg(const char* v) : kind(Kind::Text), i(0), f(0.0f), text(v) {}
    // A local variable (0@ to 31@), for what an opcode stores: a new car's
    // handle, a position. Read it back from the Locals the call fills in.
    static Arg Local(int index) {
        Arg a(index);
        a.kind = Kind::Local;
        return a;
    }
};

// The script's local variables after a command, as ints or floats.
struct Locals {
    int32_t raw[32] = {};
    int Int(int index) const { return raw[index]; }
    float Float(int index) const {
        float f;
        static_assert(sizeof f == sizeof raw[0], "local size");
        memcpy(&f, &raw[index], sizeof f);
        return f;
    }
};

// Run `opcode` with these arguments. Returns the command's condition result,
// for opcodes that are checks. Numbers, strings and locals; `out`, when given,
// receives the locals afterwards.
bool Command(uint16_t opcode, std::initializer_list<Arg> args, Locals* out = nullptr);

// The script handle for a ped (CPools::GetPedRef), which is what opcodes take
// in place of a pointer.
int PedHandle(uintptr_t ped);

}  // namespace script
