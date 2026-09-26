#pragma once

namespace Menu
{
    void InitActions();
    void ProcessTravel();
    void Draw();

    // Applies toggle-based persistent effects (Infinite Health/Armour,
    // God Mode, Never Wanted, Freeze Player, Infinite Ammo). Must be
    // called every frame regardless of whether the menu is open or which
    // tab is active - the checkboxes only live in DrawPlayerTab/
    // DrawWeaponsTab, so without this the effect would silently stop the
    // moment you switched tabs or closed the menu.
    void ApplyPersistentEffects();
}
