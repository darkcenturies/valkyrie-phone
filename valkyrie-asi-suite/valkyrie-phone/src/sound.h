// The phone's sounds, as the SP-RP phone made them: the game's own sounds by
// number, through the same script commands SA-MP's PlayerPlaySound uses.
//
// A sound is written as the ini writes it: a number from 2000 up is one of the
// game's mission-audio sounds (loaded into a slot, then played - the SP-RP
// ring is 20600); from 1000 to 1999 one of its short sound effects, played at
// the player; anything else a .wav file in the game folder. Empty is silence.
#pragma once

#include <string>

namespace sound {

void SetGameDir(const std::string& dir);

// Start a sound, replacing whatever the phone was playing. `loop` keeps it
// going until Stop(): a ringing phone, an alarm.
void Play(const std::string& sound, bool loop = false);
void Stop();

// How loud the phone's own .wav tones play, 0 silent to 1 full. The game's
// numbered sounds cannot be turned down, only off: at 0 they are not played.
void SetVolume(float volume);

// The phone's own ticks, made rather than played, over whatever else is
// sounding, at the phone's volume: a button, a key typed, the phone locking
// and unlocking.
enum class Tone { Button, Key, Lock, Unlock };
void Click(Tone kind = Tone::Button);

// Run on the game thread every frame: mission audio loads over a few frames
// before it can play.
void Update();

}  // namespace sound
