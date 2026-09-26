// The Camera app's live picture, on the phone's own screen.
//
// San Andreas can already draw the world a second time from somewhere other
// than the game camera: that is how its mirrors work, and how the stadium's
// big screens show the race from cameras round the track (CMirrors,
// CCamera::DealWithMirrorBeforeConstructRenderList). Each frame the game asks
// where the "mirror" camera is, lets the world into the render list if that
// camera can see it, and renders the scene from it into a texture before the
// main picture.
//
// The viewfinder borrows exactly that. While it is on, the mirror camera is
// the phone's camera, the texture is one of ours, and the step that would lay
// the texture over a mirror in the world is skipped - the phone's screen
// draws it instead. Nothing else about the game's frame changes.
//
// Game thread only.
#pragma once

#include <cstdint>

namespace viewfinder {

struct Vec {
    float x, y, z;
};

// Where the phone's camera is and what it looks at, for this frame. Call
// before Update.
void Aim(const Vec& eye, const Vec& target);

// On or off for this frame; call once a frame from the frame hook (after
// CGame::Process, before the world is rendered).
void Update(bool on);

// How the render is made from here on. The camera's lens: every frame, its
// near plane brought in to a hand's width. A reflection: only one frame in
// `every` (the picture from the last one stands between), the game's own
// near plane, and nothing drawn past `farPlane` metres.
void Lens();
void Reflection(int every, float farPlane);

// The picture: a texture as wide as the game's own view, squeezed into a
// 2:1 texture. Drawn at Aspect() width over height it is the right shape.
// 0 while off or before the first frame has been rendered into it.
uintptr_t Texture();

// The game camera's width over height, which the picture shares.
float Aspect();

// The game camera's view window: the tangents of half its view across and up.
bool ViewWindow(float out[2]);

// The viewfinder's own depth buffer, which a device reset needs given back
// first.
void ReleaseDeviceObjects();

}  // namespace viewfinder
