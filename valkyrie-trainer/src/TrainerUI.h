#pragma once
#include "imgui.h"
#include <string>
namespace TrainerUI {
void SetNavIcons(ImTextureID(*callback)(int));
void SetActionContext(void(*callback)(const char*));
void Setup(const std::string& gameDirectory);
ImFont* TitleFont();
bool Begin(int& active,bool& close);
void End();
bool Button(const char* label,ImVec2 size=ImVec2(0,0));
bool PrimaryButton(const char* label,ImVec2 size=ImVec2(0,0));
bool SmallButton(const char* label);
bool Toggle(const char* label,bool* value);
void Inline(float nextWidth=180.f);
void Separator();
bool WeaponTile(ImTextureID texture,const char* name,bool selected);
bool ModelRow(const char* name,int id,bool selected);
void Heading(const char* text,float size=26.0f);
void Emphasis(const char* text);
void StatusLine(const char* text,int lines=2);
bool NumberField(const char* label,int* value,float width=150.f,int step=0);
struct AnimationPanel {int mode=0;bool active=false,paused=false;float position=0,length=0,speed=1,seek=0;};
enum class AnimationAction {None,Play,Stop,Pause,Restart,Seek,Speed};
AnimationAction DrawAnimationPanel(AnimationPanel& state);
struct WeaponChoice { const char* name; ImTextureID icon; bool usesAmmo; };
enum class WeaponAction { None,Give,Set1,Set2,Set3,All,Remove };
WeaponAction DrawWeaponPanel(const WeaponChoice* choices,int count,int& selected,int& ammo,bool& infinite);
struct PlayerPanel { bool health,armour,wanted,god,freeze,hasMoney; int money; };
enum class PlayerAction { None,Jump,Launch,Restore,Heal,WantedUp,WantedDown,ClearWanted,Fat,Skinny,Muscle,Stamina,Adrenaline,Ninja,WeaponSkills,VehicleSkills,Suicide,Reset };
PlayerAction DrawPlayerPanel(PlayerPanel& player);
}
