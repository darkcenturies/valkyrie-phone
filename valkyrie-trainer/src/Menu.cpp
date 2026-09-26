#include <plugin.h>
#include <common.h>
#include <CCheat.h>
#include <CMessages.h>
#include <CPed.h>
#include <CPlayerPed.h>
#include <CPlayerInfo.h>
#include <CVehicle.h>
#include <CWeapon.h>
#include <eWeaponType.h>
#include <CStreaming.h>
#include <CWorld.h>
#include <CWaterLevel.h>
#include <CText.h>
#include <CColModel.h>
#include <CColPoint.h>
#include "TravelCatalog.h"
#include "waypoint_access.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <iomanip>
#include "imgui.h"
#include "Menu.h"
#include "Render.h"
#include "VehicleList.h"
#include "PedList.h"
#include "AnimationList.h"
#include "AnimationPlayer.h"
#include "Hotkeys.h"
#include "ModelPreview.h"
#include "TrainerUI.h"
#include "Branding.h"
#include <d3d9.h>
#include "WeaponIcons.h"
#include "ActionBindings.h"
#include "AutoWalk.h"
#include "Wardrobe.h"
#include "model_access.h"
#include <CBaseModelInfo.h>

using namespace plugin;

namespace
{
    using TrainerUI::Button;
    using TrainerUI::SmallButton;
    using TrainerUI::Separator;

    struct WeaponEntry { const char *name; eWeaponType id; };

    const WeaponEntry kWeapons[] = {
        {"Brass Knuckles", WEAPONTYPE_BRASSKNUCKLE},
        {"Knife", WEAPONTYPE_KNIFE},
        {"Baseball Bat", WEAPONTYPE_BASEBALLBAT},
        {"Chainsaw", WEAPONTYPE_CHAINSAW},
        {"Katana", WEAPONTYPE_KATANA},
        {"Grenade", WEAPONTYPE_GRENADE},
        {"Molotov", WEAPONTYPE_MOLOTOV},
        {"Pistol", WEAPONTYPE_PISTOL},
        {"Silenced Pistol", WEAPONTYPE_PISTOL_SILENCED},
        {"Desert Eagle", WEAPONTYPE_DESERT_EAGLE},
        {"Shotgun", WEAPONTYPE_SHOTGUN},
        {"Sawnoff Shotgun", WEAPONTYPE_SAWNOFF},
        {"SPAS-12", WEAPONTYPE_SPAS12},
        {"Micro Uzi", WEAPONTYPE_MICRO_UZI},
        {"MP5", WEAPONTYPE_MP5},
        {"AK-47", WEAPONTYPE_AK47},
        {"M4", WEAPONTYPE_M4},
        {"Tec-9", WEAPONTYPE_TEC9},
        {"Country Rifle", WEAPONTYPE_COUNTRYRIFLE},
        {"Sniper Rifle", WEAPONTYPE_SNIPERRIFLE},
        {"Rocket Launcher", WEAPONTYPE_RLAUNCHER},
        {"Rocket Launcher (HS)", WEAPONTYPE_RLAUNCHER_HS},
        {"Flamethrower", WEAPONTYPE_FTHROWER},
        {"Minigun", WEAPONTYPE_MINIGUN},
        {"Satchel Charge", WEAPONTYPE_SATCHEL_CHARGE},
        {"Spraycan", WEAPONTYPE_SPRAYCAN},
        {"Fire Extinguisher", WEAPONTYPE_EXTINGUISHER},
        {"Camera", WEAPONTYPE_CAMERA},
        {"Parachute", WEAPONTYPE_PARACHUTE},
    };

    int g_weaponIndex = 0;
    int g_ammo = 120;
    float g_teleport[3] = {0.f, 0.f, 20.f};
    int g_vehicleId = 400;
    char g_vehicleFilter[64] = "";
    bool g_spawnInVehicle = true;

    // Spawns a vehicle for the player, warping the player into the driver
    // seat with the engine running when g_spawnInVehicle is set (the
    // default). Warping only happens if the player is on foot and alive,
    // so this never displaces an existing driver or fights another task.
    CVehicle* SpawnVehicleForPlayer(int modelId)
    {
        CVehicle* spawned = CCheat::VehicleCheat(modelId);
        if (!spawned) return spawned;
        spawned->bHasBeenOwnedByPlayer = true;
        if (g_spawnInVehicle)
        {
            CPlayerPed* ped = FindPlayerPed();
            if (ped && ped->m_fHealth > 0 && !ped->bInVehicle)
            {
                spawned->SetDriver(ped);
                ped->bInVehicle = true;
                ped->m_pVehicle = spawned;
                spawned->bEngineOn = true;
            }
        }
        return spawned;
    }
    bool g_infiniteHealth = false;
    bool g_infiniteArmour = false;
    bool g_neverWanted = false;
    bool g_godMode = false;
    bool g_freezePlayer = false;
    CVector g_frozenPos{};
    bool g_infiniteAmmo = false;

    char g_animLib[32] = "DANCING";
    char g_animName[32] = "DAN_LOOP_A";
    float g_animBlendDelta = 8.0f;
    int g_animTimeMs = -1;
    bool g_animLoop = false;
    bool g_animLockX = false;
    bool g_animLockY = false;
    bool g_animHoldLastFrame = false;
    char g_animFilter[64] = "";
    std::vector<AnimationPlayer::Step> g_animSequence;
    bool g_seqLoop = true;

    AnimationPlayer::Step BuildStepFromFields()
    {
        int flags = 0;

        if (g_animLockX) flags |= 0x40;
        if (g_animLockY) flags |= 0x80;
        return {g_animLib, g_animName, flags, g_animBlendDelta, g_animTimeMs, g_animHoldLastFrame};
    }

    int g_skinId = 0;
    char g_skinFilter[64] = "";

