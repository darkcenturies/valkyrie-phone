#define NOMINMAX
#include "Branding.h"
#include "AssetBundle.h"
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <shlwapi.h>
#pragma comment(lib,"shlwapi.lib")
#include "imgui.h"
#include "TrainerUI.h"

namespace Branding {
namespace {
struct Brand {
    const char* name;
    const char* file;
    const char* url;
    float maxWidth,maxHeight;
    IDirect3DTexture9* texture=nullptr;
    IDirect3DTexture9* muted=nullptr;
    unsigned width=0,height=0;
};
Brand brands[]={
    {"Valkyrie","valkyrie.png","https://ko-fi.com/valkyriesamp",22,22},
    {"SP-RP","sprp.png","https://sp-rp.com/",58,20},
    {"Project Eagle","project-eagle.png","https://www.projecteaglemod.games/",24,24},
    {"Stars & Stripes Multiplayer","ssmp.png","",27,24},
    {"Project Silent Hill","project-silent-hill.png","https://discord.gg/WRthyZNdWS",30,27}
};
IDirect3DDevice9* device=nullptr;
std::string directory;
bool loaded=false;
template<class T> void Release(T*& p){if(p){p->Release();p=nullptr;}}
void Clear(){for(auto& b:brands){Release(b.texture);Release(b.muted);}loaded=false;}
bool Upload(const std::vector<unsigned char>& pixels,unsigned width,unsigned height,IDirect3DTexture9*& texture){
    if(FAILED(device->CreateTexture(width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr)))return false;
    D3DLOCKED_RECT lock{};
    if(FAILED(texture->LockRect(0,&lock,nullptr,0))){Release(texture);return false;}
    for(unsigned y=0;y<height;++y)memcpy((char*)lock.pBits+y*lock.Pitch,pixels.data()+y*width*4,width*4);
    texture->UnlockRect(0);return true;
}
void Load(){
    if(loaded||!device||directory.empty())return;
    loaded=true;Gdiplus::GdiplusStartupInput startup;ULONG_PTR token=0;
    if(Gdiplus::GdiplusStartup(&token,&startup,nullptr)!=Gdiplus::Ok)return;
    for(auto& b:brands){
        auto resource=AssetBundle::Get(103+static_cast<unsigned>(&b-brands));
        auto releaseStream=[](IStream* s){if(s)s->Release();};
        std::unique_ptr<IStream,decltype(releaseStream)> stream(resource?SHCreateMemStream(resource.data,resource.size):nullptr,releaseStream);
        std::string path=directory+"\\"+b.file;
        int count=MultiByteToWideChar(CP_ACP,0,path.c_str(),-1,nullptr,0);
        std::vector<wchar_t> wide(count);MultiByteToWideChar(CP_ACP,0,path.c_str(),-1,wide.data(),count);
        std::unique_ptr<Gdiplus::Bitmap> image(stream?Gdiplus::Bitmap::FromStream(stream.get()):Gdiplus::Bitmap::FromFile(wide.data()));
        if(!image||image->GetLastStatus()!=Gdiplus::Ok)continue;
        auto& bitmap=*image;
        b.width=bitmap.GetWidth();b.height=bitmap.GetHeight();
        if(!b.width||!b.height||b.width>4096||b.height>4096)continue;
        Gdiplus::Rect rect(0,0,b.width,b.height);Gdiplus::BitmapData data{};
        if(bitmap.LockBits(&rect,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&data)!=Gdiplus::Ok)continue;
        std::vector<unsigned char> pixels(b.width*b.height*4);
        for(unsigned y=0;y<b.height;++y)memcpy(pixels.data()+y*b.width*4,(char*)data.Scan0+(int)y*data.Stride,b.width*4);
        bitmap.UnlockBits(&data);Upload(pixels,b.width,b.height,b.texture);
        for(size_t i=0;i<pixels.size();i+=4){auto gray=(pixels[i]*29+pixels[i+1]*150+pixels[i+2]*77)>>8;pixels[i]=pixels[i+1]=pixels[i+2]=(unsigned char)gray;}
        Upload(pixels,b.width,b.height,b.muted);
    }
    Gdiplus::GdiplusShutdown(token);
}

}
void SetDevice(IDirect3DDevice9* d){if(device!=d){Clear();device=d;}}
void SetDirectory(const std::string& value){if(directory!=value){Clear();directory=value;}}
void Info(){
    Load();static int page=0;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+std::min(660.f,ImGui::GetContentRegionAvail().x));
    const char* pages[]={"About","How to use","Credits & links"};
    for(int i=0;i<3;++i){if(i)TrainerUI::Inline(170);if((page==i?TrainerUI::PrimaryButton(pages[i]):TrainerUI::Button(pages[i])))page=i;}
    TrainerUI::Separator();
    if(page==0){
        TrainerUI::Heading("about the trainer",26);
        ImGui::TextWrapped("vehicles, characters, animations and the usual trainer stuff, in one menu. use it to set up a scene, test something, or just mess around in san andreas.");
        TrainerUI::Separator();
        ImGui::TextUnformatted("hi, i'm");ImGui::SameLine();TrainerUI::Emphasis("valkyrie");
        ImGui::TextWrapped("i make tools and mods for san andreas. this is one of them.");
        ImGui::Spacing();
        ImGui::TextWrapped("if you find them useful and want to support what i'm working on, you can do that on ko-fi. it helps support the time spent building, testing and fixing things.");
        ImGui::Spacing();
        if(TrainerUI::PrimaryButton("support on ko-fi"))ShellExecuteA(nullptr,"open",brands[0].url,nullptr,nullptr,SW_SHOWNORMAL);
        ImGui::TextDisabled("ko-fi.com/valkyriesamp");
    }else if(page==1){
        TrainerUI::Heading("start here",26);
        ImGui::TextWrapped("Open with Alt + Z. Choose a page on the left. Select an item, adjust any settings, then press its amber action button. Checked options stay enabled until you turn them off.");
        TrainerUI::Separator();
        const char* titles[]={"Player & autowalk","Weapons","Vehicles & characters","World & extras","Teleport","Animations & sequences","Keys & shortcuts","If something looks wrong"};
        const char* help[]={
            "Player contains health, armour, wanted level, money and abilities. Autowalk moves you forward on foot. Manual forward/back input takes priority. Opening the trainer pauses autowalk; Escape, focus loss, game pause, death or entering a vehicle turns it off. Restore Movement stops the trainer animation, autowalk and Freeze Player.",
            "Search or scroll the named weapon list. Selecting only chooses a weapon. Set Ammunition by typing, using +/- or a quantity preset, then press Give Weapon. Melee weapons and the parachute do not need ammunition. Loadouts & inventory contains the three game weapon sets and the give-all/remove-all actions. Weapons sharing a game slot may replace one another.",
            "Search the list, select a model, then press Spawn Vehicle or Set Skin beside its preview. Drag the model to turn it; scroll over it to zoom. Rotate spins it, and Reset restores the view. Choose by ID is available for direct entry. Selecting a vehicle again refreshes its preview and varies paint when its installed colour data supports it. Repair and Flip Upright affect your occupied vehicle. In Skin, Return to CJ restores Carl; CJ clothes lets you choose a category and item, then Wear selected. Remove item removes an accessory. Restore outfit returns to the outfit from before your first change in this session.",
            "World groups weather, time and optional world effects. Extras contains saved tools and fun combinations. Save Location stores your current place; Save Loadout stores your weapons and ammo; Save Ride stores the vehicle model only. Recall Location and Go Back work within the same interior. Combined world effects can change several things at once. Player > Reset All Cheats clears trainer toggles and game cheats.",
            "Place a waypoint on the pause map, return to the game, and choose Teleport to waypoint. You can bind this action in Controls. Search cities and districts from your installed map, then choose Travel to selected, or try Surprise me. Travel checks for dry, walkable collision before moving you; city destinations may use another spot within that city. Your vehicle travels with you. Go Back returns to your previous location. Leave interiors before travelling to the map. Custom coordinates uses ground placement by default; turn it off only when you need an exact Z height.",
            "Select an animation and choose Play once, Loop full clip or Hold last frame, then Play Animation. Pause/Resume, Restart and Playback speed control the active clip. Drag Position to pause at a pose; First frame and Last frame jump directly there. Resume from the last frame restarts the clip. Stop or F9 releases it; movement also cancels playback with the menu closed. Animation settings contains optional names, blend and time limits. Sequences advance through up to 256 steps; only the final step can hold. F5-F8 store animation presets. Playback waits while another scripted action controls the player.",
            "Controls lists bindable actions. Click Bind key, release your keys, then press a key or Ctrl/Shift/Alt combination. Escape cancels. Click an existing shortcut to change it, or Unbind to remove it. You can also right-click supported action buttons. Shortcuts are saved and work with the trainer closed; typing, focus loss and game pause suppress them. F5-F8 are animation presets; F9 stops animation; Alt+Z opens/closes the trainer.",
            "A preview may need a moment to stream the game model. Use Retry preview after a loading error. Some custom models rely on game-specific rendering that the isolated viewer may not reproduce. Model changes, spawned vehicles and world effects are not all undone by closing the trainer."
        };
        for(int i=0;i<8;++i)if(ImGui::CollapsingHeader(titles[i])){ImGui::Indent(8);ImGui::TextWrapped("%s",help[i]);ImGui::Unindent(8);ImGui::Spacing();}
    }else{
        TrainerUI::Heading("with thanks",26);
        TrainerUI::Emphasis("valkyrie");ImGui::TextWrapped("Trainer creation and design direction. Built with Valkyrie Core and preview techniques shared across the Valkyrie tools.");
        ImGui::TextWrapped("Rockstar Games - Grand Theft Auto: San Andreas and the original game artwork. Each project team retains credit for its own name, logo and game assets.");
        ImGui::TextWrapped("Dear ImGui - Omar Cornut and contributors. plugin-sdk - Dmitry K., fastman92, LINK/2012 and contributors. Also built with SafetyHook and Zydis; thank you to their authors and contributors.");
        ImGui::TextWrapped("thanks to everyone testing the trainer and reporting bugs.");
        if(ImGui::CollapsingHeader("Third-party licenses")){
            const char* names[]={"Credits","Dear ImGui","Hooking","plugin-sdk","SafetyHook","Zydis / Zycore"};
            for(unsigned i=0;i<6;++i)if(ImGui::TreeNode(names[i])){
                if(auto text=AssetBundle::Get(108+i))ImGui::TextUnformatted(reinterpret_cast<const char*>(text.data),reinterpret_cast<const char*>(text.data+text.size));
                ImGui::TreePop();
            }
        }
        TrainerUI::Separator();
        TrainerUI::Heading("elsewhere",26);
        for(int i=0;i<5;++i){
            auto& b=brands[i];ImGui::PushID(4500+i);
            auto p=ImGui::GetCursorScreenPos();float w=ImGui::GetContentRegionAvail().x;
            bool clicked=ImGui::Selectable("##project",false,0,{w,36});bool hover=ImGui::IsItemHovered();auto* d=ImGui::GetWindowDrawList();
            if(b.texture){float scale=std::min(b.maxWidth/b.width,b.maxHeight/b.height);float iw=b.width*scale,ih=b.height*scale;
                d->AddImage(reinterpret_cast<ImTextureID>(hover?b.texture:b.muted),{p.x+(64-iw)*.5f,p.y+(36-ih)*.5f},{p.x+(64+iw)*.5f,p.y+(36+ih)*.5f},{0,0},{1,1},hover?IM_COL32_WHITE:IM_COL32(220,220,220,180));}
            d->AddText({p.x+68,p.y+9},IM_COL32(231,225,211,255),i==0?"valkyrie":b.name);
            if(hover&&*b.url){ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);}
            if(clicked&&*b.url)ShellExecuteA(nullptr,"open",b.url,nullptr,nullptr,SW_SHOWNORMAL);
            ImGui::PopID();
        }

    }
    ImGui::PopTextWrapPos();
}
}
