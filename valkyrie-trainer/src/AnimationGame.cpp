#ifdef TRAINER_ANIMATION_GAME_TEST
#include "../tests/animation-game-stubs.h"
#else
#include <plugin.h>
#include <extensions/ScriptCommands.h>
#include <CPlayerPed.h>
#include <CPedIntelligence.h>
#include <CTaskManager.h>
#include <CTaskSimpleRunNamedAnim.h>
#include <CAnimManager.h>
#include <common.h>
#include <CPools.h>
#include <CPad.h>
#include "Render.h"
#include "AutoWalk.h"
#include "log.h"
#endif
#include "AnimationGame.h"
#include <algorithm>
#include <cmath>

namespace AnimationGame {
namespace {
std::string library;
int blockIndex=-1;
bool requested=false,referenced=false;
CTaskSimpleRunNamedAnim* task=nullptr;
CPlayerPed* owner=nullptr;
int ownerRef=-1;
bool started=false,repeat=false,holdEnd=false,paused=false;
float playbackSpeed=1;int duration=-1;uint64_t lastTick=0,playedMs=0;
Playback playback;
RpClump* ownerClump=nullptr;
CAnimBlendAssociation* observed=nullptr;
CAnimBlendAssociation* LiveObserved(CPlayerPed* ped){
    if(!observed||!ped||ped!=owner||ped->m_pRwClump!=ownerClump)return nullptr;
    for(auto* a=RpAnimBlendClumpGetFirstAssociation(ownerClump);a;a=RpAnimBlendGetNextAssociation(a))if(a==observed)return a;
    return nullptr;
}
}
uint64_t Now(){
#ifdef TRAINER_ANIMATION_GAME_TEST
    return FakeGame::clock;
#else
    // Game time does not advance during pause; wall time caused false timeouts
    // after alt-tab. Extend the 32-bit game clock, tolerating new-game resets.
    static uint32_t previous=0;static uint64_t elapsed=0;
    const uint32_t current=*reinterpret_cast<uint32_t*>(0xB7CB84);
    const uint32_t delta=current-previous;if(delta<0x80000000u)elapsed+=delta;previous=current;return elapsed;
#endif
}
void Log(const std::string& message){logfile::Line("Animation: %s",message.c_str());}
bool HasPlayer(){auto* ped=FindPlayerPed();return ped && ped->m_pIntelligence && ped->m_pRwClump;}
bool PlayerBusy(){
    auto* ped=FindPlayerPed();if(!ped || !ped->m_pIntelligence)return true;
    const auto& tm=ped->m_pIntelligence->m_TaskMgr;
    return ped->m_fHealth<=0 || ped->bInVehicle
        || tm.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY]
        || tm.m_aPrimaryTasks[TASK_PRIMARY_PHYSICAL_RESPONSE]
        || tm.m_aPrimaryTasks[TASK_PRIMARY_EVENT_RESPONSE_TEMP]
        || tm.m_aPrimaryTasks[TASK_PRIMARY_EVENT_RESPONSE_NONTEMP];
}
LibraryState EnsureLibrary(const std::string& name){
    auto* block=CAnimManager::GetAnimationBlock(name.c_str());
    if(!block)return LibraryState::Missing;
    if(library!=name){ReleaseLibrary();library=name;blockIndex=CAnimManager::GetAnimationBlockIndex(name.c_str());}
    if(blockIndex<0)return LibraryState::Missing;
    if(!block->bLoaded){
        if(!requested){
            // Script opcodes use PE's patched streaming ranges, not the
            // stock IFP model-ID base of 25575.
            plugin::Command<plugin::Commands::REQUEST_ANIMATION>(name.c_str());
            requested=true;
        }
        return LibraryState::Loading;
    }
    if(!referenced){CAnimManager::AddAnimBlockRef(blockIndex);referenced=true;}
    return LibraryState::Ready;
}
void ReleaseLibrary(){
    if(requested && !library.empty())plugin::Command<plugin::Commands::REMOVE_ANIMATION>(library.c_str());
    if(referenced && blockIndex>=0)CAnimManager::RemoveAnimBlockRefWithoutDelete(blockIndex);
    library.clear();blockIndex=-1;requested=referenced=false;
}
bool Start(const AnimationPlayer::Step& step,bool loop,std::string& error){
    auto* ped=FindPlayerPed();
    if(!ped || !ped->m_pIntelligence || !ped->m_pRwClump){error="No player available";return false;}
    auto* block=CAnimManager::GetAnimationBlock(step.library.c_str());
    if(!block || !block->bLoaded){error="Animation library is not loaded";return false;}
    auto* hierarchy=CAnimManager::GetAnimation(step.name.c_str(),block);
    if(!hierarchy){error="Animation not found: "+step.library+" / "+step.name;return false;}
    int flags=(step.flags & (ANIMATION_PARTIAL|ANIMATION_TRANLSATE_X|ANIMATION_TRANLSATE_Y|ANIMATION_FREEZE_TRANSLATION)) | ANIMATION_STARTED;
    if(loop)flags|=ANIMATION_LOOPED;
    // Full-body scripted action belongs above DEFAULT, which remains intact.
    // A secondary partial slot lets the on-foot default continuously blend it out.
    auto& manager=ped->m_pIntelligence->m_TaskMgr;
    if(manager.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY]){error="Another scripted action is active";return false;}
    // Native timed/finish callbacks are not playback controls. Keep the task
    // alive at the end and decide release/hold from actual association time.
    auto* next=new CTaskSimpleRunNamedAnim(step.name.c_str(),step.library.c_str(),flags,
        step.blendDelta,-1,false,false,false,true);
    manager.SetTask(next,TASK_PRIMARY_PRIMARY,false);
    task=next;owner=ped;ownerRef=CPools::GetPedRef(ped);ownerClump=ped->m_pRwClump;
    started=false;observed=nullptr;repeat=loop;holdEnd=step.holdLastFrame&&!loop;
    paused=false;playbackSpeed=1;duration=step.timeMs;lastTick=Now();playedMs=0;playback={};
