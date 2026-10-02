#include "weapon_type.h"
#include <cassert>
#include <cstdio>
int Parse(const char* text, int limit = 80) {
    std::istringstream file(text);
    return phone_weapon::FindType(file, limit);
}
int main() {
    using phone_weapon::NextSelection; using phone_weapon::Selection;
    assert(NextSelection(true,false,false,true,false)==Selection::Open);
    assert(NextSelection(true,true,true,true,false)==Selection::None); // tucked stays selected
    assert(NextSelection(true,true,true,true,true)==Selection::None); // raised stays selected
    assert(NextSelection(false,true,true,true,true)==Selection::Close); // scroll away
    assert(NextSelection(false,false,true,true,false)==Selection::Select); // hotkey
    assert(NextSelection(true,true,false,true,true)==Selection::None); // finish exit
    assert(NextSelection(true,true,false,true,false)==Selection::Restore);
    assert(NextSelection(false,true,true,false,false)==Selection::None); // vehicle

    assert(Parse("#70 VALKYRIEPHONE\n;71 VALKYRIEPHONE\n") == -1);
    assert(Parse("70 VALKYRIEPHONE -1 1 0 1 97 194 247 1.0 FLOWERS\n") == 70);
    assert(Parse("75 valkyriephone -1\r\n") == 75);
    assert(Parse("80 VALKYRIEPHONE\n") == -1);
    assert(Parse("69 VALKYRIEPHONE\n") == -1);
    assert(Parse("512 VALKYRIEPHONE\n") == -1);
    assert(Parse("70 VALKYRIEPHONE\n71 VALKYRIEPHONE\n") == -1);
    assert(Parse("70 VALKYRIEPHONE\n", 70) == -1);
    assert(Parse("invalid VALKYRIEPHONE\n70 FLOWERS\n") == -1);
    puts("PASS: phone weapon discovery rejects comments, ambiguous types and out-of-range IDs");
}
