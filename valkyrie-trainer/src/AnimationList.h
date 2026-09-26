#pragma once
#include <string>
#include <vector>

namespace AnimationList
{
    struct Entry
    {
        std::string library;
        std::string name;
    };

    const std::vector<Entry> &Get();
}
