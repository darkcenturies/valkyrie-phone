#pragma once

// Decisions only: the game adapter owns animation playback and interruption checks.
namespace phone_actions {
enum class Pose { None, TakeOut, Use, Type, Camera, Selfie, Photo, CameraOut, PutAway };
constexpr bool CameraPose(Pose p) { return p == Pose::Camera || p == Pose::Selfie || p == Pose::Photo; }
constexpr bool OneShot(Pose p) {
    return p == Pose::TakeOut || p == Pose::Photo || p == Pose::CameraOut || p == Pose::PutAway;
}
constexpr bool Leaving(Pose p) { return p == Pose::CameraOut || p == Pose::PutAway; }
constexpr Pose Next(Pose current, Pose desired, bool complete, bool allowed, bool shutter) {
    if (!allowed) return Pose::None; // Calls, vehicles, falls and mission tasks always win.
    if (shutter && CameraPose(desired)) return Pose::Photo;
    if (current == Pose::Photo && !complete && CameraPose(desired)) return Pose::Photo;
    if (CameraPose(current) && !CameraPose(desired)) return Pose::CameraOut;
    if (current == Pose::CameraOut) {
        if (CameraPose(desired)) return desired;
        return complete ? (desired == Pose::None ? Pose::None : Pose::TakeOut) : current;
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
