#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
struct CPlayerPed;
enum {ANIMATION_STARTED=1,ANIMATION_LOOPED=2,ANIMATION_UNLOCK_LAST_FRAME=8,ANIMATION_PARTIAL=16,ANIMATION_TRANLSATE_X=64,ANIMATION_TRANLSATE_Y=128,ANIMATION_FREEZE_TRANSLATION=8192,ANIMBLENDCALLBACK_FINISH=1,TASK_SECONDARY_PARTIAL_ANIM=0,TASK_PRIMARY_PHYSICAL_RESPONSE=0,TASK_PRIMARY_EVENT_RESPONSE_TEMP=1,TASK_PRIMARY_EVENT_RESPONSE_NONTEMP=2,TASK_PRIMARY_PRIMARY=3,TASK_PRIMARY_DEFAULT=4,ABORT_PRIORITY_IMMEDIATE=2};
struct CAnimBlendHierarchy {float m_fTotalTime=4;};
struct CAnimBlendAssociation{
 unsigned m_nFlags=0;bool m_bLooped=false,m_bUnlockLastFrame=false,m_bPlaying=true;
 float m_fBlendDelta=8,m_fSpeed=1,m_fCurrentTime=0;CAnimBlendHierarchy* m_pHierarchy=nullptr;
 int m_nCallbackType=1;void(*m_pCallbackFunc)(CAnimBlendAssociation*,void*)=nullptr;void* m_pCallbackData=nullptr;
 void SetCurrentTime(float time){m_fCurrentTime=time;if(m_bLooped&&time>=m_pHierarchy->m_fTotalTime)m_fCurrentTime=0;}
};
struct RpClump{std::vector<CAnimBlendAssociation*> associations;};
namespace FakeGame {inline uint64_t clock=0;inline int aborts=0,deletes=0,requests=0,releases=0,refs=0;inline bool validRef=true,clip=true,movement=false;inline CPlayerPed* player=nullptr;inline CPlayerPed* reference=nullptr;inline RpClump* iterated=nullptr;}
struct CTaskSimpleRunNamedAnim{
 CAnimBlendAssociation* m_pAnim=nullptr;bool m_bIsFinished=false,m_bHoldLastFrame=false,dontInterrupt=false;int flags=0;
 CTaskSimpleRunNamedAnim(const char*,const char*,int f,float,int,bool locked,bool,bool,bool hold):m_bHoldLastFrame(hold),dontInterrupt(locked),flags(f){}
 ~CTaskSimpleRunNamedAnim(){++FakeGame::deletes;}
 void MakeAbortable(CPlayerPed*,int,void*){++FakeGame::aborts;if(m_pAnim){m_pAnim->m_fBlendDelta=-1000;m_pAnim->m_pCallbackFunc=nullptr;m_pAnim=nullptr;}m_bIsFinished=true;}
};
struct TaskManager {CTaskSimpleRunNamedAnim* m_aPrimaryTasks[5]{};
 void SetTask(CTaskSimpleRunNamedAnim* next,int slot,bool){delete m_aPrimaryTasks[slot];m_aPrimaryTasks[slot]=next;}
};
struct CPedIntelligence{TaskManager m_TaskMgr;};
struct CPlayerPed{CPedIntelligence* m_pIntelligence=nullptr;RpClump* m_pRwClump=nullptr;float m_fHealth=100;bool bInVehicle=false;};
inline CPlayerPed* FindPlayerPed(){return FakeGame::player;}
struct CPools{static int GetPedRef(CPlayerPed* p){FakeGame::reference=p;return 1;}static CPlayerPed* GetPed(int){return FakeGame::validRef?FakeGame::reference:nullptr;}};
inline CAnimBlendAssociation* RpAnimBlendClumpGetFirstAssociation(RpClump* c){FakeGame::iterated=c;return c->associations.empty()?nullptr:c->associations[0];}
inline CAnimBlendAssociation* RpAnimBlendGetNextAssociation(CAnimBlendAssociation* a){auto& list=FakeGame::iterated->associations;auto it=std::find(list.begin(),list.end(),a);return it!=list.end()&&++it!=list.end()?*it:nullptr;}
struct CAnimManager{struct Block{bool bLoaded=true;};inline static Block block{true};static auto* GetAnimationBlock(const char* name){return std::string(name)=="missing"?nullptr:&block;}static int GetAnimationBlockIndex(const char*){return 0;}static void AddAnimBlockRef(int){++FakeGame::refs;}static void RemoveAnimBlockRefWithoutDelete(int){--FakeGame::refs;}static void* GetAnimation(const char*,Block*){return FakeGame::clip?(void*)1:nullptr;}};
namespace plugin {enum class Commands{REQUEST_ANIMATION,REMOVE_ANIMATION};template<Commands command>void Command(const char*){if(command==Commands::REQUEST_ANIMATION)++FakeGame::requests;else ++FakeGame::releases;}}
namespace logfile {inline void Line(const char*,const char*){}}
