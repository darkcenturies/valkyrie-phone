#include "TrainerUI.h"
#include "Branding.h"
#include "AssetBundle.h"
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace TrainerUI {
namespace {
ImFont* titleFont=nullptr;
ImFont* boldFont=nullptr;
void(*actionContext)(const char*)=nullptr;
ImTextureID(*navIcons)(int)=nullptr;
const ImVec4 cream{.94f,.91f,.84f,1},muted{.57f,.57f,.59f,1},amber{.97f,.66f,.16f,1};
void DisplayText(ImVec2 p,const char* text,float size,ImU32 color){
    ImGui::PushFont(titleFont,size);
    auto* d=ImGui::GetWindowDrawList();
    for(const auto offset: {ImVec2(-2,0),ImVec2(2,0),ImVec2(0,-2),ImVec2(0,2),ImVec2(3,3)})
        d->AddText({p.x+offset.x,p.y+offset.y},IM_COL32(0,0,0,240),text);
    d->AddText(p,color,text);ImGui::PopFont();
}
}
void SetNavIcons(ImTextureID(*callback)(int)){navIcons=callback;}
void SetActionContext(void(*callback)(const char*)){actionContext=callback;}
ImFont* TitleFont(){return titleFont;}
void Setup(const std::string& gameDirectory){
    Branding::SetDirectory(gameDirectory+"\\valkyrie-trainer-assets");
    auto& io=ImGui::GetIO();
    char windows[MAX_PATH]{};GetWindowsDirectoryA(windows,MAX_PATH);
    const std::string body=std::string(windows)+"\\Fonts\\trebuc.ttf";
    if(GetFileAttributesA(body.c_str())!=INVALID_FILE_ATTRIBUTES)
        io.FontDefault=io.Fonts->AddFontFromFileTTF(body.c_str(),17);
    if(!io.FontDefault){ImFontConfig cfg;cfg.SizePixels=17;io.FontDefault=io.Fonts->AddFontDefault(&cfg);}
    const auto bold=std::string(windows)+"\\Fonts\\trebucbd.ttf";
    if(GetFileAttributesA(bold.c_str())!=INVALID_FILE_ATTRIBUTES)boldFont=io.Fonts->AddFontFromFileTTF(bold.c_str(),17);
    if(!boldFont)boldFont=io.FontDefault;
    const auto display=gameDirectory+"\\pricedown.ttf";
    if(auto font=AssetBundle::Get(101)){
        ImFontConfig cfg;cfg.FontDataOwnedByAtlas=false;
        titleFont=io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(font.data),static_cast<int>(font.size),36,&cfg);
    }else if(GetFileAttributesA(display.c_str())!=INVALID_FILE_ATTRIBUTES)
        titleFont=io.Fonts->AddFontFromFileTTF(display.c_str(),36);
    if(!titleFont)titleFont=io.FontDefault;
    auto& s=ImGui::GetStyle();ImGui::StyleColorsDark();
    auto* c=s.Colors;
    c[ImGuiCol_Text]=cream;c[ImGuiCol_TextDisabled]=muted;
    c[ImGuiCol_WindowBg]={.045f,.045f,.052f,.98f};c[ImGuiCol_ChildBg]={0,0,0,0};
    c[ImGuiCol_PopupBg]={.08f,.08f,.09f,1};c[ImGuiCol_Border]={1,1,1,.08f};
    c[ImGuiCol_FrameBg]={.095f,.093f,.106f,1};c[ImGuiCol_FrameBgHovered]={.14f,.135f,.15f,1};c[ImGuiCol_FrameBgActive]={.17f,.16f,.18f,1};
    c[ImGuiCol_Button]={.14f,.135f,.15f,1};c[ImGuiCol_ButtonHovered]={.22f,.20f,.18f,1};c[ImGuiCol_ButtonActive]={.29f,.24f,.17f,1};
    c[ImGuiCol_Header]={0,0,0,0};c[ImGuiCol_HeaderHovered]={1,1,1,.075f};c[ImGuiCol_HeaderActive]={.97f,.66f,.16f,.23f};
    c[ImGuiCol_CheckMark]=amber;c[ImGuiCol_SliderGrab]=amber;c[ImGuiCol_SliderGrabActive]=cream;
    c[ImGuiCol_CheckboxSelectedBg]={.97f,.66f,.16f,.2f};c[ImGuiCol_InputTextCursor]=amber;
    c[ImGuiCol_ScrollbarBg]={0,0,0,0};c[ImGuiCol_ScrollbarGrab]={.30f,.28f,.30f,.7f};c[ImGuiCol_ScrollbarGrabHovered]=muted;c[ImGuiCol_ScrollbarGrabActive]=amber;
    c[ImGuiCol_Separator]={1,1,1,.075f};c[ImGuiCol_SeparatorHovered]=amber;c[ImGuiCol_SeparatorActive]=amber;
    c[ImGuiCol_ResizeGrip]={1,1,1,.08f};c[ImGuiCol_ResizeGripHovered]=amber;c[ImGuiCol_ResizeGripActive]=amber;
    c[ImGuiCol_TextSelectedBg]={.97f,.66f,.16f,.25f};c[ImGuiCol_NavHighlight]=amber;
    s.WindowRounding=0;s.ChildRounding=0;s.FrameRounding=0;s.PopupRounding=0;
    s.ScrollbarRounding=0;s.GrabRounding=0;s.TabRounding=0;
    c[ImGuiCol_FrameBg]={.13f,.13f,.14f,1};c[ImGuiCol_FrameBgHovered]={.18f,.17f,.16f,1};c[ImGuiCol_FrameBgActive]={.22f,.20f,.16f,1};
    c[ImGuiCol_Button]={.17f,.16f,.15f,1};c[ImGuiCol_ButtonHovered]={.26f,.23f,.18f,1};c[ImGuiCol_ButtonActive]={.33f,.27f,.17f,1};
    s.WindowBorderSize=0;s.FrameBorderSize=0;s.ChildBorderSize=0;
    s.WindowPadding={26,22};s.FramePadding={10,6};s.ItemSpacing={14,9};s.ItemInnerSpacing={9,7};
    s.ScrollbarSize=7;s.GrabMinSize=12;s.CellPadding={8,5};
}
void Heading(const char* text,float size){
    auto p=ImGui::GetCursorScreenPos();DisplayText(p,text,size,IM_COL32(239,232,213,255));
    ImGui::PushFont(titleFont,size);auto t=ImGui::CalcTextSize(text);ImGui::PopFont();ImGui::Dummy(t);
    if(size==26 || size==29){float end=p.x+ImGui::GetContentRegionAvail().x;
        if(end>p.x+t.x+34)ImGui::GetWindowDrawList()->AddLine({p.x+t.x+16,p.y+t.y*.60f},{end-6,p.y+t.y*.60f-1},IM_COL32(213,202,177,27),1);
    }
}
void Emphasis(const char* text){ImGui::PushFont(boldFont);ImGui::TextUnformatted(text);ImGui::PopFont();}
void StatusLine(const char* text,int lines){
    const auto p=ImGui::GetCursorScreenPos();const ImVec2 size{ImGui::GetContentRegionAvail().x,ImGui::GetTextLineHeight()*lines+4};
    ImGui::Dummy(size);auto* draw=ImGui::GetWindowDrawList();draw->PushClipRect(p,{p.x+size.x,p.y+size.y},true);
    if(text&&*text)draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),p,ImGui::GetColorU32(ImGuiCol_TextDisabled),text,nullptr,size.x);
    draw->PopClipRect();if(text&&*text&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",text);
}
void Ink(ImDrawList* d,ImVec2 p,ImVec2 size,ImU32 color){
    const ImVec2 poly[]={{p.x+3,p.y+3},{p.x+size.x-1,p.y},{p.x+size.x-5,p.y+size.y-3},{p.x,p.y+size.y}};
    d->AddConvexPolyFilled(poly,4,color);
}
bool Action(const char* label,ImVec2 size,bool primary){
    const auto text=ImGui::CalcTextSize(label);if(size.x<=0)size.x=text.x+30;if(size.y<=0)size.y=36;
    size.x=std::min(size.x,std::max(1.f,ImGui::GetContentRegionAvail().x));
    auto p=ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0,0,0,0));ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0,0,0,0));
    bool hit=ImGui::Button("##action",size);ImGui::PopStyleColor(3);auto* d=ImGui::GetWindowDrawList();
    bool hover=ImGui::IsItemHovered(),active=ImGui::IsItemActive();
    ImU32 background=primary?(active?IM_COL32(197,124,17,255):hover?IM_COL32(255,187,61,255):IM_COL32(230,156,31,255)):
        (active?IM_COL32(77,69,54,255):hover?IM_COL32(64,61,55,255):IM_COL32(39,39,41,255));
    Ink(d,p,size,ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(background)));
    const auto color=ImGui::GetColorU32(primary?ImVec4(.06f,.055f,.045f,1):cream);
    d->PushClipRect(p,{p.x+size.x,p.y+size.y},true);
    d->AddText({p.x+(size.x-text.x)*.5f,p.y+(size.y-text.y)*.5f},color,label);d->PopClipRect();
    if(hover)ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);if(actionContext)actionContext(label);ImGui::PopID();return hit;
}
bool Button(const char* label,ImVec2 size){return Action(label,size,false);}
bool PrimaryButton(const char* label,ImVec2 size){return Action(label,size,true);}
bool SmallButton(const char* label){return Action(label,{0,25},false);}
void Inline(float nextWidth){
    float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
    if(right-ImGui::GetItemRectMax().x>nextWidth+ImGui::GetStyle().ItemSpacing.x)ImGui::SameLine();
}
bool Toggle(const char* label,bool* value){
    bool changed=ImGui::Checkbox(label,value);
    if(actionContext)actionContext(label);
    return changed;
}
bool NumberField(const char* label,int* value,float width,int step){
    ImGui::TextUnformatted(label);
    ImGui::PushID(label);ImGui::SetNextItemWidth(std::min(width,ImGui::GetContentRegionAvail().x));
    bool changed=ImGui::InputInt("##value",value,step,step?step*10:0);
    ImGui::PopID();return changed;
}
void Separator(){ImGui::Spacing();ImGui::Separator();ImGui::Spacing();}
bool WeaponTile(ImTextureID texture,const char* name,bool selected){
    auto p=ImGui::GetCursorScreenPos();float width=std::min(80.f,ImGui::GetContentRegionAvail().x);
    bool clicked=ImGui::InvisibleButton(name,{width,74});bool hover=ImGui::IsItemHovered();auto* d=ImGui::GetWindowDrawList();
    if(texture)d->AddImage(texture,{p.x+8,p.y},{p.x+width-8,p.y+54},{0,0},{1,1},selected||hover?IM_COL32_WHITE:IM_COL32(225,222,214,225));
    else {d->PushClipRect(p,{p.x+width,p.y+80},true);DisplayText({p.x+3,p.y+20},name,19,IM_COL32(232,221,197,255));d->PopClipRect();}
    if(selected)d->AddLine({p.x+width*.25f,p.y+64},{p.x+width*.75f,p.y+62},IM_COL32(245,163,26,255),3);
    if(hover)ImGui::SetTooltip("%s",name);return clicked;
}
WeaponAction DrawWeaponPanel(const WeaponChoice* choices,int count,int& selected,int& ammo,bool& infinite){
    if(count<=0)return WeaponAction::None;
    selected=std::clamp(selected,0,count-1);
    WeaponAction result=WeaponAction::None;
    static char search[80]{};
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##weapon-search","Search weapons...",search,sizeof search);
    std::string query=search;std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c){return (char)std::tolower(c);});
    float listHeight=std::clamp(ImGui::GetContentRegionAvail().y-110.f,175.f,355.f);
    if(ImGui::BeginTable("##weapon-browser",2,ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("Choose",ImGuiTableColumnFlags_WidthStretch,.55f);
        ImGui::TableSetupColumn("Apply",ImGuiTableColumnFlags_WidthStretch,.45f);
        ImGui::TableNextColumn();ImGui::TextDisabled("Choose a weapon");
        ImGui::BeginChild("##weapons",{0,listHeight});
        int matches=0;
        for(int i=0;i<count;++i){
            std::string name=choices[i].name;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return (char)std::tolower(c);});
            if(!query.empty()&&name.find(query)==std::string::npos)continue;
            ++matches;ImGui::PushID(i);auto p=ImGui::GetCursorScreenPos();float w=ImGui::GetContentRegionAvail().x;
            if(ImGui::Selectable("##choose",selected==i,0,{w,36}))selected=i;
            if(selected==i&&ImGui::IsWindowAppearing())ImGui::SetScrollHereY(.5f);
            auto* draw=ImGui::GetWindowDrawList();
            if(choices[i].icon)draw->AddImage(choices[i].icon,{p.x+5,p.y+4},{p.x+33,p.y+32},{0,0},{1,1},IM_COL32(225,219,204,225));
            draw->PushClipRect(p,{p.x+w-19,p.y+36},true);draw->AddText({p.x+44,p.y+9},ImGui::GetColorU32(selected==i?amber:cream),choices[i].name);draw->PopClipRect();
            if(selected==i)draw->AddText({p.x+w-17,p.y+9},ImGui::GetColorU32(amber),"<");
            ImGui::PopID();
        }
        if(!matches)ImGui::TextWrapped("No matching weapons. Try another name.");
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Selected weapon");
        Heading(choices[selected].name,26);ImGui::Spacing();
        if(choices[selected].usesAmmo){
            NumberField("Ammunition",&ammo,185,30);ammo=std::clamp(ammo,1,999999);
            ImGui::PushID("ammo-presets");
            if(SmallButton("30"))ammo=30;Inline(55);if(SmallButton("120"))ammo=120;Inline(60);if(SmallButton("999"))ammo=999;
            ImGui::PopID();ImGui::Spacing();
        }else ImGui::TextDisabled("No ammunition needed");
        if(PrimaryButton("Give Weapon"))result=WeaponAction::Give;
        ImGui::Spacing();Toggle("Infinite Ammo",&infinite);
        ImGui::EndTable();
    }
    Separator();
    if(ImGui::CollapsingHeader("Loadouts & inventory")){
        if(Button("Weapon Set 1"))result=WeaponAction::Set1;Inline(150);
        if(Button("Weapon Set 2"))result=WeaponAction::Set2;Inline(150);
        if(Button("Weapon Set 3"))result=WeaponAction::Set3;
        if(Button("Give All Weapons"))result=WeaponAction::All;Inline(190);
        if(Button("Remove All Weapons"))result=WeaponAction::Remove;
    }
    return result;
}
bool ModelRow(const char* name,int id,bool selected){
    ImGui::PushID(id);auto p=ImGui::GetCursorScreenPos();float w=ImGui::GetContentRegionAvail().x;
    bool click=ImGui::InvisibleButton("##model",{w,36});bool hover=ImGui::IsItemHovered();auto* d=ImGui::GetWindowDrawList();

    if(selected)d->AddRectFilled({p.x,p.y+9},{p.x+3,p.y+27},IM_COL32(247,168,41,255),0);
    std::string label=name;std::replace(label.begin(),label.end(),'_',' ');
    if(!label.empty())label[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    char number[20];snprintf(number,sizeof number,"%d",id);
    float numberWidth=ImGui::CalcTextSize(number).x;
    d->PushClipRect({p.x+10,p.y},{p.x+w-4,p.y+36},true);
    d->AddText({p.x+12,p.y+9},selected?IM_COL32(247,190,86,255):hover?IM_COL32(255,244,218,255):IM_COL32(221,217,208,255),label.c_str());d->PopClipRect();
    if(hover)ImGui::SetTooltip("%s  /  %d",name,id);
    ImGui::PopID();return click;
}
PlayerAction DrawPlayerPanel(PlayerPanel& player){
    PlayerAction action=PlayerAction::None;
    const int columns=ImGui::GetContentRegionAvail().x>=680?2:1;
    if(ImGui::BeginTable("##player-options",columns,ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextColumn();Heading("Survival");
        Toggle("Infinite Health",&player.health);Toggle("Infinite Armour",&player.armour);
        Toggle("Never Wanted",&player.wanted);Toggle("God Mode",&player.god);Toggle("Freeze Player",&player.freeze);
        ImGui::TableNextColumn();Heading("A little help");
        if(PrimaryButton("Full Health / Armour / Money"))action=PlayerAction::Restore;
        if(Button("Health Only"))action=PlayerAction::Heal;
        Inline(140);if(Button("Mega Jump"))action=PlayerAction::Jump;
        if(Button("Launch Vehicle"))action=PlayerAction::Launch;
        if(player.hasMoney){
            ImGui::SetNextItemWidth(std::min(210.f,ImGui::GetContentRegionAvail().x-70));NumberField("Money",&player.money,210,1000);
            if(Button("+$1,000"))player.money+=1000;Inline(100);
            if(Button("+$10,000"))player.money+=10000;Inline(110);
            if(Button("+$100,000"))player.money+=100000;
        }
        ImGui::TableNextColumn();Separator();Heading("Wanted level");
        if(Button("Wanted +"))action=PlayerAction::WantedUp;Inline(120);
        if(Button("Wanted -"))action=PlayerAction::WantedDown;
        if(PrimaryButton("Clear Wanted"))action=PlayerAction::ClearWanted;
        ImGui::TableNextColumn();Separator();if(ImGui::CollapsingHeader("Body & skills")){
        if(Button("Fat"))action=PlayerAction::Fat;Inline(90);
        if(Button("Skinny"))action=PlayerAction::Skinny;Inline(90);
        if(Button("Muscle"))action=PlayerAction::Muscle;
        if(Button("Max Stamina"))action=PlayerAction::Stamina;Inline(130);
        if(Button("Adrenaline"))action=PlayerAction::Adrenaline;
        if(Button("Ninja Outfit"))action=PlayerAction::Ninja;
        if(Button("Max Weapon Skills"))action=PlayerAction::WeaponSkills;
        if(Button("Max Vehicle Skills"))action=PlayerAction::VehicleSkills;
        }
        ImGui::EndTable();
    }
    Separator();
    if(Button("Reset All Cheats"))action=PlayerAction::Reset;
    Inline(100);if(Button("Suicide"))action=PlayerAction::Suicide;
    return action;
}
AnimationAction DrawAnimationPanel(AnimationPanel& state){
    AnimationAction action=AnimationAction::None;
    const char* modes[]={"Play once","Loop full clip","Hold last frame"};
    ImGui::SetNextItemWidth(210);ImGui::Combo("On Play",&state.mode,modes,3);
    if(PrimaryButton("Play Animation"))action=AnimationAction::Play;
    Inline(95);ImGui::BeginDisabled(!state.active);
    if(Button(state.paused?"Resume":"Pause"))action=AnimationAction::Pause;
    Inline(95);if(Button("Restart"))action=AnimationAction::Restart;
    Inline(80);if(Button("Stop"))action=AnimationAction::Stop;
    ImGui::EndDisabled();
    ImGui::Text("Position   %.2f / %.2f s",state.position,state.length);
    ImGui::BeginDisabled(!state.active||state.length<=0);
    state.seek=state.length>0?state.position/state.length:0;
    ImGui::SetNextItemWidth(-1);
    if(ImGui::SliderFloat("##animation-position",&state.seek,0,1,""))action=AnimationAction::Seek;
    if(SmallButton("First frame")){state.seek=0;action=AnimationAction::Seek;}
    Inline(110);if(SmallButton("Last frame")){state.seek=1;action=AnimationAction::Seek;}
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(210);
    if(ImGui::SliderFloat("Playback speed",&state.speed,.1f,3.f,"%.2fx"))action=AnimationAction::Speed;
    return action;
}
bool Begin(int& active,bool& close){
    const ImVec2 screen=ImGui::GetIO().DisplaySize;
    ImVec2 target{std::min(1140.f,screen.x-32),std::min(790.f,screen.y-32)};
    ImGui::SetNextWindowSize(target,ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({screen.x*.5f,screen.y*.5f},ImGuiCond_FirstUseEver,{.5f,.5f});
    ImGui::SetNextWindowSizeConstraints({std::min(800.f,target.x),std::min(520.f,target.y)},{screen.x-16,screen.y-16});
    ImGui::GetBackgroundDrawList()->AddRectFilled({0,0},screen,IM_COL32(0,0,0,95));
    if(!ImGui::Begin("Valkyrie Trainer",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)){
        ImGui::End();return false;
    }
    auto position=ImGui::GetWindowPos(),windowSize=ImGui::GetWindowSize();
    ImGui::SetWindowPos({std::clamp(position.x,0.f,std::max(0.f,screen.x-windowSize.x)),std::clamp(position.y,0.f,std::max(0.f,screen.y-windowSize.y))});
    auto* d=ImGui::GetWindowDrawList();const auto pos=ImGui::GetWindowPos(),sz=ImGui::GetWindowSize();
    const auto top=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
    // Loose title and layered ink silhouettes, like a GTA pause screen.
    const ImVec2 inkPanel[]={{pos.x+9,pos.y+115},{pos.x+sz.x-8,pos.y+91},{pos.x+sz.x,pos.y+sz.y-20},{pos.x+22,pos.y+sz.y}};
    d->AddConvexPolyFilled(inkPanel,4,IM_COL32(7,7,9,215));
    const ImVec2 stripe[]={{pos.x+sz.x-132,pos.y+92},{pos.x+sz.x-105,pos.y+92},{pos.x+sz.x-36,pos.y+sz.y-24},{pos.x+sz.x-74,pos.y+sz.y-23}};
    d->AddConvexPolyFilled(stripe,4,IM_COL32(45,29,46,42));
    DisplayText({top.x,top.y-3},"Valkyrie",40,IM_COL32(241,233,215,255));
    DisplayText({top.x+3,top.y+33},"Trainer",20,IM_COL32(245,163,26,255));

    ImGui::InvisibleButton("##drag",{width-48,70});
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0)){
        auto delta=ImGui::GetIO().MouseDelta;
        ImGui::SetWindowPos({std::clamp(pos.x+delta.x,0.f,std::max(0.f,screen.x-sz.x)),std::clamp(pos.y+delta.y,0.f,std::max(0.f,screen.y-sz.y))});
    }
    ImGui::SetCursorScreenPos({top.x+width-36,top.y+4});
    if(ImGui::InvisibleButton("Close trainer",{34,34}))close=true;
    const bool hover=ImGui::IsItemHovered();
    if(hover)Ink(d,{top.x+width-36,top.y+4},{34,34},IM_COL32(245,163,26,100));
    d->AddLine({top.x+width-24,top.y+16},{top.x+width-14,top.y+26},IM_COL32(218,212,201,255),1.7f);
    d->AddLine({top.x+width-14,top.y+16},{top.x+width-24,top.y+26},IM_COL32(218,212,201,255),1.7f);
    d->AddLine({top.x,top.y+88},{top.x+width,top.y+86},IM_COL32(222,202,160,27),1);
    ImGui::SetCursorScreenPos({top.x,top.y+103});
    float bodyHeight=ImGui::GetContentRegionAvail().y-8;
    const float navWidth=sz.x<940?151.f:182.f;
    d->AddLine({top.x+navWidth+12,top.y+109},{top.x+navWidth+12,top.y+103+bodyHeight-9},IM_COL32(204,194,173,22),1);
    ImGui::BeginChild("##navigation",{navWidth,bodyHeight},0,ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy({0,8});
    const char* names[]={"Player","Weapons","Vehicles","World","Teleport","Animations","Skin","Extras","Controls","Info"};

    const float navRow=std::clamp((bodyHeight-30)/10-ImGui::GetStyle().ItemSpacing.y,22.f,44.f);
    for(int i=0;i<10;++i){
        ImGui::PushID(i);const auto p=ImGui::GetCursorScreenPos();
        if(ImGui::InvisibleButton(names[i],{navWidth,navRow}))active=i;
        bool selected=active==i,hovered=ImGui::IsItemHovered();
        if(selected)Ink(ImGui::GetWindowDrawList(),{p.x,p.y+4},{navWidth-12,navRow-1},IM_COL32(245,163,26,255));
        auto icon=navIcons&&i<10?navIcons(i):ImTextureID{};
        if(icon)ImGui::GetWindowDrawList()->AddImage(icon,{p.x+4,p.y+(navRow-24)*.5f},{p.x+28,p.y+(navRow+24)*.5f});
        DisplayText({p.x+34.f,p.y+(navRow-25)*.5f},names[i],sz.x<940?22.f:25.f,selected?IM_COL32(255,243,211,255):hovered?IM_COL32(247,168,41,255):IM_COL32(180,173,159,255));

        ImGui::PopID();
    }
    ImGui::EndChild();ImGui::SameLine(0,25);
    ImGui::BeginChild("##content",{0,bodyHeight});
    ImGui::PushItemWidth(std::min(340.f,ImGui::GetContentRegionAvail().x*.66f));
    Heading(names[active],36);
    ImGui::Dummy({0,8});
    return true;
}
void End(){
    ImGui::PopItemWidth();ImGui::EndChild();

    ImGui::End();
}
}
