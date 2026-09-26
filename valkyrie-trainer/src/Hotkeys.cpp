#include <windows.h>
#include <fstream>
#include <sstream>
#include "imgui.h"
#include "Hotkeys.h"
#include "Render.h"
#include "ActionBindings.h"

namespace
{
    Hotkeys::Slot g_slots[Hotkeys::kSlotCount];
    const int kSlotKeys[Hotkeys::kSlotCount] = {VK_F5, VK_F6, VK_F7, VK_F8};
    const int kStopKey = VK_F9;

    bool g_keyWasDown[Hotkeys::kSlotCount] = {};
    bool g_stopWasDown = false;

    bool KeyPressedEdge(int vk, bool &wasDown)
    {
        bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool pressed = isDown && !wasDown;
        wasDown = isDown;
        return pressed;
    }

    std::string GetConfigPath()
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos) path = path.substr(0, slash);
        return path + "\\valkyrie-hotkeys.cfg";
    }
}

namespace Hotkeys
{
    const Slot &GetSlot(int index)
    {
        return g_slots[index];
    }

    int GetSlotKey(int index)
    {
        return kSlotKeys[index];
    }

    int GetStopKey()
    {
        return kStopKey;
    }

    void AssignSlot(int index, std::string label, std::vector<AnimationPlayer::Step> steps, bool loop)
    {
        if (index < 0 || index >= kSlotCount || steps.empty() || steps.size()>256) return;
        g_slots[index].used = true;
        g_slots[index].label = std::move(label);
        g_slots[index].steps = std::move(steps);
        g_slots[index].loop = loop;
        Save();
    }

    void ClearSlot(int index)
    {
        if (index < 0 || index >= kSlotCount) return;
        g_slots[index] = Slot{};
        Save();
    }

    void Save()
    {
        std::ofstream f(GetConfigPath());
        if (!f) return;

        for (int i = 0; i < kSlotCount; i++)
        {
            const Slot &s = g_slots[i];
            f << "SLOT " << i << "\n";
            f << "USED " << (s.used ? 1 : 0) << "\n";
            f << "LOOP " << (s.loop ? 1 : 0) << "\n";
            f << "LABEL " << s.label << "\n";
            f << "STEPS " << s.steps.size() << "\n";
            for (const auto &step : s.steps)
            {
                f << "STEP " << step.library << " " << step.name << " " << step.flags << " "
                  << step.blendDelta << " " << step.timeMs << " " << (step.holdLastFrame ? 1 : 0) << "\n";
            }
        }
    }

    void Load()
    {
        std::ifstream f(GetConfigPath());
        if (!f) return;

        int slotIndex = -1;
        Slot pending;
        std::string line;
        while (std::getline(f, line))
        {
            std::istringstream iss(line);
            std::string tag;
            iss >> tag;

            if (tag == "SLOT")
            {
                if (slotIndex >= 0 && slotIndex < kSlotCount) g_slots[slotIndex] = pending;
                slotIndex=-1;iss >> slotIndex;
                pending = Slot{};
            }
            else if (tag == "USED")
            {
                int v = 0; iss >> v; pending.used = v != 0;
            }
            else if (tag == "LOOP")
            {
                int v = 0; iss >> v; pending.loop = v != 0;
            }
            else if (tag == "LABEL")
            {
                std::string rest;
                std::getline(iss, rest);
                if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
                pending.label = rest;
            }
            else if (tag == "STEP")
            {
                AnimationPlayer::Step step;
                if(!(iss >> step.library >> step.name >> step.flags >> step.blendDelta >> step.timeMs))continue;
                int hold = 0; iss >> hold;
                step.holdLastFrame = hold != 0;
                if(pending.steps.size()<257)pending.steps.push_back(step); // overflow stays invalid, never allocates unboundedly
            }
        }
        if (slotIndex >= 0 && slotIndex < kSlotCount) g_slots[slotIndex] = pending;
    }

    void Update()
    {
        // Don't steal keystrokes while the player is typing into a filter
        // box or the game itself doesn't have focus.
        bool imguiWantsKeyboard = ActionBindings::Capturing() || !ActionBindings::Focused() || ActionBindings::Paused() || (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput);

        for (int i = 0; i < kSlotCount; i++)
        {
            bool pressed = KeyPressedEdge(kSlotKeys[i], g_keyWasDown[i]);
            if (pressed && !imguiWantsKeyboard && g_slots[i].used)
            {
                if (g_slots[i].steps.size() == 1)
                {
                    AnimationPlayer::PlaySingle(g_slots[i].steps[0], g_slots[i].loop);
                }
                else
                {
                    AnimationPlayer::PlaySequence(g_slots[i].steps, g_slots[i].loop);
                }
            }
        }

        bool stopPressed = KeyPressedEdge(kStopKey, g_stopWasDown);
        if (stopPressed && !imguiWantsKeyboard)
        {
            AnimationPlayer::Stop();
        }

        AnimationPlayer::Update();
    }
}
