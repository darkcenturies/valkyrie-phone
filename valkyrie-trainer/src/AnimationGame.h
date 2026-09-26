#pragma once
#include <cstdint>
#include <string>
#include "AnimationPlayer.h"

// Game boundary for testing loading, cancellation and sequences without GTA.
namespace AnimationGame {
enum class LibraryState { Loading, Ready, Missing };
enum class TaskState { Starting, Playing, Paused, Held, Finished, Interrupted };
LibraryState EnsureLibrary(const std::string& name);
void ReleaseLibrary();
bool HasPlayer();
bool PlayerBusy();
bool Start(const AnimationPlayer::Step& step,bool loop,std::string& error);
TaskState Poll();
void StopOwnedTask();
struct Playback {float position=0,length=0,speed=1;bool available=false,paused=false,held=false;};
Playback Snapshot();
bool Control(bool paused,float speed,float seekFraction=-1);
uint64_t Now();
void Log(const std::string& message);
}
