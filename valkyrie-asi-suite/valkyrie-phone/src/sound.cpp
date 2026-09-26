#include "sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "game.h"
#include "log.h"
#include "script.h"

namespace sound {
namespace {

constexpr uint16_t kLoadMissionAudio = 0x03CF;    // slot, sound
constexpr uint16_t kMissionAudioLoaded = 0x03D0;  // slot
constexpr uint16_t kPlayMissionAudio = 0x03D1;    // slot
constexpr uint16_t kMissionAudioEnded = 0x03D2;   // slot
constexpr uint16_t kClearMissionAudio = 0x040D;   // slot
constexpr uint16_t kPlaySoundAt = 0x018C;         // x, y, z, sound
constexpr uintptr_t kFindPlayerCoors = 0x56E010;  // CVector(int player), hidden pointer first
// Missions use the first slots for their dialogue; the phone keeps to the
// last of the four, as SA-MP keeps to the first.
constexpr int kSlot = 4;

enum class State { Idle, Loading, Playing };

std::string g_dir;
State g_state = State::Idle;
int g_id = 0;
bool g_loop = false;
bool g_wav = false;
ULONGLONG g_since = 0;
float g_volume = 1.0f;
// The .wav being played, scaled to the volume. PlaySound reads it from here
// for as long as it plays, so it is only replaced after that has stopped.
std::vector<char> g_wavBytes;

// The file with its 16-bit samples scaled; anything else as it is.
bool LoadScaled(const std::string& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.size() < 44 || memcmp(out.data(), "RIFF", 4) != 0 || memcmp(out.data() + 8, "WAVE", 4) != 0) return true;
    size_t at = 12;
    int bits = 0, format = 0;
    while (at + 8 <= out.size()) {
        uint32_t size = 0;
        memcpy(&size, out.data() + at + 4, 4);
        const char* id = out.data() + at;
        if (memcmp(id, "fmt ", 4) == 0 && at + 8 + 16 <= out.size()) {
            uint16_t f = 0, b = 0;
            memcpy(&f, out.data() + at + 8, 2);
            memcpy(&b, out.data() + at + 8 + 14, 2);
            format = f;
            bits = b;
        } else if (memcmp(id, "data", 4) == 0 && format == 1 && bits == 16) {
            const size_t end = std::min(out.size(), at + 8 + static_cast<size_t>(size));
            // Loudness is heard on a curve: half way down the buttons is
            // about a quarter of the level.
            const float gain = g_volume * g_volume;
            for (size_t i = at + 8; i + 1 < end; i += 2) {
                int16_t v = 0;
                memcpy(&v, out.data() + i, 2);
                v = static_cast<int16_t>(std::lround(v * gain));
                memcpy(out.data() + i, &v, 2);
            }
            break;
        }
        at += 8 + size + (size & 1);
    }
    return true;
}

bool IsNumber(const std::string& s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

}  // namespace

void SetGameDir(const std::string& dir) { g_dir = dir; }

void SetVolume(float volume) { g_volume = std::clamp(volume, 0.0f, 1.0f); }

namespace {
HWAVEOUT g_clickOut = nullptr;
WAVEHDR g_clickHeader{};
std::vector<int16_t> g_clickSamples;
}  // namespace

void Click(Tone kind) {
    if (g_volume <= 0.0f) return;
    constexpr int kRate = 22050;
    if (!g_clickOut) {
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 1;
        f.nSamplesPerSec = kRate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = kRate * 2;
        if (waveOutOpen(&g_clickOut, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            g_clickOut = nullptr;
            return;
        }
    }
    // The last one stopped and let go of before its buffer is written over.
    waveOutReset(g_clickOut);
    if (g_clickHeader.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_clickOut, &g_clickHeader, sizeof g_clickHeader);
    g_clickHeader = {};
    // Each is one or two plastic ticks: a few milliseconds of bright noise
    // over a short tone, both dying away fast. The button's is bright; a
    // key's softer and duller; the lock a low double clack; the unlock a
    // quick click rising into a second.
    struct Tick {
        float at, pitch, decay, noise, gain;
    };
    Tick ticks[2] = {{0.0f, 3400.0f, 420.0f, 0.55f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
    float length = 0.022f;
    switch (kind) {
        case Tone::Button: break;
        case Tone::Key:
            ticks[0] = {0.0f, 2100.0f, 620.0f, 0.7f, 0.55f};
            length = 0.016f;
            break;
        case Tone::Lock:
            ticks[0] = {0.0f, 1500.0f, 260.0f, 0.65f, 1.0f};
            ticks[1] = {0.034f, 1150.0f, 220.0f, 0.7f, 0.8f};
            length = 0.07f;
            break;
        case Tone::Unlock:
            ticks[0] = {0.0f, 1900.0f, 330.0f, 0.6f, 0.75f};
            ticks[1] = {0.028f, 2800.0f, 300.0f, 0.55f, 0.9f};
            length = 0.06f;
            break;
    }
    const int n = static_cast<int>(kRate * length);
    g_clickSamples.assign(n, 0);
    uint32_t seed = 0x2545F491u;
    const float gain = 5200.0f * g_volume * g_volume;
    for (int i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = static_cast<float>(static_cast<int32_t>(seed >> 16) - 32768) / 32768.0f;
        const float t = static_cast<float>(i) / kRate;
        float v = 0.0f;
        for (const Tick& k : ticks) {
            if (k.gain <= 0.0f || t < k.at) continue;
            const float u = t - k.at;
            const float tone = std::sin(u * 2.0f * 3.14159265f * k.pitch);
            v += (noise * k.noise + tone * (1.0f - k.noise)) * std::exp(-u * k.decay) * k.gain;
        }
        g_clickSamples[i] = static_cast<int16_t>(std::clamp(v * gain, -32767.0f, 32767.0f));
    }
    g_clickHeader.lpData = reinterpret_cast<LPSTR>(g_clickSamples.data());
    g_clickHeader.dwBufferLength = static_cast<DWORD>(g_clickSamples.size() * 2);
    if (waveOutPrepareHeader(g_clickOut, &g_clickHeader, sizeof g_clickHeader) == MMSYSERR_NOERROR) {
        waveOutWrite(g_clickOut, &g_clickHeader, sizeof g_clickHeader);
    }
}

void Stop() {
    if (g_state != State::Idle) script::Command(kClearMissionAudio, {kSlot});
    if (g_wav) PlaySoundA(nullptr, nullptr, 0);
    g_state = State::Idle;
    g_wav = false;
    g_loop = false;
}

void Play(const std::string& sound, bool loop) {
    Stop();
    if (sound.empty() || g_volume <= 0.0f) return;
    if (!IsNumber(sound)) {
        const std::string path = sound.find(':') == std::string::npos ? g_dir + sound : sound;
        if (!LoadScaled(path, g_wavBytes)) {
            logfile::Line("sound: could not read %s", path.c_str());
            return;
        }
        g_wav = PlaySoundA(g_wavBytes.data(), nullptr,
                           SND_ASYNC | SND_MEMORY | SND_NODEFAULT | (loop ? SND_LOOP : 0)) != FALSE;
        if (!g_wav) logfile::Line("sound: could not play %s", path.c_str());
        return;
    }
    const int id = atoi(sound.c_str());
    if (id >= 1000 && id < 2000) {
        game::Vector p{};
        reinterpret_cast<game::Vector*(__cdecl*)(game::Vector*, int)>(kFindPlayerCoors)(&p, -1);
        script::Command(kPlaySoundAt, {p.x, p.y, p.z, id});
        return;
    }
    if (id < 2000) return;
    g_id = id;
    g_loop = loop;
    script::Command(kLoadMissionAudio, {kSlot, id});
    g_state = State::Loading;
    g_since = GetTickCount64();
}

void Update() {
    if (g_state == State::Loading) {
        if (script::Command(kMissionAudioLoaded, {kSlot})) {
            script::Command(kPlayMissionAudio, {kSlot});
            g_state = State::Playing;
        } else if (GetTickCount64() - g_since > 5000) {
            logfile::Line("sound: %d did not load", g_id);
            Stop();
        }
    } else if (g_state == State::Playing && script::Command(kMissionAudioEnded, {kSlot})) {
        if (g_loop) {
            script::Command(kLoadMissionAudio, {kSlot, g_id});
            g_state = State::Loading;
            g_since = GetTickCount64();
        } else {
            script::Command(kClearMissionAudio, {kSlot});
            g_state = State::Idle;
        }
    }
}

}  // namespace sound
