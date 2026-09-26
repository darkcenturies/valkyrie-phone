#pragma once
#include <string>
#include <vector>

class CPlayerPed;

namespace AnimationPlayer
{
    struct Step
    {
        std::string library;
        std::string name;
        int flags = 0;
        float blendDelta = 8.0f;
        int timeMs = -1;
        bool holdLastFrame = false;
    };

    // Queues playback on the game thread after loading its animation block.
    // Loops until stopped or interrupted by the game/player.
    void PlaySingle(const Step &step, bool loop);

    // Plays a list of animations back to back. If loopSequence is true the
    // whole list restarts from the first step once the last one finishes.
    void PlaySequence(std::vector<Step> steps, bool loopSequence);

    void Stop();
    bool IsPlaying();
    bool Active();bool Paused();
    void Pause(bool value);void Seek(float fraction);void Speed(float value);void Restart();
    float Position();float Length();float PlaybackSpeed();
    const std::string& Status();
    int  CurrentStepIndex();
    int  TotalSteps();

    // Must be called once per game frame.
    void Update();
}
