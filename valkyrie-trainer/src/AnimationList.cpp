#include <windows.h>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include "AnimationList.h"
#include "AnimationCatalog.h"

namespace
{
    std::string GetExeDir()
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
        {
            path = path.substr(0, slash);
        }
        return path;
    }

    std::string CStr(const char *data, size_t maxLen)
    {
        size_t len = 0;
        while (len < maxLen && data[len] != '\0') len++;
        return std::string(data, len);
    }

    std::string LowerExt(const std::string &name)
    {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lower;
    }

    void ParseAnp3(const std::vector<uint8_t>& buf,const std::string& library,std::vector<AnimationList::Entry>& out){AnimationCatalog::Parse(buf,library,out);}

    struct ImgEntry
    {
        uint32_t offsetSectors;
        uint16_t streamSectors;
        std::string name;
    };

    std::vector<ImgEntry> ReadImgDirectory(std::ifstream &f)
    {
        std::vector<ImgEntry> entries;
        char magic[4];
        f.read(magic, 4);
        if (!f || std::memcmp(magic, "VER2", 4) != 0) return entries;

        uint32_t count = 0;
        f.read(reinterpret_cast<char *>(&count), 4);
        if (!f || count>100000) return entries;

        entries.reserve(count);
        for (uint32_t i = 0; i < count && f; i++)
        {
            ImgEntry e{};
            char name[24];
            f.read(reinterpret_cast<char *>(&e.offsetSectors), 4);
            f.read(reinterpret_cast<char *>(&e.streamSectors), 2);
            f.seekg(2, std::ios::cur); // size field, unused for VER2
            f.read(name, 24);
            if (!f) break;
            e.name = CStr(name, 24);
            entries.push_back(e);
        }
        return entries;
    }

    std::vector<AnimationList::Entry> LoadAnimationList()
    {
        std::vector<AnimationList::Entry> out;
        const std::string exeDir = GetExeDir();

        // Loose block: SA reads player/ped animation from this file directly.
        {
            std::ifstream f(exeDir + "\\anim\\ped.ifp", std::ios::binary | std::ios::ate);
            if (f)
            {
                size_t size = static_cast<size_t>(f.tellg());
                if(size>64*1024*1024)return out;
                f.seekg(0);
                std::vector<uint8_t> buf(size);
                f.read(reinterpret_cast<char *>(buf.data()), size);
                ParseAnp3(buf, "ped", out);
            }
        }

        // Everything else lives packed in anim.img (VER2 archive format).
        {
            std::ifstream f(exeDir + "\\anim\\anim.img", std::ios::binary);
            if (f)
            {
                auto entries = ReadImgDirectory(f);
                for (const auto &e : entries)
                {
                    std::string lower = LowerExt(e.name);
                    if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".ifp") continue;

                    std::string library = e.name.substr(0, e.name.size() - 4);

                    uint64_t byteOffset = static_cast<uint64_t>(e.offsetSectors) * 2048;
                    size_t byteSize = static_cast<size_t>(e.streamSectors) * 2048;
                    if (byteSize == 0 || byteSize>64*1024*1024) continue;

                    f.clear();f.seekg(static_cast<std::streamoff>(byteOffset));
                    std::vector<uint8_t> buf(byteSize);
                    f.read(reinterpret_cast<char *>(buf.data()), byteSize);
                    if(f)ParseAnp3(buf, library, out);
                }
            }
        }

        std::sort(out.begin(), out.end(), [](const AnimationList::Entry &a, const AnimationList::Entry &b) {
            if (a.library != b.library) return a.library < b.library;
            return a.name < b.name;
        });

        out.erase(std::unique(out.begin(),out.end(),[](const auto& a,const auto& b){return a.library==b.library&&a.name==b.name;}),out.end());
        return out;
    }
}

namespace AnimationList
{
    const std::vector<Entry> &Get()
    {
        static std::vector<Entry> entries = LoadAnimationList();
        return entries;
    }
}
