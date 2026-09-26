#pragma once
#include <string>
#include <vector>

namespace PedList
{
    struct Entry
    {
        int id;
        std::string name;
    };

    const std::vector<Entry> &Get();
}
