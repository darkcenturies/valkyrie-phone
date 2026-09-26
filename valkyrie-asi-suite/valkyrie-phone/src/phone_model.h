// The phone CJ holds: a model of the phone's own, separate from the game's.
//
// The game has one cellphone, model 330, which story missions put in CJ's hand
// for their calls. This phone brings its own model (valkyrie-phone-model.dff
// and .txd next to the game) and shows it only while this phone is in use: for
// that time model 330 is pointed at it, and afterwards at the game's own
// again, so every call the game makes still has the game's phone.
//
// Without the files the game's phone is used throughout.
#pragma once

#include <cstdint>
#include <string>

namespace phone_model {

// Load the model. Call once the game is running. Returns false, and leaves the
// game's phone in place, when the files are missing or will not load.
bool Load(const std::string& dff, const std::string& txd);
bool Loaded();

// While true, model 330 is this phone's; while false, the game's. Call before
// the cellphone model is put in CJ's hand, and again once it is taken away.
void Use(bool ours);

// Give the model the game's shine: every material reflects `envTexture`
// (the game's own car reflection, where it can be found) as a car's paint
// does, turning with the camera, `coefficient` strong (1 the most). Call
// after Load, once the game's camera exists.
void MakeShiny(uintptr_t envTexture, float coefficient);

// The phone in a ped's left hand. The game only ever draws a held model in
// the right hand (a twin pistol's second copy aside), so this phone draws
// its own: a copy of model 330 - this phone's, while Use(true) - placed on
// the left hand's bone each frame, turned and moved by `turn` (degrees about
// x, y and z) and `offset` (metres, in the hand's own frame), `flip` turning
// it half round about its own long axis so its screen faces the other way,
// and lit as the game lights the ped.
//
// It is drawn in the game's own weapon pass, right after the world, so the
// world's depth stands and CJ's body hides what is behind it: HookWeaponPass
// once, then SetHand every frame with what to draw (off while the phone is
// not in his hand). Where the pass cannot be found, RenderInLeftHand, from a
// 2D draw hook, draws it over the finished frame instead.
bool HookWeaponPass();
void SetHand(bool on, const float turn[3], const float offset[3], bool flip);
bool RenderInLeftHand(uintptr_t ped, const float turn[3], const float offset[3], bool flip);
// Where the phone in the left hand is, as of the last frame the game
// animated the ped: its middle and the way it faces.
bool LeftHandPlace(uintptr_t ped, const float turn[3], const float offset[3], bool flip, float pos[3], float facing[3]);
// Let go of the copy (when the phone is put away).
void ReleaseHand();

}  // namespace phone_model
