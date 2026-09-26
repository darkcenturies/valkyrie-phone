#include "log.h"

#ifndef NDEBUG

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace logfile {
namespace {

FILE* g_file = nullptr;
std::mutex g_lock;

}  // namespace

void Open(const char* filename, const char* title) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_file) {
        return;
    }

    // Beside the executable, so it turns up in the game folder where anybody
    // would think to look, rather than in whatever the working directory
    // happens to be.
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) {
        slash[1] = '\0';
    }
    strncat_s(path, filename ? filename : "sprp-client.log", _TRUNCATE);

    // The last session's log kept beside it (name.previous.log): whatever went
    // wrong is usually why the game was started again, and a fresh log would
    // otherwise write over the only record of it.
    char previous[MAX_PATH]{};
    strncpy_s(previous, path, _TRUNCATE);
    if (char* dot = strrchr(previous, '.')) *dot = '\0';
    strncat_s(previous, ".previous.log", _TRUNCATE);
    MoveFileExA(path, previous, MOVEFILE_REPLACE_EXISTING);

    // _fsopen rather than fopen_s, because fopen_s opens the file exclusively
    // and nobody - not the player, not us - can read the log while the game is
    // still running. The whole value of this file is being able to look at it
    // while something is going wrong.
    g_file = _fsopen(path, "w", _SH_DENYWR);
    if (!g_file) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fprintf(g_file, "%s - started %04d-%02d-%02d %02d:%02d:%02d\n\n",
            title ? title : "SP-RP client component",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    fflush(g_file);
}

void Line(const char* fmt, ...) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_file) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fprintf(g_file, "[%02d:%02d:%02d] ", now.wHour, now.wMinute, now.wSecond);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);

    fputc('\n', g_file);
    fflush(g_file);
}

void Close() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace logfile

#endif  // NDEBUG
