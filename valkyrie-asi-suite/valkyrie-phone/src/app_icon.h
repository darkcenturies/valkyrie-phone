// Built-in app artwork used when a configured icon cannot be loaded.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace phone_icons {
inline std::string Builtin(const std::string& app) {
    std::string name = app == "Camera" ? "Photos" : app;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return "app_" + name;
}
}
