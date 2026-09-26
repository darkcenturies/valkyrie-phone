#pragma once

// Loads and caches the pre-baked rotation-frame thumbnails (see
// tools/model-conversion/preview-bake in valkyrie-workshop) for display on
// hover in the Skin/Vehicle tabs. Deliberately NOT live 3D rendering - see
// that pipeline's own notes for why.
namespace PreviewImages
{
    // kind is "ped" or "vehicle", frameIndex is 0-11. Returns an
    // IDirect3DTexture9* (void* to keep d3d9.h out of this header) usable
    // directly as an ImGui::Image texture ID, or nullptr if that frame
    // hasn't been baked / couldn't be loaded. Loads and caches lazily;
    // cheap to call every frame for whatever's currently hovered.
    void *GetFrame(const char *kind, int modelId, int frameIndex);

    // Frees cached textures once the cache grows large. Call when the menu
    // closes or a tab changes away from Skin/Vehicle.
    void Trim();
}
