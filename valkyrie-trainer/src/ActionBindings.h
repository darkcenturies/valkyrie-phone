#pragma once
#include <functional>
namespace ActionBindings {
// state, for an action that turns something on or off: whether it is on.
void Register(const char* label,const char* group,std::function<void()> action,std::function<bool()> state={});
// The actions, in the order registered: for the phone's Trainer app.
int Count();const char* Label(int index);const char* Group(int index);void Run(int index);
int State(int index);  // -1: not a toggle; else 0 off, 1 on
void Load();void Update();void Draw();void Prompt();void Context(const char* label);
bool Capturing();bool Focused();bool Paused();
}
