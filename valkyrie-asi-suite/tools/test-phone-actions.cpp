#include "phone_actions.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace phone_actions;
int main() {
    assert(Next(Pose::None, Pose::Use, false, true, false) == Pose::TakeOut);
    assert(Next(Pose::TakeOut, Pose::Use, false, true, false) == Pose::TakeOut);
    assert(Next(Pose::TakeOut, Pose::Use, true, true, false) == Pose::Use);
    assert(Next(Pose::Use, Pose::Type, false, true, false) == Pose::Type);
    assert(Next(Pose::Type, Pose::Use, false, true, false) == Pose::Use);
    assert(Next(Pose::Use, Pose::Camera, false, true, false) == Pose::Camera);
    assert(Next(Pose::Camera, Pose::Camera, true, true, true) == Pose::Photo);
    assert(Next(Pose::Photo, Pose::Camera, false, true, false) == Pose::Photo);
    assert(Next(Pose::Photo, Pose::Camera, true, true, false) == Pose::Camera);
    assert(Next(Pose::Camera, Pose::Use, true, true, false) == Pose::CameraOut);
    assert(Next(Pose::CameraOut, Pose::Use, false, true, false) == Pose::CameraOut);
    assert(Next(Pose::CameraOut, Pose::Use, true, true, false) == Pose::Use);
    assert(Next(Pose::Selfie, Pose::None, false, true, false) == Pose::CameraOut);
    assert(Next(Pose::CameraOut, Pose::None, true, true, false) == Pose::PutAway);
    assert(Next(Pose::Use, Pose::None, false, true, false) == Pose::PutAway);
    assert(Next(Pose::PutAway, Pose::None, false, true, false) == Pose::PutAway);
    assert(Next(Pose::PutAway, Pose::None, true, true, false) == Pose::None);
    assert(Next(Pose::PutAway, Pose::Use, false, true, false) == Pose::TakeOut);
    assert(Next(Pose::CameraOut, Pose::Selfie, false, true, false) == Pose::Selfie);
    assert(Next(Pose::Use, Pose::Call, false, true, false) == Pose::CallIn);
    assert(Next(Pose::CallIn, Pose::Call, false, true, false) == Pose::CallIn);
    assert(Next(Pose::CallIn, Pose::Call, true, true, false) == Pose::Call);
    assert(Next(Pose::Call, Pose::Hold, false, true, false) == Pose::CallOut);
    assert(Next(Pose::CallOut, Pose::Hold, true, true, false) == Pose::Hold);
    assert(Next(Pose::CallOut, Pose::None, true, true, false) == Pose::PutAway);
    for (auto p : {Pose::TakeOut, Pose::Use, Pose::Type, Pose::Camera, Pose::Selfie, Pose::Photo,
                   Pose::CameraOut, Pose::PutAway, Pose::Hold, Pose::CallIn, Pose::Call, Pose::CallOut}) {
        assert(Next(p, Pose::Use, false, false, true) == Pose::None);
    }
    assert(LegacyDefault("BETSLP_LOOP", "OTB", true, "betslp_loop", "otb", true));
    assert(LegacyDefault("ARRESTgun", "ped", false, "ARRESTgun", "ped", false));
    assert(!LegacyDefault("my_phone_idle", "custom", true, "betslp_loop", "otb", true));
    assert(!LegacyDefault("betslp_loop", "otb", false, "betslp_loop", "otb", true));
    assert(!LegacyDefault("ARRESTgun", "custom", false, "ARRESTgun", "ped", false));
    assert(ResumeHeldCamera(Pose::Photo, Pose::Camera));
    assert(ResumeHeldCamera(Pose::Photo, Pose::Selfie));
    assert(!ResumeHeldCamera(Pose::Use, Pose::Camera));
    assert(!ResumeHeldCamera(Pose::Photo, Pose::Use));
    std::puts("PASS: entry, typing, camera/shutter, exit, rapid reopening and protected interruptions");
}
