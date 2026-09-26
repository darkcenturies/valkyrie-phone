// Picture files as textures: icons the ini names, and the photos the camera
// saved. Decoded with Windows' own image decoder (PNG, JPEG, BMP, GIF) and
// scaled into a square texture a power of two across, the size the game's
// textures come in; `width` and `height` keep the picture's own shape.
#pragma once

#include <cstdint>
#include <string>

namespace picture {

struct Picture {
    uintptr_t texture = 0;
    int width = 0, height = 0;
};

// Game thread only. `side` is the texture's size; a power of two.
Picture Load(const std::string& path, int side);
void Free(Picture& p);

// Cut a JPEG down to `aspect` (width over height) about its middle, keeping
// its full height, and save it over itself - on a thread of its own, so the
// game does not wait for the encoder. Done() is true once the last one asked
// for has been written (or given up on).
void CropToAspect(const std::string& path, float aspect);
bool CropDone();

}  // namespace picture
