#include <plugin.h>
#include <common.h>
#include <CPlayerPed.h>
#include <CPad.h>
#include <safetyhook.hpp>
#include "Render.h"
#include "AutoWalk.h"
#include "ActionKeys.h"
#include "ActionBindings.h"
#include "instrument.h"
#include "log.h"
namespace AutoWalk {
namespace {safetyhook::InlineHook hook;bool active=false;const char* status="";
short __fastcall Axis(CPad* pad,void*){
 const short original=hook.thiscall<short>(pad);if(!active)return original;auto* ped=FindPlayerPed();
 auto* playerPad=reinterpret_cast<CPad*(__cdecl*)(int)>(0x53FB70)(0);
 const bool eligible=ped&&pad==playerPad&&!ped->bInVehicle&&ped->m_fHealth>0&&!Render::IsMenuOpen()&&!ActionBindings::Paused()&&ActionBindings::Focused()&&!pad->DisablePlayerControls;
 return ActionKeys::WalkAxis(original,active,eligible);
}}
void Init(){if(hook)return;auto made=safetyhook::InlineHook::create(reinterpret_cast<void*>(0x53FD30),reinterpret_cast<void*>(&Axis));if(made){hook=std::move(*made);instrument::RegisterHook("trainer autowalk input",0x53FD30,reinterpret_cast<void*>(&Axis));}else{status="Autowalk unavailable: input hook failed";logfile::Line("%s",status);}}
void Toggle(){Init();if(hook){active=!active;status="";}}
void Cancel(){active=false;}
bool Enabled(){return active;}const char* Status(){return status;}
void Update(){auto* ped=FindPlayerPed();if(!ped||ped->bInVehicle||ped->m_fHealth<=0||!ActionBindings::Focused()||ActionBindings::Paused()){active=false;return;}if(active&&(GetAsyncKeyState(VK_ESCAPE)&0x8000))active=false;}
}
