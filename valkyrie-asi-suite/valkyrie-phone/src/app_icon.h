// Built-in app artwork used when a configured icon cannot be loaded.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace phone_icons {
inline std::string Builtin(const std::string& app) {
    std::string name = app;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return "app_" + name;
}
// Replace the defaults shipped before separate Camera/Photos artwork existed.
// Other configured artwork remains a user override.
inline std::string UpgradeDefault(const std::string& app, const std::string& icon) {
    if ((app == "Camera" && icon == "app_photos") ||
        (app == "Photos" && icon == "hud:radar_1hourphoto")) return Builtin(app);
    return icon;
}
}
