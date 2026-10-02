#pragma once
#include <cctype>
#include <istream>
#include <sstream>
#include <string>

namespace phone_weapon {
enum class Selection { None, Open, Close, Select, Restore };
constexpr Selection NextSelection(bool selected, bool wasSelected, bool open,
                                   bool canSelect, bool finishingHand) {
    if (!canSelect) return Selection::None;
    if (selected && !wasSelected && !open) return Selection::Open;
    if (!selected && wasSelected && open) return Selection::Close;
    if (open && !selected && !wasSelected) return Selection::Select;
    if (!open && selected && !finishingHand) return Selection::Restore;
    return Selection::None;
}

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