    struct LocationEntry {std::string name;float x,y,z;TravelCatalog::Zone zone;};
    std::vector<LocationEntry> kLocations;
    int g_locationIndex=-1;
    void LoadLocations(){
        if(!kLocations.empty())return;
        char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);
        std::string root=exe;root.resize(root.find_last_of("\\/")+1);
        const auto zones=TravelCatalog::Load(root);
        for(const auto& zone:zones){
            std::string name=zone.key;
            if(name.rfind("City: ",0)!=0){
                const char* label=TheText.Get(zone.key.c_str());if(label&&*label&&std::strcmp(label," ")&&std::strcmp(label,zone.key.c_str()))name=label;
                const TravelCatalog::Zone* region=nullptr;float smallest=FLT_MAX;
                for(const auto& candidate:zones)if(candidate.key.rfind("City: ",0)==0&&zone.x>=candidate.minX&&zone.x<=candidate.maxX&&zone.y>=candidate.minY&&zone.y<=candidate.maxY){
                    float area=(candidate.maxX-candidate.minX)*(candidate.maxY-candidate.minY);if(area<smallest){smallest=area;region=&candidate;}
                }
                if(region)name=region->key.substr(6)+" / "+name;
            }
            auto old=std::find_if(kLocations.begin(),kLocations.end(),[&](auto& e){return e.zone.key==zone.key;});
            // Choose the widest constituent district, not an arbitrary tiny sliver.
            float size=(zone.maxX-zone.minX)*(zone.maxY-zone.minY);
            if(old==kLocations.end())kLocations.push_back({name,zone.x,zone.y,zone.z,zone});
            else if(size>(old->zone.maxX-old->zone.minX)*(old->zone.maxY-old->zone.minY))*old={name,zone.x,zone.y,zone.z,zone};
        }
        // A city shortcut uses a real district inside its bounds where possible,
        // rather than the geometric center of a large rectangle (often water).
        for(auto& entry:kLocations)if(entry.name.rfind("City: ",0)==0){
            const TravelCatalog::Zone* best=nullptr;float distance=FLT_MAX;
            for(const auto& zone:zones)if(zone.key.rfind("City: ",0)!=0&&zone.x>entry.zone.minX&&zone.x<entry.zone.maxX&&zone.y>entry.zone.minY&&zone.y<entry.zone.maxY&&(zone.maxX-zone.minX)<2000&&(zone.maxY-zone.minY)<2000){
                float dx=zone.x-entry.x,dy=zone.y-entry.y,d=dx*dx+dy*dy;if(d<distance){distance=d;best=&zone;}
            }
            if(best){entry.x=best->x;entry.y=best->y;entry.z=best->z;entry.zone=*best;}
        }
        std::sort(kLocations.begin(),kLocations.end(),[](auto& a,auto& b){return a.name<b.name;});
    }

    void ResetTrainer(){g_infiniteHealth=g_infiniteArmour=g_neverWanted=g_godMode=g_freezePlayer=g_infiniteAmmo=false;if(AutoWalk::Enabled())AutoWalk::Toggle();CCheat::ResetCheats();}
    void DrawPlayerTab()
    {
        CPlayerPed *ped = FindPlayerPed();
        if (!ped)
        {
            ImGui::TextDisabled("No player ped found.");
            return;
        }

        auto* info=ped->GetPlayerInfoForThisPlayerPed();
        TrainerUI::PlayerPanel panel{g_infiniteHealth,g_infiniteArmour,g_neverWanted,g_godMode,g_freezePlayer,info!=nullptr,info?info->m_nMoney:0};
        bool autowalk=AutoWalk::Enabled();if(TrainerUI::Toggle("Autowalk",&autowalk))AutoWalk::Toggle();
        TrainerUI::StatusLine(AutoWalk::Status(),1);
        if(Button("Restore Movement")){g_freezePlayer=false;if(AutoWalk::Enabled())AutoWalk::Toggle();AnimationPlayer::Stop();}

        const auto action=TrainerUI::DrawPlayerPanel(panel);
        if(panel.freeze && !g_freezePlayer)g_frozenPos=ped->GetPosition();
        g_infiniteHealth=panel.health;g_infiniteArmour=panel.armour;g_neverWanted=panel.wanted;
        g_godMode=panel.god;g_freezePlayer=panel.freeze;
        if(info)info->m_nMoney=panel.money;
        using A=TrainerUI::PlayerAction;
        switch(action){
            case A::Jump:ped->m_vecMoveSpeed.z+=.6f;break;
            case A::Launch:if(ped->m_pVehicle)ped->m_pVehicle->m_vecMoveSpeed.z+=.6f;break;
            case A::Restore:CCheat::MoneyArmourHealthCheat();break;
            case A::Heal:CCheat::HealthCheat();break;
            case A::WantedUp:CCheat::WantedLevelUpCheat();break;
            case A::WantedDown:CCheat::WantedLevelDownCheat();break;
            case A::ClearWanted:CCheat::NotWantedCheat();break;
            case A::Fat:CCheat::FatCheat();break;
            case A::Skinny:CCheat::SkinnyCheat();break;
            case A::Muscle:CCheat::MuscleCheat();break;
            case A::Stamina:CCheat::StaminaCheat();break;
            case A::Adrenaline:CCheat::AdrenalineCheat();break;
            case A::Ninja:CCheat::NinjaCheat();break;
            case A::WeaponSkills:CCheat::WeaponSkillsCheat();break;
            case A::VehicleSkills:CCheat::VehicleSkillsCheat();break;
            case A::Suicide:CCheat::SuicideCheat();break;
            case A::Reset:ResetTrainer();break;
            default:break;
        }
    }

    void DrawWeaponsTab()
    {
        CPlayerPed *ped = FindPlayerPed();
        if (!ped)
        {
            ImGui::TextDisabled("No player ped found.");
            return;
        }

        TrainerUI::WeaponChoice choices[IM_ARRAYSIZE(kWeapons)];
        for(int i=0;i<IM_ARRAYSIZE(kWeapons);++i){
            const int id=(int)kWeapons[i].id;
            choices[i]={kWeapons[i].name,reinterpret_cast<ImTextureID>(WeaponIcons::Get(kWeapons[i].id)),id>=16&&id!=46};
        }
        auto action=TrainerUI::DrawWeaponPanel(choices,IM_ARRAYSIZE(kWeapons),g_weaponIndex,g_ammo,g_infiniteAmmo);
        using W=TrainerUI::WeaponAction;
        switch(action){
            case W::Give:ped->GiveWeapon(kWeapons[g_weaponIndex].id,choices[g_weaponIndex].usesAmmo?static_cast<unsigned>(g_ammo):1u,false);break;
            case W::Set1:CCheat::WeaponCheat1();break;
            case W::Set2:CCheat::WeaponCheat2();break;
            case W::Set3:CCheat::WeaponCheat3();break;
            case W::All:for(const auto& weapon:kWeapons)ped->GiveWeapon(weapon.id,static_cast<unsigned>(g_ammo),false);break;
            case W::Remove:ped->ClearWeapons();break;
            default:break;
        }
        static std::string feedback;static double until=0;
        if(action!=W::None){feedback=action==W::Give?std::string("Added ")+kWeapons[g_weaponIndex].name:action==W::Remove?"Weapons removed":"Loadout applied";until=ImGui::GetTime()+3;}
        TrainerUI::StatusLine(ImGui::GetTime()<until?feedback.c_str():"",1);
    }

    void QuickAction(const char* action);
    void DrawVehiclesTab()
    {
        const auto &vehicles = VehicleList::Get();

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##g_vehicleFilter", "Search vehicles...", g_vehicleFilter, sizeof(g_vehicleFilter));

        const float browserHeight=std::clamp(ImGui::GetContentRegionAvail().y-90.f,200.f,430.f);
        ImGui::BeginTable("##VehicleBrowser", 2, ImGuiTableFlags_SizingStretchProp);
        ImGui::TableSetupColumn("Models", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.56f);
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("##VehicleListBox", ImVec2(0, browserHeight)))
        {
            std::string filter = g_vehicleFilter;
            std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

            for (const auto &entry : vehicles)
            {
                if (!filter.empty())
                {
                    std::string lowerName = entry.name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                    if (lowerName.find(filter) == std::string::npos) continue;
                }

                bool selected = (g_vehicleId == entry.id);
                std::string label = entry.name + " (" + std::to_string(entry.id) + ")";
                if (TrainerUI::ModelRow(entry.name.c_str(), entry.id, selected)) { g_vehicleId = entry.id; ModelPreview::Refresh("vehicle", entry.id); }
                if (selected) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        auto selection=std::find_if(vehicles.begin(),vehicles.end(),[](const auto& e){return e.id==g_vehicleId;});
        const bool vehicleIdKnown=selection!=vehicles.end();
        if(vehicleIdKnown)ImGui::TextWrapped("%s",selection->name.c_str());
        else ImGui::TextDisabled("Choose a vehicle");
        ModelPreview::Draw("vehicle", g_vehicleId, ImGui::GetContentRegionAvail().x, std::max(100.f,browserHeight-140.f));
        ImGui::BeginDisabled(!vehicleIdKnown);
        if(TrainerUI::PrimaryButton("Spawn Vehicle")) SpawnVehicleForPlayer(g_vehicleId);
        ImGui::EndDisabled();
        ImGui::Checkbox("Spawn in vehicle (engine running)", &g_spawnInVehicle);
        if(ImGui::CollapsingHeader("Choose by ID"))TrainerUI::NumberField("Vehicle ID",&g_vehicleId,110);
        ImGui::EndTable();

        Separator();
        TrainerUI::Heading("Current Vehicle");
        if (CPlayerPed *ped = FindPlayerPed(); ped && ped->bInVehicle && ped->m_pVehicle)
        {
            CVehicle *vehicle = ped->m_pVehicle;
            ImGui::Text("Vehicle condition: %.0f%%", std::clamp(vehicle->m_fHealth / 10.f,0.f,100.f));

            if (Button("Repair")) QuickAction("Repair");
            TrainerUI::Inline();
            if (Button("Flip Upright")) QuickAction("Flip Upright");
        }
        else
        {
            ImGui::TextDisabled("Get into a vehicle to repair or flip it.");
        }

        Separator();
        if(ImGui::CollapsingHeader("Special vehicles & equipment")){
        if (Button("Tank")) CCheat::TankCheat();
        TrainerUI::Inline();
        if (Button("Hearse")) CCheat::HearseCheat();
        TrainerUI::Inline();
        if (Button("Trashmaster")) CCheat::TrashmasterCheat();
        TrainerUI::Inline();
        if (Button("Dozer")) CCheat::DozerCheat();

        if (Button("Quad")) CCheat::QuadCheat();
        TrainerUI::Inline();
        if (Button("Golfcart")) CCheat::GolfcartCheat();
        TrainerUI::Inline();
        if (Button("Monster Truck")) CCheat::MonsterTruckCheat();
        TrainerUI::Inline();
        if (Button("Stunt Plane")) CCheat::StuntPlaneCheat();

        if (Button("Parachute")) CCheat::ParachuteCheat();
        TrainerUI::Inline();
        if (Button("Jetpack")) CCheat::JetpackCheat();

        if (Button("Hydra (Apache)")) CCheat::ApacheCheat();
        TrainerUI::Inline();
        if (Button("Flyboy")) CCheat::FlyboyCheat();
        TrainerUI::Inline();
        if (Button("Tanker Rig")) CCheat::TankerCheat();
        TrainerUI::Inline();
        if (Button("Vortex Hovercraft")) CCheat::VortexCheat();

        TrainerUI::Heading("Stock Cars (demolition derby)");
        if (Button("Stock Car 1")) CCheat::StockCarCheat();
        TrainerUI::Inline();
        if (Button("Stock Car 2")) CCheat::StockCar2Cheat();
        TrainerUI::Inline();
        if (Button("Stock Car 3")) CCheat::StockCar3Cheat();
        TrainerUI::Inline();
        if (Button("Stock Car 4")) CCheat::StockCar4Cheat();

        }
        Separator();
        if(ImGui::CollapsingHeader("Traffic effects")){
        if (Button("All Cars Great")) CCheat::AllCarsAreGreatCheat();
        TrainerUI::Inline();
        if (Button("All Cars Cheap")) CCheat::AllCarsAreShitCheat();
        TrainerUI::Inline();
        if (Button("Black Traffic")) CCheat::BlackCarsCheat();
        TrainerUI::Inline();
        if (Button("Pink Traffic")) CCheat::PinkCarsCheat();
        if (Button("Blow Up Nearby Cars")) CCheat::BlowUpCarsCheat();
        TrainerUI::Inline();
        if (Button("Aggressive Drivers")) CCheat::DrivebyCheat();
        }
    }

    void DrawWorldTab()
    {
        TrainerUI::Heading("Weather");
        if (Button("Sunny")) CCheat::SunnyWeatherCheat();
        TrainerUI::Inline();
        if (Button("Extra Sunny")) CCheat::ExtraSunnyWeatherCheat();
        TrainerUI::Inline();
        if (Button("Cloudy")) CCheat::CloudyWeatherCheat();
        TrainerUI::Inline();
        if (Button("Rainy")) CCheat::RainyWeatherCheat();

        if (Button("Foggy")) CCheat::FoggyWeatherCheat();
        TrainerUI::Inline();
        if (Button("Sandstorm")) CCheat::SandstormCheat();
        TrainerUI::Inline();
        if (Button("Thunderstorm")) CCheat::StormCheat();

        Separator();
        TrainerUI::Heading("Time");
        if (Button("Midnight")) CCheat::MidnightCheat();
        TrainerUI::Inline();
        if (Button("Dusk")) CCheat::DuskCheat();
        TrainerUI::Inline();
        if (Button("Fast Time")) CCheat::FastTimeCheat();
        TrainerUI::Inline();
        if (Button("Slow Time")) CCheat::SlowTimeCheat();

        Separator();
        if(ImGui::CollapsingHeader("Chaos & gangs")){
        if (Button("Riot")) CCheat::RiotCheat();
        TrainerUI::Inline();
        if (Button("Gang War")) CCheat::GangsCheat();
        TrainerUI::Inline();
        if (Button("Gangland")) CCheat::GangLandCheat();
        TrainerUI::Inline();
        if (Button("Mayhem")) CCheat::MayhemCheat();
        if (Button("Max Wanted")) CCheat::WantedCheat();

        if (Button("Everybody Attacks Player")) CCheat::EverybodyAttacksPlayerCheat();
        TrainerUI::Inline();
        if (Button("Countryside Invasion")) CCheat::CountrysideInvasionCheat();

        }
        Separator();
        if(ImGui::CollapsingHeader("People & party effects")){
        if (Button("Beach Party")) CCheat::BeachPartyCheat();
        TrainerUI::Inline();
        if (Button("Funhouse")) CCheat::FunhouseCheat();
        TrainerUI::Inline();
        if (Button("Village People")) CCheat::VillagePeopleCheat();
        if (Button("Lovefist")) CCheat::LovefistCheat();
        TrainerUI::Inline();
        if (Button("Elvis Lives")) CCheat::ElvisLivesCheat();
        TrainerUI::Inline();
        if (Button("Love Conquers All")) CCheat::LoveConquersAllCheat();
        }
    }

    int pendingSkin=-1;
    std::string skinStatus;
    void SetPlayerSkin(CPlayerPed*,int modelId){pendingSkin=modelId;skinStatus="Applying character...";}
    void ProcessSkin(CPlayerPed* ped){
        if(pendingSkin<0)return;
        const int id=pendingSkin;pendingSkin=-1;
        if(!ped->m_pRwClump||ped->m_fHealth<=0||ped->bInVehicle){skinStatus="Stand on foot before changing character.";return;}
        auto* info=static_cast<CBaseModelInfo*>(valkyrie_models::Find(id));
        if(!info||info->GetModelType()!=MODEL_INFO_PED){skinStatus="That character is unavailable.";return;}
        if(id==0&&(!ped->m_pPlayerData||!ped->m_pPlayerData->m_pPedClothesDesc)){skinStatus="CJ clothing data is unavailable.";return;}
        CStreaming::RequestModel(id,2);CStreaming::LoadAllRequestedModels(false);
        if(!info->m_pRwObject){CStreaming::SetModelIsDeletable(id);skinStatus="Character did not load. Try again.";return;}
        AnimationPlayer::Stop();AnimationPlayer::Update();ModelPreview::BeforeSkinChange();
        ped->DeleteRwObject();ped->m_nModelIndex=-1;ped->SetModelIndex(static_cast<unsigned>(id));
        if(id==0){CClothes::RebuildPlayer(ped,false);ped->m_nAnimGroup=(eAnimGroup)CClothes::GetDefaultPlayerMotionGroup();}
        CStreaming::SetModelIsDeletable(id);g_skinId=id;skinStatus=id==0?"Back to CJ.":"Character applied.";
    }

    void DrawSkinTab()
    {
        CPlayerPed *ped = FindPlayerPed();
        if (!ped)
        {
            ImGui::TextDisabled("No player ped found.");
            return;
        }

        if(TrainerUI::PrimaryButton("Return to CJ"))SetPlayerSkin(ped,0);
        TrainerUI::StatusLine(skinStatus.c_str(),1);
        Wardrobe::Draw(ped);
        const auto &peds = PedList::Get();

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##g_skinFilter", "Search characters...", g_skinFilter, sizeof(g_skinFilter));

        const float browserHeight=std::clamp(ImGui::GetContentRegionAvail().y-90.f,200.f,430.f);
        ImGui::BeginTable("##SkinBrowser", 2, ImGuiTableFlags_SizingStretchProp);
        ImGui::TableSetupColumn("Models", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.56f);
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("##SkinListBox", ImVec2(0, browserHeight)))
        {
            std::string filter = g_skinFilter;
            std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

            for (const auto &entry : peds)
            {
                if (!filter.empty())
                {
                    std::string lowerName = entry.name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                    if (lowerName.find(filter) == std::string::npos) continue;
                }

                bool selected = (g_skinId == entry.id);
                std::string label = entry.name + " (" + std::to_string(entry.id) + ")";
                if (TrainerUI::ModelRow(entry.name.c_str(), entry.id, selected)) { g_skinId = entry.id; ModelPreview::Refresh("ped", entry.id); }
                if (selected) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        auto selection=std::find_if(peds.begin(),peds.end(),[](const auto& e){return e.id==g_skinId;});
        const bool skinIdKnown=selection!=peds.end();
        if(skinIdKnown)ImGui::TextWrapped("%s",selection->name.c_str());
        else ImGui::TextDisabled("Choose a character");
        ModelPreview::Draw("ped", g_skinId, ImGui::GetContentRegionAvail().x, std::max(100.f,browserHeight-140.f));
        ImGui::BeginDisabled(!skinIdKnown);
        if(TrainerUI::PrimaryButton("Set Skin")){SetPlayerSkin(ped,g_skinId);}
        ImGui::EndDisabled();
        if(ImGui::CollapsingHeader("Choose by ID"))TrainerUI::NumberField("Character ID",&g_skinId,110);
        ImGui::EndTable();

        Separator();
        // Deliberately one-shot, not an unattended auto-cycle: some peds.ide
        // entries are known-bad/incompatible skins (per doctor-valkyrie's
        // own crash database), and silently plowing through hundreds of
        // them on a timer hits those without you choosing to.
        if (Button("Random") && !peds.empty())
        {
            g_skinId = peds[rand() % peds.size()].id;
            SetPlayerSkin(ped, g_skinId);
        }
    }

    void DrawAnimationsTab()
    {
        CPlayerPed *ped = FindPlayerPed();
        if (!ped)
        {
            ImGui::TextDisabled("No player ped found.");
            return;
        }

        const auto &anims = AnimationList::Get();


        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##g_animFilter", "Search animations...", g_animFilter, sizeof(g_animFilter));

        if (ImGui::BeginListBox("##AnimListBox", ImVec2(-FLT_MIN, std::clamp(ImGui::GetContentRegionAvail().y-365.f,100.f,220.f))))
        {
            static std::vector<std::string> labels,searchText;
            if(labels.size()!=anims.size()){labels.clear();searchText.clear();for(const auto& entry:anims){labels.push_back(entry.library+" / "+entry.name);auto lower=labels.back();std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return (char)tolower(c);});searchText.push_back(std::move(lower));}}
            static std::string previousQuery;static std::vector<int> matches;static bool initialized=false;
            std::string query=g_animFilter;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return (char)tolower(c);});
            if(!initialized||query!=previousQuery){initialized=true;previousQuery=query;matches.clear();for(int i=0;i<(int)anims.size();++i)if(query.empty()||searchText[i].find(query)!=std::string::npos)matches.push_back(i);}
            ImGuiListClipper clipper;clipper.Begin((int)matches.size());
            while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row){int i=matches[row];const auto& entry=anims[i];
                bool selected=entry.library==g_animLib&&entry.name==g_animName;
                if(ImGui::Selectable(labels[i].c_str(),selected)){strncpy_s(g_animLib,entry.library.c_str(),sizeof(g_animLib)-1);strncpy_s(g_animName,entry.name.c_str(),sizeof(g_animName)-1);}
            }
            if(matches.empty())ImGui::TextDisabled("No matching animations.");
            ImGui::EndListBox();
        }

        ImGui::Text("Selected: %s",g_animName);
        TrainerUI::AnimationPanel transport{g_animLoop?1:g_animHoldLastFrame?2:0,AnimationPlayer::Active(),AnimationPlayer::Paused(),AnimationPlayer::Position(),AnimationPlayer::Length(),AnimationPlayer::PlaybackSpeed()};
        auto control=TrainerUI::DrawAnimationPanel(transport);
        g_animLoop=transport.mode==1;g_animHoldLastFrame=transport.mode==2;
        switch(control){
            case TrainerUI::AnimationAction::Play:AnimationPlayer::PlaySingle(BuildStepFromFields(),g_animLoop);break;
            case TrainerUI::AnimationAction::Stop:AnimationPlayer::Stop();break;
            case TrainerUI::AnimationAction::Pause:AnimationPlayer::Pause(!AnimationPlayer::Paused());break;
            case TrainerUI::AnimationAction::Restart:AnimationPlayer::Restart();break;
            case TrainerUI::AnimationAction::Seek:AnimationPlayer::Seek(transport.seek);break;
            case TrainerUI::AnimationAction::Speed:AnimationPlayer::Speed(transport.speed);break;
            default:break;
        }
        if (Button("Add To Sequence"))
        {
            if(g_animSequence.size()<256)g_animSequence.push_back(BuildStepFromFields());
        }

        std::string animationStatus=AnimationPlayer::Status();
        if(AnimationPlayer::IsPlaying())animationStatus+="  ("+std::to_string(AnimationPlayer::CurrentStepIndex()+1)+"/"+std::to_string(AnimationPlayer::TotalSteps())+")";
        TrainerUI::StatusLine(animationStatus.c_str());

        if(ImGui::CollapsingHeader("Animation settings")){
            ImGui::TextUnformatted("Library");ImGui::InputText("##Library",g_animLib,sizeof(g_animLib));
            ImGui::TextUnformatted("Animation name");ImGui::InputText("##Animation",g_animName,sizeof(g_animName));
            ImGui::TextUnformatted("Blend speed");ImGui::InputFloat("##Blend",&g_animBlendDelta);
            TrainerUI::NumberField("Duration in milliseconds (-1 = automatic)",&g_animTimeMs,190);
            TrainerUI::Toggle("Lock X", &g_animLockX);TrainerUI::Inline();TrainerUI::Toggle("Lock Y", &g_animLockY);

        }

        Separator();
        if(ImGui::CollapsingHeader("Sequence builder")){
        ImGui::TextDisabled("Plays back to back, in order.");
        if (g_animSequence.empty())
        {
            ImGui::TextDisabled("(empty - use \"Add To Sequence\" above)");
        }
        else
        {
            int removeIndex = -1;
            for (int i = 0; i < (int)g_animSequence.size(); i++)
            {
                ImGui::PushID(i);
                ImGui::Text("%d. %s / %s", i + 1, g_animSequence[i].library.c_str(), g_animSequence[i].name.c_str());
                TrainerUI::Inline();
                if (SmallButton("Remove")) removeIndex = i;
                ImGui::PopID();
            }
            if (removeIndex >= 0) g_animSequence.erase(g_animSequence.begin() + removeIndex);
        }

        TrainerUI::Toggle("Loop Sequence", &g_seqLoop);
        TrainerUI::Inline();
        if (TrainerUI::PrimaryButton("Play Sequence") && !g_animSequence.empty())
        {
            AnimationPlayer::PlaySequence(g_animSequence, g_seqLoop);
        }
        TrainerUI::Inline();
        if (Button("Clear Sequence")) g_animSequence.clear();

        }
        Separator();
        if(ImGui::CollapsingHeader("Animation shortcuts (F5 - F8)")){
        for (int i = 0; i < Hotkeys::kSlotCount; i++)
        {
            ImGui::PushID(100 + i);
            const Hotkeys::Slot &slot = Hotkeys::GetSlot(i);
            ImGui::Text("F%d:", 5 + i);
            TrainerUI::Inline();
            ImGui::TextUnformatted(slot.used ? slot.label.c_str() : "(unassigned)");
            TrainerUI::Inline();
            if (SmallButton("Assign Anim"))
            {
                std::string label = std::string(g_animLib) + " / " + g_animName;
                Hotkeys::AssignSlot(i, label, {BuildStepFromFields()}, g_animLoop);
            }
            TrainerUI::Inline();
            if (SmallButton("Assign Sequence") && !g_animSequence.empty())
            {
                std::string label = "Sequence (" + std::to_string(g_animSequence.size()) + " steps)";
                Hotkeys::AssignSlot(i, label, g_animSequence, g_seqLoop);
            }
            TrainerUI::Inline();
            if (SmallButton("Clear")) Hotkeys::ClearSlot(i);
            ImGui::PopID();
        }
        }
    }

    bool IsSaneTeleportCoord(const CVector &dest)
    {
        // Rejects NaN/Inf (garbage InputFloat state) and absurd magnitudes
        // that would index way outside the world's sector grid - a classic
        // SA-engine crash from typing something like "99999999" into a
        // coordinate field. The bound is generous (PE's world spans several
        // combined city maps, far past vanilla SA's ~3000 unit extent).
        const float kMaxCoord = 200000.0f;
        return std::isfinite(dest.x) && std::isfinite(dest.y) && std::isfinite(dest.z)
            && std::fabs(dest.x) < kMaxCoord && std::fabs(dest.y) < kMaxCoord && std::fabs(dest.z) < kMaxCoord;
    }

    CVector g_previousLocation{};unsigned g_previousArea=0;bool g_previousLocationValid=false;
    std::string g_travelStatus="Place a waypoint on the pause map, then travel here.";
    struct TravelRequest {bool pending=false,ground=true;CVector destination{};CPlayerPed* player=nullptr;unsigned area=0;TravelCatalog::Zone zone{};bool searchZone=false;};
    TravelRequest g_travel;
    bool TeleportPedTo(CPlayerPed* ped,const CVector& dest,bool ground=false,const TravelCatalog::Zone* zone=nullptr){
        if(!ped||!IsSaneTeleportCoord(dest)||g_travel.pending)return false;
        if(ground&&ped->m_nAreaCode){g_travelStatus="Leave the interior before travelling to the map.";return false;}
        g_travel={true,ground,dest,ped,ped->m_nAreaCode,zone?*zone:TravelCatalog::Zone{},zone!=nullptr};
        g_travelStatus="Loading destination...";return true;
    }
    void WaypointTravel(){float x=0,y=0;if(!valkyrie_waypoint::Position(x,y)){g_travelStatus="Place a waypoint on the pause map first.";return;}TeleportPedTo(FindPlayerPed(),CVector(x,y,2000.f),true);}
    void ProcessTravelRequest(){
        if(!g_travel.pending||ActionBindings::Paused())return;
        const auto request=g_travel;g_travel.pending=false;
        auto* ped=FindPlayerPed();if(!ped||ped!=request.player||ped->m_nAreaCode!=request.area){g_travelStatus="Travel cancelled: player changed.";return;}
        CVector dest=request.destination;bool found=!request.ground;
        // Only execute streaming on the game update, never inside ImGui rendering.
        const float offsets[][2]={{.5f,.5f},{.25f,.25f},{.75f,.25f},{.25f,.75f},{.75f,.75f},{.5f,.25f},{.5f,.75f},{.25f,.5f},{.75f,.5f}};
        for(int attempt=0;attempt<(request.searchZone?9:1);++attempt){
            if(request.searchZone){dest.x=request.zone.minX+(request.zone.maxX-request.zone.minX)*offsets[attempt][0];dest.y=request.zone.minY+(request.zone.maxY-request.zone.minY)*offsets[attempt][1];}
            CStreaming::LoadSceneCollision(&dest);CStreaming::LoadScene(&dest);
            if(!request.ground)break;
            CColPoint surface{};CEntity* entity=nullptr;
            bool hit=CWorld::ProcessVerticalLine(dest,-1000.f,surface,entity,true,false,false,true,false,false,nullptr);
            if(!hit||!std::isfinite(surface.m_vecPoint.z)||surface.m_vecNormal.z<.65f)continue;
            float z=surface.m_vecPoint.z,water=0;
            bool submerged=CWaterLevel::GetWaterLevelNoWaves(dest.x,dest.y,z,&water)&&water>z+.15f;
            if(std::fabs(z)<10000&&!submerged){dest.z=z;found=true;break;}
        }
        if(!found){auto old=ped->GetPosition();CStreaming::LoadSceneCollision(&old);CStreaming::LoadScene(&old);g_travelStatus="No dry ground found. Choose another spot on the map.";return;}
        const auto oldPosition=ped->GetPosition();const auto oldArea=ped->m_nAreaCode;
        auto* vehicle=ped->bInVehicle?ped->m_pVehicle:nullptr;
        CPhysical* target=vehicle?static_cast<CPhysical*>(vehicle):static_cast<CPhysical*>(ped);
        if(request.ground){float clearance=1.1f;if(vehicle){auto* col=vehicle->GetColModel();if(col)clearance=std::clamp(-col->m_boundBox.m_vecMin.z+.35f,.5f,20.f);vehicle->SetOrientation(0,0,vehicle->GetHeading());}dest.z+=clearance;}
        if(AutoWalk::Enabled())AutoWalk::Toggle();AnimationPlayer::Stop();
        target->Teleport(dest,false);target->m_vecMoveSpeed={0,0,0};target->m_vecTurnSpeed={0,0,0};
        g_frozenPos=dest;
        g_previousLocation=oldPosition;g_previousArea=oldArea;g_previousLocationValid=true;
        g_travelStatus="Arrived. Go Back returns to your previous location.";
    }

    void DrawTeleportTab(){
        auto* ped=FindPlayerPed();if(!ped){TrainerUI::StatusLine("Load a game to travel.",2);return;}
        ImGui::BeginDisabled(g_travel.pending);
        if(TrainerUI::PrimaryButton("Teleport to waypoint"))WaypointTravel();
        TrainerUI::Inline(100);ImGui::BeginDisabled(!g_previousLocationValid||ped->m_nAreaCode!=g_previousArea);
        if(Button("Go Back"))QuickAction("Go Back");ImGui::EndDisabled();
        TrainerUI::StatusLine(g_travelStatus.c_str(),2);
        Separator();
        int& destination=g_locationIndex;static char filter[80]{};
        ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##location-search","Search cities, districts and landmarks...",filter,sizeof filter);
        std::string query=filter;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(ImGui::BeginListBox("##LocationListBox",ImVec2(-FLT_MIN,std::clamp(ImGui::GetContentRegionAvail().y-125.f,100.f,280.f)))){
            for(int i=0;i<int(kLocations.size());++i){auto name=kLocations[i].name;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});if(!query.empty()&&name.find(query)==name.npos)continue;ImGui::PushID(i);
                if(ImGui::Selectable(kLocations[i].name.c_str(),destination==i)){destination=i;g_teleport[0]=kLocations[i].x;g_teleport[1]=kLocations[i].y;g_teleport[2]=kLocations[i].z;}
                ImGui::PopID();
            }ImGui::EndListBox();
        }
        ImGui::BeginDisabled(destination<0);if(Button("Travel to selected")){auto& e=kLocations[destination];TeleportPedTo(ped,CVector(e.x,e.y,e.z),true,&e.zone);}ImGui::EndDisabled();
        TrainerUI::Inline(150);ImGui::BeginDisabled(kLocations.empty());if(Button("Surprise me")){destination=rand()%kLocations.size();auto& e=kLocations[destination];TeleportPedTo(ped,CVector(e.x,e.y,e.z),true,&e.zone);}ImGui::EndDisabled();
        if(ImGui::CollapsingHeader("Custom coordinates")){
            ImGui::InputFloat3("X / Y / Z",g_teleport);static bool ground=true;ImGui::Checkbox("Place on ground",&ground);
            if(Button("Travel to coordinates")){auto dest=CVector(g_teleport[0],g_teleport[1],ground?2000.f:g_teleport[2]);TeleportPedTo(ped,dest,ground);}
            TrainerUI::Inline(160);if(Button("Use Current Position")){auto pos=ped->GetPosition();g_teleport[0]=pos.x;g_teleport[1]=pos.y;g_teleport[2]=pos.z;}
        }ImGui::EndDisabled();
    }

    struct Bookmark {bool valid=false;CVector position{};unsigned area=0;} g_bookmark;
    struct SavedWeapon {int type;unsigned ammo;};std::vector<SavedWeapon> g_loadout;bool g_loadoutSaved=false;
    int g_savedRide=-1;
    std::string QuickStatePath(){char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);std::string path=exe;path.resize(path.find_last_of("\\/")+1);return path+"valkyrie-quick-state.cfg";}
    void SaveQuickState(){std::ofstream f(QuickStatePath());f<<std::setprecision(9)<<g_bookmark.valid<<' '<<g_bookmark.position.x<<' '<<g_bookmark.position.y<<' '<<g_bookmark.position.z<<' '<<g_bookmark.area<<'\n'<<g_savedRide<<' '<<g_loadoutSaved<<' '<<g_loadout.size()<<'\n';for(auto& w:g_loadout)f<<w.type<<' '<<w.ammo<<'\n';}
    void LoadQuickState(){std::ifstream f(QuickStatePath());unsigned count=0;Bookmark b;int ride=-1;bool loadout=false;
        if(!(f>>b.valid>>b.position.x>>b.position.y>>b.position.z>>b.area>>ride>>loadout>>count)||count>13)return;
        std::vector<SavedWeapon> weapons;for(unsigned i=0;i<count;++i){SavedWeapon w;if(!(f>>w.type>>w.ammo)||w.type<1||w.type>46||w.ammo>999999)return;weapons.push_back(w);}
        if(b.area>255||!IsSaneTeleportCoord(b.position))b.valid=false;g_bookmark=b;g_savedRide=ride;g_loadoutSaved=loadout;g_loadout=std::move(weapons);
    }
    void Notice(const char* text){CMessages::AddMessageJumpQ(text,1800,0);}
    void QuickAction(const char* action){auto* p=FindPlayerPed();if(!p)return;std::string a=action;auto* v=p->bInVehicle?p->m_pVehicle:nullptr;
        if(a=="Repair"){if(v){reinterpret_cast<void(__thiscall*)(CVehicle*)>((*reinterpret_cast<void***>(v))[50])(v);v->m_fHealth=1000;}else Notice("Get into a vehicle first");}
        else if(a=="Flip Upright"){if(v){float heading=v->GetHeading();v->SetOrientation(0,0,heading);v->GetPosition().z+=.5f;v->m_vecMoveSpeed={0,0,0};v->m_vecTurnSpeed={0,0,0};}else Notice("Get into a vehicle first");}
        else if(a=="Go Back"){if(g_previousLocationValid&&p->m_nAreaCode==g_previousArea){auto destination=g_previousLocation;TeleportPedTo(p,destination);}else Notice("No return location in this interior");}
        else if(a=="Save Location"){g_bookmark={true,p->GetPosition(),p->m_nAreaCode};SaveQuickState();Notice("Location saved");}
        else if(a=="Recall Location"){if(!g_bookmark.valid)Notice("Save a location first");else if(p->m_nAreaCode!=g_bookmark.area)Notice("Return to the saved location's interior first");else TeleportPedTo(p,g_bookmark.position);}
        else if(a=="Save Loadout"){g_loadout.clear();for(const auto& w:p->m_aWeapons)if(w.m_eWeaponType>0&&w.m_eWeaponType<=46)g_loadout.push_back({(int)w.m_eWeaponType,std::min(w.m_nAmmoTotal,999999u)});g_loadoutSaved=true;SaveQuickState();Notice("Loadout saved");}
        else if(a=="Recall Loadout"){if(!g_loadoutSaved)Notice("Save a loadout first");else{p->ClearWeapons();for(auto& w:g_loadout)p->GiveWeapon((eWeaponType)w.type,w.ammo,false);}}
        else if(a=="Save Ride"){if(v){g_savedRide=(unsigned short)v->m_nModelIndex;SaveQuickState();Notice("Vehicle model saved");}else Notice("Get into a vehicle first");}
        else if(a=="Recall Ride"||a=="Random Ride"){const auto& list=VehicleList::Get();int id=g_savedRide;if(a=="Random Ride"&&!list.empty())id=list[rand()%list.size()].id;auto found=std::find_if(list.begin(),list.end(),[&](auto& entry){return entry.id==id;});if(found!=list.end()){g_vehicleId=id;SpawnVehicleForPlayer(id);}else Notice("Save a vehicle first");}
        else if(a=="Sunshine"){CCheat::ExtraSunnyWeatherCheat();}
        else if(a=="Stormy Night"){CCheat::StormCheat();CCheat::MidnightCheat();}
        else if(a=="Beach Day"){CCheat::ExtraSunnyWeatherCheat();CCheat::BeachPartyCheat();}
    }
    void DrawExtrasTab()
    {
        TrainerUI::Heading("Saved tools");
        if(ImGui::BeginTable("##saved-tools",3,ImGuiTableFlags_SizingStretchProp)){
            ImGui::TableSetupColumn("Item",ImGuiTableColumnFlags_WidthStretch,1.4f);
            ImGui::TableSetupColumn("Save",ImGuiTableColumnFlags_WidthStretch,1.f);
            ImGui::TableSetupColumn("Recall",ImGuiTableColumnFlags_WidthStretch,1.f);
            const char* names[]={"Location","Loadout","Vehicle model"};
            const char* save[]={"Save Location","Save Loadout","Save Ride"};
            const char* recall[]={"Recall Location","Recall Loadout","Recall Ride"};
            bool saved[]={g_bookmark.valid,g_loadoutSaved,g_savedRide>=0};
            for(int i=0;i<3;++i){ImGui::TableNextColumn();ImGui::TextUnformatted(names[i]);ImGui::TextDisabled(saved[i]?"Saved":"Nothing saved yet");
                ImGui::TableNextColumn();if(Button(save[i]))QuickAction(save[i]);
                ImGui::TableNextColumn();ImGui::BeginDisabled(!saved[i]);if(Button(recall[i]))QuickAction(recall[i]);ImGui::EndDisabled();}
            ImGui::EndTable();
        }
        Separator();
        TrainerUI::Heading("Just for fun");
        for(const char* label:{"Random Ride","Sunshine","Stormy Night","Beach Day","Go Back"}){if(Button(label))QuickAction(label);TrainerUI::Inline(190);}
        ImGui::NewLine();

        Separator();
        if(ImGui::CollapsingHeader("Combined world effects")){
        ImGui::TextWrapped("These apply several world effects together.");
        if (Button("UNLEASH CHAOS"))
        {
            CCheat::RiotCheat();
            CCheat::GangsCheat();
            CCheat::MayhemCheat();
            CCheat::EverybodyAttacksPlayerCheat();
        }
        TrainerUI::Inline();
        if (Button("PARTY MODE"))
        {
            CCheat::BeachPartyCheat();
            CCheat::FunhouseCheat();
            CCheat::VillagePeopleCheat();
            CCheat::LovefistCheat();
        }
        TrainerUI::Inline();
        if (Button("APOCALYPSE"))
        {
            CCheat::StormCheat();
            CCheat::MidnightCheat();
            CCheat::CountrysideInvasionCheat();
        }

        }
        Separator();
        if(ImGui::CollapsingHeader("On-screen message")){
        ImGui::TextUnformatted("Message");
        static char s_shoutText[128] = "VALKYRIE TRAINER WAS HERE";
        ImGui::InputText("##ShoutText", s_shoutText, sizeof(s_shoutText));
        TrainerUI::Inline();
        if (Button("Shout"))
        {
            CMessages::AddMessageJumpQ(s_shoutText, 2500, 0);
        }

        }
        Separator();
        if(ImGui::CollapsingHeader("More character effects")){
        if (Button("Elvis Lives")) CCheat::ElvisLivesCheat();
        TrainerUI::Inline();
        if (Button("Love Conquers All")) CCheat::LoveConquersAllCheat();
        TrainerUI::Inline();
        if (Button("Ninja Outfit")) CCheat::NinjaCheat();
        }
    }
}

