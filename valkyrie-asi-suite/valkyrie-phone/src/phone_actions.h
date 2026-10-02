#pragma once
#include <string_view>

// Decisions only: the game adapter owns animation playback and interruption checks.
namespace phone_actions {
constexpr bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
constexpr bool LegacyDefault(std::string_view name, std::string_view file, bool loop,
                             std::string_view oldName, std::string_view oldFile, bool oldLoop) {
    return SameName(name, oldName) && SameName(file, oldFile) && loop == oldLoop;
}
enum class Pose { None, TakeOut, Use, Type, Camera, Selfie, Photo, CameraOut, PutAway };
constexpr bool NeedsHand(bool focused, bool camera, bool playing, Pose pose) {
    return focused || camera || (playing && pose != Pose::None);
}
constexpr bool CameraPose(Pose p) { return p == Pose::Camera || p == Pose::Selfie || p == Pose::Photo; }
constexpr bool OneShot(Pose p) {
    return p == Pose::TakeOut || p == Pose::Photo || p == Pose::CameraOut || p == Pose::PutAway;
}
constexpr bool ResumeHeldCamera(Pose from, Pose to) {
    return from == Pose::Photo && (to == Pose::Camera || to == Pose::Selfie);
}
constexpr bool Leaving(Pose p) { return p == Pose::CameraOut || p == Pose::PutAway; }
constexpr Pose Next(Pose current, Pose desired, bool complete, bool allowed, bool shutter) {
    if (!allowed) return Pose::None; // Calls, vehicles, falls and mission tasks always win.
    if ((current == Pose::None || current == Pose::PutAway) && desired != Pose::None) return Pose::TakeOut;
    if (current == Pose::TakeOut && !complete && desired != Pose::None) return Pose::TakeOut;
    if (shutter && CameraPose(desired)) return Pose::Photo;
    if (current == Pose::Photo && !complete && CameraPose(desired)) return Pose::Photo;
    if (CameraPose(current) && !CameraPose(desired)) return Pose::CameraOut;
    if (current == Pose::CameraOut) {
        if (CameraPose(desired)) return desired;
        return complete ? desired : current;
    }
    if (desired == Pose::None) {
        if (current == Pose::None || (current == Pose::PutAway && complete)) return Pose::None;
        return Pose::PutAway;
    }
    if (CameraPose(desired)) return desired;
    if (current == Pose::None || current == Pose::PutAway) return Pose::TakeOut;
    if (current == Pose::TakeOut && !complete) return current;
    return desired;
}
} // namespace phone_actions
