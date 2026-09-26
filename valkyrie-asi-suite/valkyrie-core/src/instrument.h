// One log that knows what every part of the suite is doing.
//
// WHY THIS EXISTS
//
// Each ASI already logs its own startup. What none of them could answer, and
// what has cost days, is the question one step further on:
//
//     "The hook installed. Is it RUNNING?"
//
// Those are different questions and they look identical from outside. A hook
// can report success, still hold the call site an hour later, and never once
// execute - because another component detoured the FUNCTION that call site
// lives inside, and the jump goes over the whole body. The call site keeps our
// address. Nothing errors. Nothing logs. The feature is simply absent, and
// every investigation starts from scratch with a hand-written probe.
//
// So the three things worth knowing are recorded permanently, by the code that
// installs the hooks rather than by each ASI remembering to:
//
//   * every hook in the process - who owns it, where it is, is it installed
//   * how many times each one has actually FIRED
//   * who owns each interesting game function - unpatched, or detoured to
//     which module
//
// WHY SHARED MEMORY
//
// The suite is eight separate DLLs with eight separate heaps. A registry in
// one of them is invisible to the others, so it lives in a named mapping that
// every ASI opens. Any of them can publish; the diagnostics ASI reports the
// lot. Nothing here needs the others to be loaded, or loaded in any order.
//
// WHY NOT AN IN-GAME OVERLAY
//
// Because drawing is one of the things that breaks, and a debugger that needs
// the render path in order to report on the render path is useless exactly
// when it is needed. The log is the channel that works when nothing else does.
#pragma once

#include <cstdint>

namespace instrument {

// An invalid hook id. Registering into a full or unavailable registry returns
// this, and every call taking an id ignores it, so a caller never has to check.
constexpr int kNoHook = -1;

// Record a hook. `what` is what it hooks in plain words ("the frame", "the HUD
// text flush") rather than a function name - the log is read by somebody
// trying to work out why a feature is missing.
//
// The owning ASI is worked out from the caller's own module, so an ASI cannot
// forget to identify itself or get it wrong.
int RegisterHook(const char* what, uintptr_t site, const void* trampoline);

// Called from inside the hook, every time it runs. This is the whole point of
// the registry, so it has to be cheap enough that nobody is tempted to leave
// it out: one interlocked increment, no logging, no branching on state.
void HookFired(int id);
long HookExecutions(int id);

// Whether the call site still points where we put it. Cheap; call it on a
// timer rather than per frame.
void SetHookIntact(int id, bool intact);

// Anything else worth having in the panel - "stations", "53". Replaces the
// value if the same owner writes the same key again, so a live figure can be
// kept current rather than accumulating.
void Note(const char* key, const char* value);
void NoteNumber(const char* key, double value);

// ---------------------------------------------------------------------------
// Execution probes: "does this line of code ever run?"
// ---------------------------------------------------------------------------

// Count how often a call site executes, without changing what it does.
//
// This is the tool for the case the ownership list cannot explain: a call site
// that holds our hook, inside a function nothing has patched, that still never
// runs - because the function returns before reaching it. Watching several
// sites down a function shows exactly how far execution gets.
//
// The probe is a naked trampoline that saves everything, counts, restores, and
// jumps to the original, so it is transparent to any calling convention and to
// any arguments. It is still a patched call site: use it to find something,
// not to leave in.
//
// Returns a hook id that Report() lists like any other, or kNoHook if the site
// is not a five-byte call or all the probe slots are taken.
int WatchCallSite(const char* what, uintptr_t site);

// A game function worth knowing the owner of.
struct Subject {
    uintptr_t address;
    const char* name;
};

// Read who owns each of these right now and write it to the log: unpatched, or
// jumped somewhere else, with the module it lands in named. Reads only - it
// patches nothing and is safe to call at any time.
void ReportOwnership(const Subject* subjects, size_t count);

// The render path, which is what goes wrong most often. Passing nothing to
// ReportOwnership uses this.
const Subject* RenderPath(size_t& count);

// Write the whole panel - every hook, whether it has fired, the notes, and the
// render path - to whichever log the calling ASI has open.
void Report();

// True when a hook is registered as installed but has never fired. This is the
// single most useful question in the whole file, so it is worth asking
// directly rather than reading it out of a table by eye.
bool AnySilentHooks();

}  // namespace instrument
