#include "weapon_type.h"
#include <cassert>
#include <cstdio>
int Parse(const char* text, int limit = 80) {
    std::istringstream file(text);
    return phone_weapon::FindType(file, limit);
}
int main() {
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