namespace Menu
{
    void ProcessTravel(){ProcessTravelRequest();}
    void InitActions(){
        static bool initialized=false;if(initialized||!FindPlayerPed())return;initialized=true;LoadLocations();
        TrainerUI::SetActionContext(&ActionBindings::Context);
        TrainerUI::SetNavIcons(&WeaponIcons::Nav);
        LoadQuickState();
        auto add=[](const char* label,const char* group,std::function<void()> fn){ActionBindings::Register(label,group,[fn]{if(FindPlayerPed())fn();});};
        // The ones that turn something on or off say whether it is on.
        auto toggle=[](const char* label,const char* group,std::function<void()> fn,std::function<bool()> on){ActionBindings::Register(label,group,[fn]{if(FindPlayerPed())fn();},std::move(on));};
        toggle("Infinite Health","Player",[]{g_infiniteHealth=!g_infiniteHealth;},[]{return g_infiniteHealth;});
        toggle("Infinite Armour","Player",[]{g_infiniteArmour=!g_infiniteArmour;},[]{return g_infiniteArmour;});
        toggle("Never Wanted","Player",[]{g_neverWanted=!g_neverWanted;},[]{return g_neverWanted;});
        toggle("God Mode","Player",[]{g_godMode=!g_godMode;},[]{return g_godMode;});
        toggle("Infinite Ammo","Player",[]{g_infiniteAmmo=!g_infiniteAmmo;},[]{return g_infiniteAmmo;});
        add("Reset All Cheats","Player",[]{ResetTrainer();});
        toggle("Autowalk","Player",[]{AutoWalk::Toggle();},[]{return AutoWalk::Enabled();});
        toggle("Freeze Player","Player",[]{g_freezePlayer=!g_freezePlayer;if(g_freezePlayer)g_frozenPos=FindPlayerPed()->GetPosition();},[]{return g_freezePlayer;});
        add("Full Health / Armour / Money","Player",[]{CCheat::MoneyArmourHealthCheat();});
        add("Health Only","Player",[]{CCheat::HealthCheat();});
        add("Mega Jump","Player",[]{FindPlayerPed()->m_vecMoveSpeed.z+=.6f;});
        add("Launch Vehicle","Player",[]{auto* p=FindPlayerPed();if(p->bInVehicle&&p->m_pVehicle)p->m_pVehicle->m_vecMoveSpeed.z+=.6f;});
        add("Wanted +","Player",[]{CCheat::WantedLevelUpCheat();});
        add("Wanted -","Player",[]{CCheat::WantedLevelDownCheat();});
        add("Clear Wanted","Player",[]{CCheat::NotWantedCheat();});
        add("Fat","Player",[]{CCheat::FatCheat();});
        add("Skinny","Player",[]{CCheat::SkinnyCheat();});
        add("Muscle","Player",[]{CCheat::MuscleCheat();});
        add("Max Stamina","Player",[]{CCheat::StaminaCheat();});
        add("Adrenaline","Player",[]{CCheat::AdrenalineCheat();});
        add("Max Weapon Skills","Player",[]{CCheat::WeaponSkillsCheat();});
        add("Max Vehicle Skills","Player",[]{CCheat::VehicleSkillsCheat();});
        add("Suicide","Player",[]{CCheat::SuicideCheat();});
        add("Spawn Vehicle","Vehicle",[]{const auto& list=VehicleList::Get();if(std::any_of(list.begin(),list.end(),[](auto& e){return e.id==g_vehicleId;}))SpawnVehicleForPlayer(g_vehicleId);});
        add("Return to CJ","Character",[]{SetPlayerSkin(FindPlayerPed(),0);});
        add("Restore Movement","Player",[]{g_freezePlayer=false;if(AutoWalk::Enabled())AutoWalk::Toggle();AnimationPlayer::Stop();});
        add("Set Skin","Character",[]{const auto& list=PedList::Get();if(std::any_of(list.begin(),list.end(),[](auto& e){return e.id==g_skinId;}))SetPlayerSkin(FindPlayerPed(),g_skinId);});
        add("Give Weapon","Weapons",[]{FindPlayerPed()->GiveWeapon(kWeapons[g_weaponIndex].id,std::clamp(g_ammo,0,999999),false);});
        add("Give All Weapons","Weapons",[]{for(auto& w:kWeapons)FindPlayerPed()->GiveWeapon(w.id,std::clamp(g_ammo,0,999999),false);});
        add("Remove All Weapons","Weapons",[]{FindPlayerPed()->ClearWeapons();});
        add("Teleport","Travel",[]{TeleportPedTo(FindPlayerPed(),CVector(g_teleport[0],g_teleport[1],2000.f),true);});
        add("Teleport to waypoint","Travel",[]{WaypointTravel();});
        add("Travel to selected","Travel",[]{if(g_locationIndex>=0&&g_locationIndex<int(kLocations.size())){auto& e=kLocations[g_locationIndex];TeleportPedTo(FindPlayerPed(),CVector(e.x,e.y,e.z),true,&e.zone);}});
        add("Surprise me","Travel",[]{if(!kLocations.empty()){g_locationIndex=rand()%kLocations.size();auto& e=kLocations[g_locationIndex];TeleportPedTo(FindPlayerPed(),CVector(e.x,e.y,e.z),true,&e.zone);}});
        for(const auto& location:kLocations){const auto saved=location;add(location.name.c_str(),"Travel",[saved]{TeleportPedTo(FindPlayerPed(),CVector(saved.x,saved.y,saved.z),true,&saved.zone);});}
        add("Pause / Resume Animation","Animation",[]{AnimationPlayer::Pause(!AnimationPlayer::Paused());});
        add("Restart Animation","Animation",[]{AnimationPlayer::Restart();});
        add("Hold Last Frame","Animation",[]{AnimationPlayer::Seek(1);});
        add("Play Animation","Animation",[]{AnimationPlayer::PlaySingle(BuildStepFromFields(),g_animLoop);});
        add("Play Sequence","Animation",[]{if(!g_animSequence.empty())AnimationPlayer::PlaySequence(g_animSequence,g_seqLoop);});
        add("UNLEASH CHAOS","Fun",[]{CCheat::RiotCheat();CCheat::GangsCheat();CCheat::MayhemCheat();CCheat::EverybodyAttacksPlayerCheat();});
        add("PARTY MODE","Fun",[]{CCheat::BeachPartyCheat();CCheat::FunhouseCheat();CCheat::VillagePeopleCheat();CCheat::LovefistCheat();});
        add("APOCALYPSE","Fun",[]{CCheat::StormCheat();CCheat::MidnightCheat();CCheat::CountrysideInvasionCheat();});
        add("Stop","Animation",[]{AnimationPlayer::Stop();});
        add("Add To Sequence","Animation",[]{if(g_animSequence.size()<256)g_animSequence.push_back(BuildStepFromFields());});
        add("Clear Sequence","Animation",[]{g_animSequence.clear();});
        add("Loop","Animation",[]{g_animLoop=!g_animLoop;});
        add("Loop Sequence","Animation",[]{g_seqLoop=!g_seqLoop;});
        add("Lock X","Animation",[]{g_animLockX=!g_animLockX;});
        add("Lock Y","Animation",[]{g_animLockY=!g_animLockY;});
        add("Hold Last Frame","Animation",[]{g_animHoldLastFrame=!g_animHoldLastFrame;});
        add("Random","Character",[]{const auto& peds=PedList::Get();if(!peds.empty()){g_skinId=peds[rand()%peds.size()].id;SetPlayerSkin(FindPlayerPed(),g_skinId);}});
        add("Use Current Position","Travel",[]{auto pos=FindPlayerPed()->GetPosition();g_teleport[0]=pos.x;g_teleport[1]=pos.y;g_teleport[2]=pos.z;});

        add("Weapon Set 1","Game actions",[]{CCheat::WeaponCheat1();});
        add("Weapon Set 2","Game actions",[]{CCheat::WeaponCheat2();});
        add("Weapon Set 3","Game actions",[]{CCheat::WeaponCheat3();});
        add("Tank","Game actions",[]{CCheat::TankCheat();});
        add("Hearse","Game actions",[]{CCheat::HearseCheat();});
        add("Trashmaster","Game actions",[]{CCheat::TrashmasterCheat();});
        add("Dozer","Game actions",[]{CCheat::DozerCheat();});
        add("Quad","Game actions",[]{CCheat::QuadCheat();});
        add("Golfcart","Game actions",[]{CCheat::GolfcartCheat();});
        add("Monster Truck","Game actions",[]{CCheat::MonsterTruckCheat();});
        add("Stunt Plane","Game actions",[]{CCheat::StuntPlaneCheat();});
        add("Parachute","Game actions",[]{CCheat::ParachuteCheat();});
        add("Jetpack","Game actions",[]{CCheat::JetpackCheat();});
        add("Hydra (Apache)","Game actions",[]{CCheat::ApacheCheat();});
        add("Flyboy","Game actions",[]{CCheat::FlyboyCheat();});
        add("Tanker Rig","Game actions",[]{CCheat::TankerCheat();});
        add("Vortex Hovercraft","Game actions",[]{CCheat::VortexCheat();});
        add("Stock Car 1","Game actions",[]{CCheat::StockCarCheat();});
        add("Stock Car 2","Game actions",[]{CCheat::StockCar2Cheat();});
        add("Stock Car 3","Game actions",[]{CCheat::StockCar3Cheat();});
        add("Stock Car 4","Game actions",[]{CCheat::StockCar4Cheat();});
        add("All Cars Great","Game actions",[]{CCheat::AllCarsAreGreatCheat();});
        add("All Cars Cheap","Game actions",[]{CCheat::AllCarsAreShitCheat();});
        add("Black Traffic","Game actions",[]{CCheat::BlackCarsCheat();});
        add("Pink Traffic","Game actions",[]{CCheat::PinkCarsCheat();});
        add("Blow Up Nearby Cars","Game actions",[]{CCheat::BlowUpCarsCheat();});
        add("Aggressive Drivers","Game actions",[]{CCheat::DrivebyCheat();});
        add("Sunny","Game actions",[]{CCheat::SunnyWeatherCheat();});
        add("Extra Sunny","Game actions",[]{CCheat::ExtraSunnyWeatherCheat();});
        add("Cloudy","Game actions",[]{CCheat::CloudyWeatherCheat();});
        add("Rainy","Game actions",[]{CCheat::RainyWeatherCheat();});
        add("Foggy","Game actions",[]{CCheat::FoggyWeatherCheat();});
        add("Sandstorm","Game actions",[]{CCheat::SandstormCheat();});
        add("Thunderstorm","Game actions",[]{CCheat::StormCheat();});
        add("Midnight","Game actions",[]{CCheat::MidnightCheat();});
        add("Dusk","Game actions",[]{CCheat::DuskCheat();});
        add("Fast Time","Game actions",[]{CCheat::FastTimeCheat();});
        add("Slow Time","Game actions",[]{CCheat::SlowTimeCheat();});
        add("Riot","Game actions",[]{CCheat::RiotCheat();});
        add("Gang War","Game actions",[]{CCheat::GangsCheat();});
        add("Gangland","Game actions",[]{CCheat::GangLandCheat();});
        add("Mayhem","Game actions",[]{CCheat::MayhemCheat();});
        add("Max Wanted","Game actions",[]{CCheat::WantedCheat();});
        add("Everybody Attacks Player","Game actions",[]{CCheat::EverybodyAttacksPlayerCheat();});
        add("Countryside Invasion","Game actions",[]{CCheat::CountrysideInvasionCheat();});
        add("Beach Party","Game actions",[]{CCheat::BeachPartyCheat();});
        add("Funhouse","Game actions",[]{CCheat::FunhouseCheat();});
        add("Village People","Game actions",[]{CCheat::VillagePeopleCheat();});
        add("Lovefist","Game actions",[]{CCheat::LovefistCheat();});
        add("Elvis Lives","Game actions",[]{CCheat::ElvisLivesCheat();});
        add("Love Conquers All","Game actions",[]{CCheat::LoveConquersAllCheat();});
        add("Ninja Outfit","Game actions",[]{CCheat::NinjaCheat();});
        for(const char* label:{"Repair","Flip Upright","Save Location","Recall Location","Save Loadout","Recall Loadout","Save Ride","Recall Ride","Random Ride","Sunshine","Stormy Night","Beach Day","Go Back"}){std::string name=label;add(label,"Quick tools",[name]{QuickAction(name.c_str());});}
        ActionBindings::Load();
    }

