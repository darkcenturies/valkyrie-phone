#include <windows.h>
#include <fstream>
#include <algorithm>
#include <cctype>
#include "PedList.h"

namespace
{
    std::string Trim(const std::string &s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string GetGameDataPath()
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
        {
            path = path.substr(0, slash);
        }
        path += "\\data\\peds.ide";
        return path;
    }

    std::vector<PedList::Entry> LoadPedList()
    {
        std::vector<PedList::Entry> entries{{0,"CJ"}};

        std::ifstream file(GetGameDataPath());
        if (!file.is_open())
        {
            return entries;
        }

        bool inPedsSection = false;
        std::string line;
        while (std::getline(file, line))
        {
            std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            if (!inPedsSection)
            {
                if (trimmed == "peds") inPedsSection = true;
                continue;
            }

            if (trimmed == "end") break;

            size_t comma1 = trimmed.find(',');
            if (comma1 == std::string::npos) continue;
            size_t comma2 = trimmed.find(',', comma1 + 1);
            std::string idStr = trimmed.substr(0, comma1);
            std::string nameStr = (comma2 == std::string::npos)
                ? trimmed.substr(comma1 + 1)
                : trimmed.substr(comma1 + 1, comma2 - comma1 - 1);

            idStr = Trim(idStr);
            nameStr = Trim(nameStr);
            if (idStr.empty() || nameStr.empty()) continue;

            bool numeric = std::all_of(idStr.begin(), idStr.end(), [](unsigned char c) { return std::isdigit(c); });
            if (!numeric) continue;

            if(std::atoi(idStr.c_str())!=0)entries.push_back({std::atoi(idStr.c_str()), nameStr});
        }

        std::sort(entries.begin(), entries.end(), [](const PedList::Entry &a, const PedList::Entry &b) {
            return a.name < b.name;
        });

        return entries;
    }
}

namespace PedList
{
    const std::vector<Entry> &Get()
    {
        static std::vector<Entry> entries = LoadPedList();
        return entries;
    }
}
