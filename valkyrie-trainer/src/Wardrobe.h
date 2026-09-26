#pragma once
#include <CClothes.h>
#include <CPedClothesDesc.h>
#include <CPlayerData.h>
#include <CText.h>
#include <fstream>
#include <array>
#include "WardrobeData.h"
#include "TrainerUI.h"
#include "AnimationPlayer.h"
#include "ModelPreview.h"
namespace Wardrobe {
inline int pending=-1,removeSlot=-1;
inline bool restore=false,hasSaved=false;
inline std::array<unsigned char,sizeof(CPedClothesDesc)> saved{};
inline CPedClothesDesc* savedOwner=nullptr;
inline std::string status;
inline const std::vector<WardrobeData::Item>& Items(){
    static auto items=[](){char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);std::string path=exe;path.resize(path.find_last_of("\\/")+1);std::ifstream file(path+"data\\shopping.dat");return WardrobeData::Read(file);}();return items;
}
inline void Process(CPlayerPed* player){
    if(pending<0&&removeSlot<0&&!restore)return;
    const int item=pending,slot=removeSlot;const bool undo=restore;pending=removeSlot=-1;restore=false;
    if(!player||player->m_nModelIndex!=0||player->bInVehicle||player->m_fHealth<=0||!player->m_pRwClump||!player->m_pPlayerData||!player->m_pPlayerData->m_pPedClothesDesc){status="Return to CJ and stand on foot to change clothes.";return;}
    auto* clothes=player->m_pPlayerData->m_pPedClothesDesc;
    AnimationPlayer::Stop();AnimationPlayer::Update();ModelPreview::BeforeSkinChange();
    if(undo){if(!hasSaved||savedOwner!=clothes){status="No outfit to restore in this session.";return;}memcpy(clothes,saved.data(),saved.size());hasSaved=false;}
    else {
        if(!hasSaved||savedOwner!=clothes){memcpy(saved.data(),clothes,saved.size());savedOwner=clothes;hasSaved=true;}
        if(item>=0&&item<(int)Items().size()){const auto& choice=Items()[item];clothes->SetTextureAndModel(choice.texture.c_str(),choice.model.c_str(),choice.slot);}
        else if(slot>=13&&slot<=16)clothes->SetTextureAndModel(0u,0u,slot);
        else {status="Choose an item first.";return;}
    }
    CClothes::RebuildPlayer(player,false);status="Outfit updated.";
}
inline void Draw(CPlayerPed* player){
    if(!ImGui::CollapsingHeader("CJ clothes"))return;
    bool available=player&&player->m_nModelIndex==0&&!player->bInVehicle;
    if(!available){TrainerUI::StatusLine("Return to CJ and stand on foot to change clothes.");return;}
    static int category=0,selected=-1;static char filter[80]{};
    const char* names[]={"Tops","Trousers","Shoes","Necklaces","Watches","Glasses","Hats","Outfits"};const int slots[]={0,2,3,13,14,15,16,17};
    ImGui::TextUnformatted("Category");ImGui::SetNextItemWidth(180);
    if(ImGui::Combo("##clothes-category",&category,names,8))selected=-1;
    ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##clothes-search","Search clothes...",filter,sizeof filter))selected=-1;
    std::string query=filter;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return (char)tolower(c);});
    if(ImGui::BeginListBox("##clothes-list",{-FLT_MIN,190})){
        int matches=0;for(int i=0;i<(int)Items().size();++i){const auto& item=Items()[i];if(item.slot!=slots[category])continue;
            const char* translated=TheText.Get(item.label.c_str());std::string label=translated&&*translated?translated:item.texture;
            std::string search=label+" "+item.texture;std::transform(search.begin(),search.end(),search.begin(),[](unsigned char c){return (char)tolower(c);});if(!query.empty()&&search.find(query)==std::string::npos)continue;
            ++matches;ImGui::PushID(i);if(ImGui::Selectable(label.c_str(),selected==i))selected=i;ImGui::PopID();}
        if(!matches)ImGui::TextDisabled("No matching clothes.");ImGui::EndListBox();
    }
    ImGui::BeginDisabled(selected<0);if(TrainerUI::PrimaryButton("Wear selected")){pending=selected;status="Applying outfit...";}ImGui::EndDisabled();
    if(category>=3&&category<=6){TrainerUI::Inline(130);if(TrainerUI::Button("Remove item"))removeSlot=slots[category];}
    TrainerUI::Inline(160);ImGui::BeginDisabled(!hasSaved);if(TrainerUI::Button("Restore outfit"))restore=true;ImGui::EndDisabled();
    TrainerUI::StatusLine(status.c_str());
}
}
