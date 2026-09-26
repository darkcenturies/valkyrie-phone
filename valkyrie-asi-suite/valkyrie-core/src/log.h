// A log file next to the game.
//
// A client plugin has nowhere to print. When something goes wrong for a player
// the only thing we will ever get is this file, so it is worth it being
// readable by somebody who is not us - say what happened, not which function it
// happened in.
//
// RELEASE BUILDS: define NDEBUG (build.ps1 does this for all targets). Every
// call becomes an inline no-op; the compiler eliminates the format strings from
// the binary entirely. Do not add logfile::Line calls gated by any other flag -
// NDEBUG is the one gate, and it must strip everything.
#pragma once

#ifndef NDEBUG

namespace logfile {

// Each split ASI owns its own file. Separate logs are part of the isolation:
// one component can fail before another starts without destroying its evidence.
void Open(const char* filename, const char* title);
void Line(const char* fmt, ...);
void Close();

}  // namespace logfile

#else

namespace logfile {
inline void Open(const char*, const char*) {}
inline void Line(const char*, ...) {}
inline void Close() {}
}  // namespace logfile

#endif