    void ApplyPersistentEffects()
    {
        CPlayerPed *ped = FindPlayerPed();
        if (!ped) return;
        ProcessSkin(ped);Wardrobe::Process(ped);

        if (g_infiniteHealth || g_godMode) ped->m_fHealth = 100.f;
        if (g_infiniteArmour || g_godMode) ped->m_fArmour = 100.f;
        if (g_neverWanted) CCheat::NotWantedCheat();
        static CPlayerInfo* proofOwner=nullptr;static bool originalProof=false;
        if(auto* info=ped->GetPlayerInfoForThisPlayerPed()){
            if(g_godMode){if(proofOwner!=info){proofOwner=info;originalProof=info->m_bFireProof;}info->m_bFireProof=true;}
            else if(proofOwner==info){info->m_bFireProof=originalProof;proofOwner=nullptr;}
        }
        if (g_freezePlayer)
        {
            ped->GetPosition() = g_frozenPos;
            ped->m_vecMoveSpeed = CVector(0.f, 0.f, 0.f);
        }
        if (g_infiniteAmmo)
        {
            if (CWeapon *weapon = ped->GetWeapon())
            {
                weapon->m_nAmmoInClip = 9999;
                weapon->m_nAmmoTotal = 9999;
            }
        }
    }

    void Draw()
    {
        InitActions();
        static int active=0;
        bool close=false;
        if(!TrainerUI::Begin(active,close))return;
        if(close)Render::CloseMenu();
        switch(active) {
            case 0:DrawPlayerTab();break;case 1:DrawWeaponsTab();break;
            case 2:DrawVehiclesTab();break;case 3:DrawWorldTab();break;
            case 4:DrawTeleportTab();break;case 5:DrawAnimationsTab();break;
            case 6:DrawSkinTab();break;case 7:DrawExtrasTab();break;case 8:ActionBindings::Draw();break;case 9:Branding::Info();break;
        }
        TrainerUI::End();
        ActionBindings::Prompt();
    }
}

