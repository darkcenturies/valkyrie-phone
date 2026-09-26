#pragma once

// A live, orbitable mesh viewer using the installed game's streamed DFF/TXD.
// No world entity, animation task, camera hook, or pre-rendered image files.
namespace ModelPreview
{
    void BeginFrame();
    void Draw(const char* kind, int modelId, float width, float height);
    void Refresh(const char* kind, int modelId);
    void EndFrame();
    void Process(); // game process only: request streaming and hold model refs
    void BeforeSkinChange(); // game callback only: drop cloned geometry before rebuild
    void InvalidateDeviceObjects();
}
