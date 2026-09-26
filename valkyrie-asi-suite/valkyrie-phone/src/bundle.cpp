#include "bundle.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"

namespace bundle {
namespace {

// The blob, little-endian: "VPAK", u32 version (1), u32 file count, then
// each file as u16 name length, the name (bytes, '\' between folders),
// u32 size, the bytes.
constexpr int kResourceId = 1;
std::string g_dir;

HMODULE Self() {
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Self), &module);
    return module;
}

// FNV-1a over the blob, naming the folder: a different build unpacks afresh.
uint32_t Hash(const uint8_t* data, size_t size) {
    uint32_t h = 2166136261u;
    // Every 64th byte and the length: quick, and different for any rebuild.
    for (size_t i = 0; i < size; i += 64) h = (h ^ data[i]) * 16777619u;
    return (h ^ static_cast<uint32_t>(size)) * 16777619u;
}

bool MakeFolders(const std::string& path) {
    for (size_t at = path.find('\\', 3); at != std::string::npos; at = path.find('\\', at + 1)) {
        CreateDirectoryA(path.substr(0, at).c_str(), nullptr);
    }
    return true;
}

bool SameFile(const std::string& path, uint32_t size) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    return GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeHigh == 0 &&
           info.nFileSizeLow == size;
}

}  // namespace

bool Unpack() {
    const HMODULE module = Self();
    HRSRC found = module ? FindResourceA(module, MAKEINTRESOURCEA(kResourceId), MAKEINTRESOURCEA(10)) : nullptr;  // RT_RCDATA
    HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
    const auto* data = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
    const DWORD size = found ? SizeofResource(module, found) : 0;
    if (!data || size < 12 || memcmp(data, "VPAK", 4) != 0) {
        logfile::Line("bundle: this build carries nothing - reading the game folder");
        return false;
    }
    uint32_t version = 0, count = 0;
    memcpy(&version, data + 4, 4);
    memcpy(&count, data + 8, 4);
    if (version != 1) {
        logfile::Line("bundle: version %u is not one this reads", version);
        return false;
    }
    char temp[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, temp)) return false;
    char folder[64];
    snprintf(folder, sizeof folder, "valkyrie-phone-%08x\\", Hash(data, size));
    const std::string dir = std::string(temp) + folder;
    size_t at = 12;
    int written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 2 > size) return false;
        uint16_t nameLength = 0;
        memcpy(&nameLength, data + at, 2);
        at += 2;
        if (at + nameLength + 4 > size) return false;
        std::string name(reinterpret_cast<const char*>(data + at), nameLength);
        at += nameLength;
        uint32_t fileSize = 0;
        memcpy(&fileSize, data + at, 4);
        at += 4;
        if (at + fileSize > size || name.find("..") != std::string::npos) return false;
        const std::string path = dir + name;
        if (!SameFile(path, fileSize)) {
            MakeFolders(path);
            const std::string part = path + ".part";
            HANDLE file = CreateFileA(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD done = 0;
            const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, data + at, fileSize, &done, nullptr) &&
                            done == fileSize;
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (!ok || !MoveFileExA(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileA(part.c_str());
                logfile::Line("bundle: could not write %s", path.c_str());
                return false;
            }
            ++written;
        }
        at += fileSize;
    }
    g_dir = dir;
    logfile::Line("bundle: %u files in %s (%d written now)", count, dir.c_str(), written);
    return true;
}

const std::string& Dir() { return g_dir; }

std::string Path(const std::string& name) {
    if (g_dir.empty()) return {};
    const std::string path = g_dir + name;
    return GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : path;
}

}  // namespace bundle
