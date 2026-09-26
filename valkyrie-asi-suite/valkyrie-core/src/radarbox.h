// Where the HUD radar sits and how big it is - one definition, shared.
//
// Three separate things have to agree about this box:
//
//   * valkyrie-radar.asi replaces GTA's CRadar::TransformRadarPointToScreenSpace
//     with these numbers, so the blips, route line, gang overlay and north
//     marker the game draws all land inside it;
//   * valkyrie-radar.asi also scissors the radar draw to it, which is what stops
//     the gang overlay painting across the screen now that the round mask no
//     longer clips it;
//   * valkyrie-radar.asi draws its rounded rectangle of world into it.
//
// When these disagree the failures are the ones already hit once each: the
// overlay escaping the radar, or the image ending up clipped to a different
// shape than the markers drawn on top of it. They are separate modules, so
// this header is the only thing keeping them in step - change it, rebuild
// both.
//
// Units are GTA's HUD space: x measured against 640, y against 448. Note
// that these use *different* scales on screen - x by width/640 and y by
// height/448 - which is how GTA itself does it. Using one scale for both is
// wrong and shows up as a box that drifts sideways at other resolutions.
//
// The four numbers that place the box are settings rather than constants: a
// player can move or resize the panel in valkyrie-radar.ini. They are read
// through radarcfg, which every ASI in the suite links, so all three of the
// things listed above go on agreeing about the box wherever it is put. Changing
// them at compile time is no longer how this is done - see radarcfg.h - but the
// values that used to be here are still the defaults there.
#pragma once
#include "radarcfg.h"

namespace radarbox {

// Proportions taken from the GTA VI minimap: roughly 2:1, tucked close into
// the bottom-left corner, rather than San Andreas' near-square 94x76 sitting
// well in from the edge.
inline float Width()  { return radarcfg::Current().panelWidth; }
inline float Height() { return radarcfg::Current().panelHeight; }
inline float Left()   { return radarcfg::Current().panelLeft; }

// Gap between the bottom of the radar and the bottom of the screen.
inline float BottomMargin() { return radarcfg::Current().panelBottomMargin; }

// What GTA's own transform calls the "from bottom" distance: it measures to
// the *top* of the radar, so it is the margin plus the height.
inline float BottomOffset() { return BottomMargin() + Height(); }

// World coverage shared by the 3D camera and every native marker projected
// over it. Keeping this only in radar3d.cpp made the terrain show 18% of the
// radar range while blips still used 100%, so their positions could not match.
constexpr float kViewScale = 0.10f;

// How far below panel-centre the player sits, as a fraction of the panel's
// half-height - shared by two independent systems that must move together:
// Valkyrie Radar's 3D camera (radar3d.cpp derives its view-space
// kPlayerDrop from this) and its native marker/blip/route
// transform (TransformRadarPointToScreenHelper in game.cpp adds this
// directly as a screen-space offset). Change it here, not in either ASI, or
// the road will show through at one position while the arrow sits at
// another.
// 0.30 places the arrow 80% of the way down the panel. The projection puts it
// at NDC -2f, so the fraction from the top edge is simply 0.5 + f; measured off
// the reference minimap the arrow sits at about 0.8, where 0.24 was putting
// ours at 0.74. More road ahead, less behind.
constexpr float kPlayerDropFraction = 0.30f;

// GTA expands the driving radar from 180 to 350 world units as speed rises.
// That near-doubling is far too dramatic for the tight navigation camera.
// Retain only a subtle 15-unit (8.3%) speed zoom while preserving any smaller
// scripted range unchanged.
constexpr float kMaximumDrivingRange = 195.0f;
constexpr float EffectiveDrivingRange(float rawRange) {
  return rawRange > kMaximumDrivingRange ? kMaximumDrivingRange : rawRange;
}

}  // namespace radarbox
