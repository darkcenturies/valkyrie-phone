#pragma once
#include <string>
#include <vector>

namespace VehicleList
{
    struct Entry
    {
        int id;
        std::string name;
    };

    const std::vector<Entry> &Get();
}
