#include <plugin.h>
#include <CMessages.h>
#include "Render.h"
#include "FaultGuard.h"
#include "Menu.h"
#include "Hotkeys.h"
#include "ModelPreview.h"
#include "layout_check.h"
#include "instrument.h"
#include "log.h"
#include <d3d9.h>
#include "WeaponIcons.h"
#include "ActionBindings.h"
#include "AutoWalk.h"
#include <cstring>
#include <string>

using namespace plugin;

struct Main
{
    Main()
    {
#ifdef VALKYRIE_TRAINER_IN_PHONE
        // Carried in the phone's ASI: its log is the phone's, and a separate
        // valkyrie-trainer.asi beside the game, which would load too, is left
        // to be the trainer - two would hook everything twice.
        logfile::Open("valkyrie-phone.log", "Valkyrie phone");
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (char* slash = strrchr(path, '\\')) slash[1] = 0;
            if (GetFileAttributesA((std::string(path) + "valkyrie-trainer.asi").c_str()) != INVALID_FILE_ATTRIBUTES) {
                logfile::Line("trainer: valkyrie-trainer.asi is installed too - it is the trainer, the phone's copy stands down");
                return;
            }
            logfile::Line("trainer: the phone's own trainer starting");
        }
#else
        logfile::Open("valkyrie-trainer.log", "Valkyrie Trainer / live model preview");
#endif
        if (!valkyrie_layout::Compatible10()) { logfile::Line("Unsupported game layout; trainer disabled"); return; }
        instrument::Note("preview", "Live streamed DFF/TXD / isolated viewer");
        Events::gameProcessEvent += [this]{ OnGameProcess(); };
        Render::Init();
        Hotkeys::Load();
    }

    void OnGameProcess()
    {
        if(Render::Faulted())return;
        static bool shown = false;
        if (!shown)
        {
            CMessages::AddMessageJumpQ("Valkyrie Trainer loaded - press ALT+Z (F5-F8 play, F9 stop)", 1000, 0);
            shown = true;
        }

        // Hotkeys and persistent toggles (Infinite Health, Freeze Player,
        // etc.) fire every frame independent of the menu being open, so
        // they get the same crash containment as menu button clicks - a
        // bad assigned animation or a null ped shouldn't take the game down.
        __try
        {
            FaultGuard::stage="action initialization";Menu::InitActions();
            FaultGuard::stage="action binding";ActionBindings::Update();
            FaultGuard::stage="autowalk";AutoWalk::Update();
            if(Render::IsMenuOpen())WeaponIcons::Process();
            FaultGuard::stage="model preview";ModelPreview::Process();
            FaultGuard::stage="animation update";Hotkeys::Update();
            FaultGuard::stage="travel";Menu::ProcessTravel();
            FaultGuard::stage="persistent effects and skin change";Menu::ApplyPersistentEffects();
        }
        __except (FaultGuard::Report(GetExceptionInformation()))
        {
            Render::DisableAfterFault();
            CMessages::AddMessageJumpQ(
                "Valkyrie Trainer: disabled after a background error. Please restart the game.", 4000, 0);
        }
    }
} gInstance;
