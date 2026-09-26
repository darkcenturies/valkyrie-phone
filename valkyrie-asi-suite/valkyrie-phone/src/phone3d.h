// The phone on the game's screen, drawn as the 3D model it is
// (tools/phone-model): each part lit and reflecting its own way, the phone's
// own screen - drawn into a texture of its own - lit on its face, and the
// whole of it ringed by the Valkyrie ink outline the trainer draws round its
// models. It knows nothing of the game: it is given the device, the textures,
// the light and where the phone is, so it can be tried outside the game
// (tools/phone-model/phone3d-test.cpp).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>

#include <string>

namespace phone3d {

// The model's parts, by the textures they are drawn with.
// The side buttons are not a part of the model file: they are split off the
// steel and plastic they are built of when it is read, to be coloured.
enum Part { kFront, kScreen, kBack, kChrome, kBlack, kLens, kButtons, kParts };
extern const char* const kPartTextures[kParts];

// Read the model. False when the file is not one this can draw.
bool Load(IDirect3DDevice9* device, const std::string& dff);
bool Loaded();

// The phone's screen: drawn into a texture of `width` x `height` pixels
// between these two calls, the device's own target put back after.
bool BeginScreen(IDirect3DDevice9* device, int width, int height);
void EndScreen(IDirect3DDevice9* device);
IDirect3DTexture9* ScreenTexture();

// What lies on the front glass - rain, cracks - drawn into a texture of the
// whole front (as body.png lays it out) between these two calls, and laid
// over the glass and the screen under it.
bool BeginGlass(IDirect3DDevice9* device, int width, int height);
void EndGlass(IDirect3DDevice9* device);

// How what lies on the glass bends the light through it, drawn the same way
// over the same area: each stroke's colour the way the glass leans there
// (red across, green down, 128 upright), premultiplied by its alpha. Drops
// are little lenses, the shards of a crack each tipped its own way. The
// picture under the glass is seen through it, and the light catches it.
bool BeginBend(IDirect3DDevice9* device, int width, int height);
void EndBend(IDirect3DDevice9* device);

struct Light {
    float dir[3];      // toward the sun: x right, y up, z out of the game's screen
    float sun[4];      // its colour, and strength
    float ambient[3];
    float env[3];      // the surroundings' turn and tilt, and how strongly they show
    bool liveEnv;      // env is the live mirror render rather than the studio picture
    // The live render's camera: the tangents of half its field of view across
    // and up (RwCamera's view window).
    float liveWindow[2] = {0.7f, 0.4f};
};

struct Pose {
    // Where the phone's screen is, in pixels, when the phone faces the viewer
    // at rest: left, top, width, height. The rest of the model follows from
    // it.
    float screen[4];
    float offset[2];         // moved this far from rest, in pixels
    float yaw, pitch, roll;  // turned from facing the viewer, in radians
    float brightness;        // the screen's own light, 0 to 1
    float lcd = 0.5f;        // how strongly the display's pixels show, 0 to 1
    // Where the screen is on the front, as fractions of it: left, top,
    // right, bottom; and whether the glass layer was drawn this frame.
    float screenOnFront[4] = {0, 0, 1, 1};
    bool glass = false;
    bool bend = false;  // the bend layer was drawn this frame
    // Fine scratches on the front glass from use, seen only where light
    // catches them: how many (0 none, 1 as shipped, up to 3), and the
    // pattern's seed.
    float wear = 0;
    uint32_t wearSeed = 0x5C7A7C4Eu;
    bool wearStrong = true;  // plain to see in the sun; false: only just there
    uint32_t ink = 0xFF000000;  // the outline's colour, 0xAARRGGBB
};

// Draw it onto the device's current target. `parts` are the six parts'
// textures, `env` the surroundings reflected; `inkScale` sizes the outline
// (1 at 1080 lines) and `inkTime` breathes it. False when it could not, so the
// caller can draw the phone some other way.
bool Draw(IDirect3DDevice9* device, const Pose& pose, const Light& light, IDirect3DTexture9* const parts[kParts],
          IDirect3DTexture9* env, float inkScale, float inkTime);

// Everything Draw would otherwise make the first time - the shaders compiled,
// the glass's wear - made now, as the game loads, so taking the phone out
// never waits on it.
bool Prepare(IDirect3DDevice9* device, const Pose& pose);

// Where the phone is on the target, in pixels, the outline included, as last
// drawn: left, top, right, bottom.
void Bounds(float out[4]);

// Where the screen's top left, top right and bottom left corners landed, in
// pixels (x, y each), as last drawn: for turning the cursor into a point on
// the screen however the phone is turned. False before the first draw.
bool ScreenCorners(float out[6]);

// Everything held in the device's own memory, which a device reset needs
// given back first.
void ReleaseDeviceObjects();

// The picture the phone is drawn into before its ink, for tools/phone-model's test.
IDirect3DTexture9* DebugTarget();

}  // namespace phone3d
