#pragma once
#include <string>
#include <vector>
#include "AnimationPlayer.h"

namespace Hotkeys
{
    constexpr int kSlotCount = 4;

    struct Slot
    {
        bool used = false;
        std::string label;
        std::vector<AnimationPlayer::Step> steps;
        bool loop = true;
    };

    const Slot &GetSlot(int index);
    int GetSlotKey(int index);   // virtual-key code, e.g. VK_F5
    int GetStopKey();            // virtual-key code, e.g. VK_F9

    void AssignSlot(int index, std::string label, std::vector<AnimationPlayer::Step> steps, bool loop);
    void ClearSlot(int index);

    // Slots persist to valkyrie-hotkeys.cfg next to the .asi so assignments
    // survive a game restart.
    void Load();
    void Save();

    // Polls hotkeys and drives AnimationPlayer. Must be called once per
    // game frame, only while the game window has focus.
    void Update();
}
