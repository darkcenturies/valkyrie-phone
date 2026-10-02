#pragma once
#include <cctype>
#include <istream>
#include <sstream>
#include <string>

namespace phone_weapon {
// Read only the explicitly configured extra type; never probe outside the
// stock or limit-adjuster's weapon-info array looking for a matching model.
inline int FindType(std::istream& file, int limit) {
    int found = -1;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        int id = -1;
        std::string name;
        if (!(row >> id >> name)) continue;
        for (char& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (name != "VALKYRIEPHONE") continue;
        if (id < 70 || id >= limit || found >= 0) return -1;
        found = id;
    }
    return found;
}
}