// The phone's Trainer app: the actions the menu registers, and the lists it
// picks a vehicle, a weapon or a character from. Game thread only.
extern "C" __declspec(dllexport) int __cdecl ValkyrieTrainerActionCount() { Menu::InitActions(); return ActionBindings::Count(); }
extern "C" __declspec(dllexport) const char* __cdecl ValkyrieTrainerActionLabel(int i) { return ActionBindings::Label(i); }
extern "C" __declspec(dllexport) const char* __cdecl ValkyrieTrainerActionGroup(int i) { return ActionBindings::Group(i); }
extern "C" __declspec(dllexport) int __cdecl ValkyrieTrainerActionState(int i) { return ActionBindings::State(i); }
extern "C" __declspec(dllexport) void __cdecl ValkyrieTrainerActionRun(int i) { ActionBindings::Run(i); }
// kind: 0 vehicles, 1 weapons, 2 characters.
extern "C" __declspec(dllexport) int __cdecl ValkyrieTrainerListCount(int kind) {
    if (kind == 0) return static_cast<int>(VehicleList::Get().size());
    if (kind == 1) return static_cast<int>(std::size(kWeapons));
    if (kind == 2) return static_cast<int>(PedList::Get().size());
    return 0;
}
extern "C" __declspec(dllexport) const char* __cdecl ValkyrieTrainerListName(int kind, int i) {
    if (i < 0 || i >= ValkyrieTrainerListCount(kind)) return "";
    if (kind == 0) return VehicleList::Get()[i].name.c_str();
    if (kind == 1) return kWeapons[i].name;
    return PedList::Get()[i].name.c_str();
}
extern "C" __declspec(dllexport) void __cdecl ValkyrieTrainerListPick(int kind, int i) {
    CPlayerPed* ped = FindPlayerPed();
    if (!ped || i < 0 || i >= ValkyrieTrainerListCount(kind)) return;
    if (kind == 0) SpawnVehicleForPlayer(VehicleList::Get()[i].id);
    else if (kind == 1) ped->GiveWeapon(kWeapons[i].id, std::clamp(g_ammo, 0, 999999), false);
    else SetPlayerSkin(ped, PedList::Get()[i].id);
}
