#include "AnimationPlayer.h"
#include "AnimationGame.h"
#include <cmath>
#include <utility>
#include <algorithm>

namespace {
enum class Mode { None,Single,Sequence };
enum class Phase { Loading,Starting,Playing };
Mode mode=Mode::None;
Phase phase=Phase::Loading;
std::vector<AnimationPlayer::Step> steps;
size_t index=0;
bool loop=false,replacePending=false,stopPending=false;
uint64_t phaseSince=0;
bool paused=false,controlPending=false;float speed=1,seek=-1;
std::string status="Select an animation";
void SetStatus(std::string value){if(status!=value){status=std::move(value);AnimationGame::Log(status);}}
void Finish(const std::string& message){
    AnimationGame::StopOwnedTask();AnimationGame::ReleaseLibrary();
    mode=Mode::None;paused=false;controlPending=false;seek=-1;SetStatus(message);
}
bool Valid(const AnimationPlayer::Step& s){
    // Capacities of CTaskSimpleRunNamedAnim's fixed strings, including NUL.
    return !s.library.empty() && s.library.size()<16 && !s.name.empty() && s.name.size()<24
        && std::isfinite(s.blendDelta) && s.blendDelta>0 && s.blendDelta<=1000 && s.timeMs>=-1
        && s.timeMs!=0 && (s.flags & ~0x20DC)==0;
}
void Queue(std::vector<AnimationPlayer::Step> next,Mode nextMode,bool repeat){
    // Menu calls run at EndScene; task and streaming changes belong in Update.
    steps=std::move(next);index=0;mode=nextMode;loop=repeat;
    replacePending=true;stopPending=false;phase=Phase::Loading;
    paused=false;seek=-1;controlPending=true;
    SetStatus("Queued");
}
}
namespace AnimationPlayer {
void PlaySingle(const Step& step,bool repeat){Queue({step},Mode::Single,repeat);}
void PlaySequence(std::vector<Step> next,bool repeat){if(!next.empty())Queue(std::move(next),Mode::Sequence,repeat);}
void Stop(){stopPending=true;replacePending=false;mode=Mode::None;SetStatus("Stopping");}
bool IsPlaying(){return mode!=Mode::None && phase==Phase::Playing && !replacePending;}
bool Active(){return mode!=Mode::None;}
bool Paused(){return paused||AnimationGame::Snapshot().held;}
void Pause(bool value){paused=value;controlPending=true;}
void Seek(float value){if(std::isfinite(value)){seek=std::clamp(value,0.f,1.f);paused=true;controlPending=true;}}
void Speed(float value){if(std::isfinite(value)){if(Active())paused=Paused();speed=std::clamp(value,.1f,3.f);controlPending=true;}}
void Restart(){if(mode!=Mode::None){seek=0;paused=false;controlPending=true;}}
float Position(){return AnimationGame::Snapshot().position;}
float Length(){return AnimationGame::Snapshot().length;}
float PlaybackSpeed(){return speed;}
const std::string& Status(){return status;}
int CurrentStepIndex(){return static_cast<int>(index);}
int TotalSteps(){return static_cast<int>(steps.size());}
void Update(){
    if(stopPending){stopPending=false;Finish("Stopped");steps.clear();return;}
    if(replacePending){
        replacePending=false;AnimationGame::StopOwnedTask();AnimationGame::ReleaseLibrary();
        phase=Phase::Loading;phaseSince=AnimationGame::Now();
        if(steps.empty()||steps.size()>256){Finish("Choose between 1 and 256 sequence steps");return;}
        for(const auto& step:steps)if(!Valid(step)){Finish("Invalid animation name, library, blend speed or duration");return;}
    }
    if(mode==Mode::None)return;
    const auto now=AnimationGame::Now();
    if(!AnimationGame::HasPlayer()){Finish("No player available");return;}
    const auto& step=steps[index];
    if(phase==Phase::Loading){
        const auto state=AnimationGame::EnsureLibrary(step.library);
        if(state==AnimationGame::LibraryState::Missing){Finish("Animation library not found: "+step.library);return;}
        if(state==AnimationGame::LibraryState::Loading){
            if(now-phaseSince>=15000){Finish("Animation library did not load: "+step.library);return;}
            SetStatus("Loading "+step.library+"...");return;
        }
        if(AnimationGame::PlayerBusy()){phaseSince=now;SetStatus("Waiting for player to be on foot and free");return;}
        std::string error;
        auto playable=step;
        // A held last frame would prevent advancing to the next step.
        if(mode==Mode::Sequence&&(loop||index+1<steps.size())){playable.holdLastFrame=false;playable.flags&=~0x4;}
        if(!AnimationGame::Start(playable,mode==Mode::Single && loop,error)){Finish(error);return;}
        controlPending=true;phase=Phase::Starting;phaseSince=now;SetStatus("Starting "+step.library+" / "+step.name);return;
    }
    if(controlPending&&AnimationGame::Control(paused,speed,seek)){controlPending=false;seek=-1;}
    const auto task=AnimationGame::Poll();
    if(task==AnimationGame::TaskState::Starting){
        if(now-phaseSince>=5000)Finish("Animation failed to start: "+step.library+" / "+step.name);
        return;
    }
    if(task==AnimationGame::TaskState::Paused||task==AnimationGame::TaskState::Held){phase=Phase::Playing;SetStatus(task==AnimationGame::TaskState::Held?"Holding final pose":"Paused");return;}
    if(task==AnimationGame::TaskState::Playing){phase=Phase::Playing;SetStatus("Playing "+step.library+" / "+step.name);return;}
    if(task==AnimationGame::TaskState::Interrupted){Finish("Animation interrupted; press Play to retry");return;}
    AnimationGame::StopOwnedTask();AnimationGame::ReleaseLibrary();
    if(mode==Mode::Single){if(!loop){Finish("Finished");return;}}
    else if(++index>=steps.size()){
        if(!loop){Finish("Sequence finished");return;}
        index=0;
    }
    paused=false;seek=-1;controlPending=true;phase=Phase::Loading;phaseSince=now;SetStatus("Loading "+steps[index].library+"...");
}
}
