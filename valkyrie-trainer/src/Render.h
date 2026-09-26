#pragma once

struct ImFont;

namespace Render
{
    void Init();
    bool IsMenuOpen();
    void CloseMenu();
    // Open the menu as Alt+Z does, for another ASI (the phone's trainer
    // number). Game thread only. False when the trainer cannot open.
    bool OpenMenuFromOutside();
    void DisableAfterFault();
    bool Faulted();
    ImFont *GetTitleFont();
}