#ifndef TRAINER_ANIMATION_GAME_TEST
    AutoWalk::Cancel();
#endif
    Log("Task started: "+step.library+" / "+step.name+" in scripted primary slot; default task preserved");
    return true;
}
namespace {
CPlayerPed* OwnedPlayer(){
    if(!task||ownerRef<0||CPools::GetPed(ownerRef)!=owner)return nullptr;
    auto* ped=FindPlayerPed();
    if(!ped||ped!=owner||!ped->m_pIntelligence||ped->m_pRwClump!=ownerClump)return nullptr;
    return ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY]==task?ped:nullptr;
}
bool MovementRequested(){
#ifdef TRAINER_ANIMATION_GAME_TEST
    return FakeGame::movement;
#else
    if(Render::IsMenuOpen())return false;
    auto* pad=CPad::GetPad(0);return pad&&(std::abs(pad->GetPedWalkLeftRight())>20||std::abs(pad->GetPedWalkUpDown())>20);
#endif
}
}
Playback Snapshot(){return playback;}
bool Control(bool pause,float speed,float seekFraction){
    if(!OwnedPlayer()||task->m_bIsFinished||!task->m_pAnim)return false;
    auto* anim=task->m_pAnim;
    if(!anim->m_pHierarchy)return false;
    const float length=anim->m_pHierarchy->m_fTotalTime;
    if(!std::isfinite(length)||length<=0)return false;
    if(!pause&&playback.held)playedMs=0;
    paused=pause;playbackSpeed=std::clamp(std::isfinite(speed)?speed:1.f,.1f,3.f);
    if(std::isfinite(seekFraction)&&seekFraction>=0){
        float time=std::clamp(seekFraction,0.f,1.f)*length;
        // SetCurrentTime wraps exact endpoints for looping associations.
        const bool wasLooped=anim->m_bLooped;anim->m_bLooped=false;
        anim->SetCurrentTime(time);anim->m_bLooped=wasLooped;
        if(time>=length){paused=true;}
        anim->m_bPlaying=true;lastTick=Now();
    }
    anim->m_fSpeed=paused?0.f:playbackSpeed;
    // Resume/restart a stopped endpoint from the beginning.
    if(!paused&&anim->m_fCurrentTime>=length){anim->SetCurrentTime(0);anim->m_bPlaying=true;playedMs=0;}
    return true;
}
TaskState Poll(){
    auto* ped=OwnedPlayer();
    if(!ped||ped->m_fHealth<=0||ped->bInVehicle||MovementRequested())return TaskState::Interrupted;
    const auto& tm=ped->m_pIntelligence->m_TaskMgr;
    if(tm.m_aPrimaryTasks[TASK_PRIMARY_PHYSICAL_RESPONSE]||tm.m_aPrimaryTasks[TASK_PRIMARY_EVENT_RESPONSE_TEMP]||tm.m_aPrimaryTasks[TASK_PRIMARY_EVENT_RESPONSE_NONTEMP])return TaskState::Interrupted;
    if(task->m_bIsFinished)return TaskState::Interrupted;
    auto* anim=task->m_pAnim;if(!anim)return TaskState::Starting;
    if(!anim->m_pHierarchy)return TaskState::Interrupted;
    if(!started){observed=anim;started=true;lastTick=Now();
        Log("Association ready; duration="+std::to_string(anim->m_pHierarchy->m_fTotalTime)+" seconds");}
    const auto now=Now();if(!paused)playedMs+=now-lastTick;lastTick=now;
    const float length=anim->m_pHierarchy->m_fTotalTime;
    if(!std::isfinite(length)||length<=0)return TaskState::Interrupted;
    playback={anim->m_fCurrentTime,length,playbackSpeed,true,paused,false};
    if(anim->m_fBlendDelta<0)return TaskState::Interrupted; // blend-out is not natural completion
    if(paused)return TaskState::Paused;
    const bool atEnd=!repeat&&anim->m_fCurrentTime>=length;
    const bool timedEnd=duration>0&&playedMs>=(uint64_t)duration;
    if(atEnd||timedEnd){
        if(holdEnd){anim->m_fSpeed=0;playback.held=true;return TaskState::Held;}
        return TaskState::Finished;
    }
    return TaskState::Playing;
}
void StopOwnedTask(){
    auto* ped=ownerRef>=0&&CPools::GetPed(ownerRef)==owner?owner:nullptr;
    auto* association=LiveObserved(ped);
    if(association){association->m_bLooped=false;association->m_bUnlockLastFrame=true;association->m_nFlags|=0x4;association->m_fBlendDelta=-1000.f;}
    if(task&&ped&&ped->m_pIntelligence&&ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY]==task){
        task->m_bHoldLastFrame=false;
        if(ped->m_pRwClump==ownerClump)task->MakeAbortable(ped,ABORT_PRIORITY_IMMEDIATE,nullptr);
        else task->m_pAnim=nullptr;
        ped->m_pIntelligence->m_TaskMgr.SetTask(nullptr,TASK_PRIMARY_PRIMARY,false);
    }
    task=nullptr;owner=nullptr;ownerRef=-1;ownerClump=nullptr;observed=nullptr;started=false;playback={};
}
}
