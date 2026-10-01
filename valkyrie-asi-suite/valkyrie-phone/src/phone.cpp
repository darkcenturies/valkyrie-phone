#include "phone.h"
#include "radar_start.h"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "arcade.h"
#include "bundle.h"
#include "config.h"
#include "game.h"
#include "games.h"
#include "input.h"
#include "log.h"
#include "phone_art.h"
#include "phone_art_sm.h"
#include "phone_data.h"
#include "phone3d.h"
#include "phone_model.h"
#include "picture.h"
#include "script.h"
#include "coverage.h"
#include "services.h"
#include "sound.h"
#include "sprite.h"
#include "ui.h"
#include "version.h"
#include "viewfinder.h"
#include "web.h"

namespace phone {
namespace {

using phone_data::Contact;
using phone_data::Message;
using ui::Font;
using ui::Rect;

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

constexpr uintptr_t kFindPlayerPed = 0x56E210;
constexpr size_t kPedHealth = 0x540;
constexpr size_t kPedWeaponSlot = 0x718;     // uint8, 0 is fists
constexpr uintptr_t kCutsceneRunning = 0xB5F851;
constexpr uintptr_t kGameNotLoaded = 0xBA6748 + 0xE9;  // FrontEndMenuManager
constexpr uintptr_t kUserFolder = 0x744FB0;            // InitUserDirectories, char*()
// CMessages::AddMessageJump(text, ms, flag, previousBrief). The game keeps the
// pointer, not a copy, so the text has to outlive the subtitle.
constexpr uintptr_t kAddMessageJump = 0x69F1E0;
// TASK_USE_MOBILE_PHONE (ped, bool): what a mission runs to put the phone to
// CJ's ear for a call and take it away again - phone_in, phone_talk and
// phone_out all come with it.
constexpr uint16_t kTaskUseMobilePhone = 0x0729;
// The weapon in CJ's hand, and the cellphone model the phone's own copy is
// made from (the mission phone task puts the same model in his right hand).
constexpr uintptr_t kSetCurrentWeapon = 0x5E61F0;   // CPed::SetCurrentWeapon(int slot)
constexpr uintptr_t kRequestModel = 0x4087E0;
constexpr uintptr_t kLoadAllRequestedModels = 0x40EA10;
constexpr uintptr_t kSetModelIsDeletable = 0x409C10;
constexpr int kCellphoneModel = 330;

uintptr_t PlayerPed() {
    return reinterpret_cast<uintptr_t(__cdecl*)(int)>(kFindPlayerPed)(-1);
}

// CJ is alive, in the game and not in a cutscene: the phone may stay in his
// hand and a call may go on. The pause menu does not change this.
// What CJ's body is doing (CPed::m_nPedState, ePedState in plugin-sdk):
// the moments no phone animation may be started over or put back into.
constexpr size_t kPedState = 0x46C;
int PedState(uintptr_t ped) { return ped ? *reinterpret_cast<const int*>(ped + kPedState) : 0; }

// Jumping, falling, getting up, staggering, diving; getting on or off a
// train; opening a door; getting into, out of, or pulled out of a car.
bool InTransition(uintptr_t ped) {
    switch (PedState(ped)) {
        case 41: case 42: case 43: case 44: case 45:  // jump, fall, get up, stagger, evade dive
        case 47: case 48:                             // enter train, exit train
        case 53:                                      // open door
        case 57: case 58: case 59: case 60: case 61:  // carjack, dragged from car, enter car, steal car, exit car
            return true;
        default:
            return false;
    }
}

// Dying, dead, hands up, arrested: his health can still be above nothing.
bool Overpowered(uintptr_t ped) {
    const int state = PedState(ped);
    return state == 54 || state == 55 || state == 56 || state == 62 || state == 63;
}

bool PlayerAble() {
    const uintptr_t ped = PlayerPed();
    return ped && *reinterpret_cast<const float*>(ped + kPedHealth) > 0.0f && !Overpowered(ped) &&
           !*reinterpret_cast<const bool*>(kGameNotLoaded) && !*reinterpret_cast<const int8_t*>(kCutsceneRunning);
}

// CJ is swimming - not only wading at the shore: his intelligence has a swim
// task (CPedIntelligence::GetTaskSwim, the check the game's own mission
// skip makes).
bool PlayerSwimming() {
    const uintptr_t ped = PlayerPed();
    const uintptr_t brain = ped ? *reinterpret_cast<const uintptr_t*>(ped + 0x47C) : 0;  // m_pIntelligence
    return brain && reinterpret_cast<void*(__thiscall*)(uintptr_t)>(0x601070)(brain) != nullptr;
}

// ...and the game is running, so the phone can be used.
bool PlayerCanUsePhone() { return PlayerAble() && !game::MenuIsOpen(); }

void Subtitle(const std::string& line) {
    // [Features] Subtitles: off as shipped - what is said is on the phone's
    // own screen, and the game's subtitles run under the phone.
    if (!config::Get().features.subtitles) return;
    static char ring[8][256];
    static int next = 0;
    char* slot = ring[next];
    next = (next + 1) % 8;
    const std::string safe = sprite::Printable(line);
    strncpy_s(slot, sizeof ring[0], safe.c_str(), _TRUNCATE);
    reinterpret_cast<void(__cdecl*)(const char*, uint32_t, uint16_t, bool)>(kAddMessageJump)(
        slot, 4500, 1, false);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Config {
    int key = 'P';
    float height = 0.78f;  // of the screen's height
    bool right = true;
} g_config;

std::string g_gameDir;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class Screen {
    Off, Lock, Home,
    Keypad, Recents, Contacts, ContactView, ContactEdit, ContactIcon,
    Messages, Thread, Compose,
    Photos, PhotoView, CameraRoll, PhotoOpen, Camera,
    Maps,
    Clock, Alarm, Stopwatch, Timer,
    Games,
    Internet, Sites,
    Calculator, Notes, NoteEdit,
    Weather, Stocks, Radio, Calendar, CalendarEvent,
    Flashlight,
    Settings, About, Tones,
    SavePrompt,
    Trainer,
};

// The SP-RP names for the wallpapers, in texture order.
const char* const kWallpaperNames[27] = {
    "Santa Maria Beach", "Juniper Flats", "Verona Beach", "Police Car", "Jogging Girl",
    "Unity Station", "Oldtimer", "Ganton Corner", "Gym Gains", "Muscle car", "Ball Courts",
    "Sanchez Wheelie", "Let me Bounce", "Suxxx", "Sa-bay", "East Los Santos", "Alhambra Fever",
    "Retro Skyline", "Moneymaker", "Aimed Pistol", "Ghetto Bird", "Ganton Danger", "Sports Car",
    "City Skyline", "Seville", "Narcotics", "City Skyline 2",
};

const char* const kDispatcherNames[] = {"Monica", "Jacqueline", "Janette", "Kaitlin",
                                        "Shenelle", "Brandi", "Rosalia", "Carlos",
                                        "Andrea", "Jason", "Marcus"};

enum class CallStage {
    Dialing,      // ringing out
    Unreachable,  // "can't be reached"
    Connected,    // a line that just talks
    Choose,       // the operator wants a department
    Describe,     // the operator wants it in words
    Ending,       // the operator has said goodbye
};

struct Call {
    bool active = false;
    std::string number;
    CallStage stage = CallStage::Dialing;
    ULONGLONG started = 0;
    ULONGLONG connected = 0;
    ULONGLONG stageAt = 0;
    ULONGLONG lostSince = 0;  // since when a connected call has had no service
    int choice = 0;
    std::string typed;
    std::string lastLine;  // the other end's last words, for the screen
    // The in-call screen's buttons. On speaker CJ keeps the phone in his
    // hand instead of at his ear.
    bool mute = false, keypad = false, speaker = false, hold = false;
};

struct Field {
    std::string* value = nullptr;
    size_t max = 0;
    bool digits = false;
};

struct State {
    bool loaded = false;
    int dictionary = -1;
    int hud = -1;       // the game's models\hud.txd, for its radar icons
    int frontend = -1;  // the game's models\fronten_pc.txd, for its cursor

    bool out = false;      // on screen at all
    bool focused = false;  // raised, cursor on, input captured
    float slide = 0.0f;    // 0 put away, 1 raised
    float slideVel = 0.0f;
    ULONGLONG lastTick = 0;

    Screen screen = Screen::Lock;
    Screen resume = Screen::Home;
    Screen phoneTab = Screen::Keypad;
    std::map<Screen, float> scroll;
    std::map<Screen, float> scrollMax;

    std::string dial;
    int contact = -1;
    std::string editName, editNumber, editPicture;
    int editFocus = 0;
    Screen editReturn = Screen::Contacts;

    std::string threadNumber;
    std::string draft;
    std::string composeTo;
    bool composeToFocus = true;
    bool pickingForCompose = false;
    bool editingMessages = false;
    ULONGLONG hotlineSentAt = 0;
    std::string notice;
    ULONGLONG noticeAt = 0;

    int photo = 0;

    bool sliding = false;
    float knob = 0.0f;

    Call call;
    bool phoneAtEar = false;
    ULONGLONG earLostAt = 0;   // since when the talking animation has been missing mid-call
    ULONGLONG earRetryAt = 0;  // not looked for again before this, after putting it back
    bool holding = false;   // the cellphone model is in CJ's hand
    bool flashlight = false;  // the Flashlight app's light is on
    int holsteredSlot = 0;  // the weapon slot put away to hold it
    // The phone as a weapon of its own (the Valkyrie Phone modloader folder):
    // it was the weapon in CJ's hand last frame.
    bool wieldedLastFrame = false;
    // The camera lowered with the phone: the Camera app stays up, and raising
    // the phone takes it up again, as it was.
    bool cameraPaused = false;
    bool cameraPausedSelfie = false;
    Screen cameraPausedReturn = Screen::Home;

    bool placeCursor = false;

    // A machine being played on the whole screen.
    std::unique_ptr<games::Game> game;
    ULONGLONG gameLast = 0;

    // The browser: the page on screen, where it is scrolled to in page
    // pixels, and the pages behind and ahead of it.
    struct Browser {
        int page = web::kNoPage;
        std::vector<int> back, forward;
        float scrollX = 0.0f, scrollY = 0.0f;
        bool zoomed = false;
        bool typing = false;
        std::string address;
        ULONGLONG lastTap = 0;
        float lastTapX = 0.0f, lastTapY = 0.0f;
        ULONGLONG loadingSince = 0;
        // The page coming down the phone's EDGE connection: how long it
        // takes, and when each tenth of it (top to bottom) arrives.
        ULONGLONG loadMs = 0;
        ULONGLONG chunkAt[10] = {};
        // How far the page has come, in the time it takes on a full signal:
        // it runs slower on a weak one and stops with none.
        float loaded = 0.0f;
        ULONGLONG tick = 0;
        float stalled = 0.0f;
        bool failed = false;
        std::vector<int> visited;  // pages in the browser's cache this session
        std::string toast;
        ULONGLONG toastAt = 0;
    } browser;

    // The calculator: what is on its display, what is held, and the sum
    // waiting to be done.
    struct Calc {
        std::string shown = "0";
        double acc = 0.0;
        char op = 0;
        bool fresh = true;
    } calc;

    int note = -1;  // the note being written, -1 for a new one
    std::string noteText;

    int stock = 0;              // the company whose day is drawn
    bool stockPercent = false;  // changes as percentages rather than dollars
    int radio = 0;              // the station the phone started, 0 none
    int radioHeld = 0;          // the one a call silenced, to come back after
    // The home screen being arranged: its icons jiggle and can be dragged.
    bool arranging = false;
    std::string dragging;       // the app being carried, by id
    std::string iconHeld;       // the app held down, toward arranging
    // The keypad menu: the page it is on, and the app lit on it.
    int menuPage = 0;
    std::string menuSel;
    ULONGLONG iconHeldSince = 0;
    ULONGLONG radioAt = 0;      // when the phone started it
    int calYear = 0, calMonth = 1, calDay = 1;  // the month shown and the day picked
    int calEvent = -1;                          // the event being edited, -1 a new one
    std::string calText;

    bool alarmRinging = false;
    int alarmLastMinute = -1;
    std::string ringingFor = "Alarm";  // what the ringing screen says

    struct Stopwatch {
        bool running = false;
        ULONGLONG since = 0, total = 0;
        std::vector<ULONGLONG> laps;
    } stopwatch;
    struct Timer {
        bool running = false;
        ULONGLONG length = 5 * 60 * 1000, end = 0;
    } timer;

    // The Camera app.
    struct Camera {
        bool on = false;
        bool selfie = false;
        float heading = 0.0f;  // CJ's, in degrees, which the mouse turns
        float pitch = 0.0f;    // degrees up from level
        float arm = 0.0f;      // for a selfie, how far the phone is raised
        float zoom = 1.0f;
        float lookX = 0.0f, lookY = 0.0f;  // mouse movement not yet applied
        // The picture: 1 asked for, 2 the game's camera at the lens, 3 the
        // game taking it.
        int shot = 0;
        int shotFrames = 0;
        bool savedSetting = false;
        int taken = 0;
        ULONGLONG since = 0, flashAt = 0, flippedAt = 0, shotAt = 0;
        bool refreshThumb = false;
        bool lastSaved = false;        // the last picture reached the Gallery folder
        bool savedDone = false;        // the menu's "save done" flag, put back after
        bool gameCameraMoved = false;  // fixed at the lens for a picture
        Screen returnTo = Screen::Home;
        picture::Picture thumb;  // the last picture, in the corner
        std::string thumbPath;
    } camera;

    struct Roll {
        std::vector<std::string> files;
        std::map<std::string, picture::Picture> thumbs;
        picture::Picture view;
        std::string viewPath;
        int index = -1;
        ULONGLONG scanned = 0;
    } roll;

    // Settings' list of tones: 0 the ringtone, 1 the text tone, 2 the alarm's
    // (from the ringtones); and the screen it goes back to.
    int toneKind = 0;
    Screen toneBack = Screen::Settings;
    // The save number was confirmed: the save menu opens once the phone is
    // down.
    bool pendingSave = false;
    bool confirmDelete = false;

    // One of CJ's own contacts from the ini being looked at, or -1.
    int builtIn = -1;
    ULONGLONG deleteArmedAt = 0;  // Delete Contact tapped once; a second tap within 3 s deletes
    // The Trainer app: the group open (empty: its first page), or a list
    // being picked from (0 vehicles, 1 weapons, 2 characters; -1 none).
    std::string trainerGroup;
    int trainerList = -1;

    // The back soft key was pressed: the screen's own back button takes it
    // on the next frame, as if it had been tapped.
    bool backPending = false;

    Field field;
    bool enter = false;
    bool tab = false;

    // Motion. How long the last drawn frame took, in seconds; where on the
    // home screen the app being opened was, for it to grow out of; when the
    // icons were last sent flying in; when the phone was switched on or off.
    float dt = 0.0f;
    // The side buttons. Asleep: the screen is off but the phone is on, as the
    // sleep button leaves it. The power menu is the "slide to power off" the
    // sleep button brings up when it is held. The overlay is the volume or
    // ring/silent picture the side buttons show.
    bool asleep = false;
    ULONGLONG asleepAt = 0, wokeAt = 0;
    ULONGLONG sleepHeldSince = 0;
    bool sleepHoldFired = false;
    bool powerMenu = false;
    float powerKnob = 0.0f;
    bool powerSliding = false;
    ULONGLONG overlayAt = 0;
    int overlayKind = 0;  // 0 volume, 1 ring, 2 silent
    ULONGLONG shakeUntil = 0;
    float lightR = 1.0f, lightG = 1.0f, lightB = 1.0f;  // the handset's lighting, eased
    float iconX = ui::kScreenW / 2, iconY = ui::kScreenH / 2;
    ULONGLONG homeFlyAt = 0, powerAt = 0;
    ULONGLONG bootAt = 0;  // switched on: starting up from then
    float restTilt[3] = {};  // how it sits once up: turn, tip and spin, in radians
    std::map<Screen, float> scrollVel;
    // The Maps app: the point looked at, which way is up, how far off the
    // camera is, whether it leans, and whether it follows CJ.
    struct Map {
        bool placed = false;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float heading = 0.0f;
        float dist = 360.0f;
        bool lean = false;
        bool follow = true;
        float lean01 = 0.0f;  // eased toward lean
    } map;
    // The newest bubble in the open conversation slides in when it arrives.
    std::string bubbleThread;
    size_t bubbleCount = 0;
    ULONGLONG bubbleAt = 0;

    unsigned drawnFrame = 0;
    unsigned drawGrace = 0;  // until this frame, not being drawn yet is expected (just taken out)
    bool drawnSinceUp = false;  // drawn at least once since it was last taken out or raised
    unsigned frame = 0;
    // Auto-Lock: when the phone was last used, and how far the screen has
    // dimmed toward locking.
    ULONGLONG activeAt = 0;
    float dim = 0.0f;
    input::Point lastCursor{};
} g;

// ---------------------------------------------------------------------------
// Motion
//
// Nothing on the phone just appears. A screen deeper in an app slides in from
// the right and slides back out the way it came, as the 2007 phone's do; an
// app grows out of its icon and shrinks back into the home screen; the lock
// slides away and the icons fly in; alerts rise, lists coast and spring back
// at their ends, bubbles slide in, the screen switches off like a tube.
// ---------------------------------------------------------------------------

float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

float Progress(ULONGLONG since, float ms) {
    return std::clamp(static_cast<float>(GetTickCount64() - since) / ms, 0.0f, 1.0f);
}

// Switched on, the phone takes its time starting up (BootScreen), as a phone
// of 2007 did.
// [Features] Startup / StartupSeconds; 0 when it is switched off.
float BootMs() {
    const auto& f = config::Get().features;
    return f.startup ? f.startupSeconds * 1000.0f : 0.0f;
}

bool Booting() {
    return phone_data::Get().settings.poweredOn && g.bootAt && GetTickCount64() - g.bootAt < BootMs();
}

// Off, or not yet started: nothing but holding the sleep button does
// anything.
bool Dead() { return !phone_data::Get().settings.poweredOn || Booting(); }

enum class Motion { None, Push, Pop, ZoomIn, ZoomOut, Unlock };

struct Move {
    Motion motion = Motion::None;
    Screen from = Screen::Home;
    ULONGLONG at = 0;
    float ox = 0.0f, oy = 0.0f;  // where an app grows from or shrinks to
} g_move;

constexpr float kMoveMs = 330.0f;
bool g_drawingFrom = false;  // the outgoing screen is being drawn: a picture only

// How deep a screen is: the home screen, an app's first screen, and the
// screens behind that.
int Depth(Screen s) {
    switch (s) {
        case Screen::Off: case Screen::Lock: return -1;
        case Screen::Home: return 0;
        case Screen::ContactView: case Screen::Thread: case Screen::Compose: case Screen::PhotoView:
        case Screen::CameraRoll: case Screen::Sites: case Screen::NoteEdit: case Screen::About:
        case Screen::Tones: case Screen::SavePrompt: case Screen::CalendarEvent: return 2;
        case Screen::ContactEdit: case Screen::PhotoOpen: return 3;
        case Screen::ContactIcon: return 4;
        default: return 1;
    }
}

// The tabs of one app change over without moving, as the phone's tab bars do.
bool Siblings(Screen a, Screen b) {
    auto phone = [](Screen s) { return s == Screen::Keypad || s == Screen::Recents || s == Screen::Contacts; };
    auto clock = [](Screen s) {
        return s == Screen::Clock || s == Screen::Alarm || s == Screen::Stopwatch || s == Screen::Timer;
    };
    return (phone(a) && phone(b)) || (clock(a) && clock(b));
}

void StartMove(Screen from, Screen to) {
    if (from == to || !g.out || g.slide < 0.9f || !config::Get().features.motion) return;
    Motion m = Motion::None;
    if (from == Screen::Lock || from == Screen::Off) m = to == Screen::Lock ? Motion::None : Motion::Unlock;
    else if (to == Screen::Lock || to == Screen::Off) m = Motion::None;
    else if (to == Screen::Camera) m = Motion::ZoomIn;
    else if (to == Screen::Home) m = Motion::ZoomOut;
    else if (from == Screen::Home) m = Motion::ZoomIn;
    else if (Siblings(from, to)) m = Motion::None;
    else m = Depth(to) >= Depth(from) ? Motion::Push : Motion::Pop;
    if (m == Motion::None) return;
    g_move = {m, from, GetTickCount64(), g.iconX, g.iconY};
    if (m == Motion::ZoomIn && from != Screen::Home) {
        g_move.ox = ui::kScreenW / 2;
        g_move.oy = ui::kScreenH / 2;
    }
    if (m == Motion::Unlock) g.homeFlyAt = g_move.at;
    // An app opened other than from its icon grows from the middle.
    g.iconX = ui::kScreenW / 2;
    g.iconY = ui::kScreenH / 2;
}

bool Moving() {
    if (g_move.motion == Motion::None) return false;
    if (Progress(g_move.at, kMoveMs) >= 1.0f) g_move.motion = Motion::None;
    return g_move.motion != Motion::None;
}

void Save() { phone_data::Save(); }
void Alert(const char* title, const std::string& text, float& buttonsY);
void StartCamera(bool selfie);
void PlayAlert(const std::string& tone, ULONGLONG buzz = 0);
void TogglePower();
void Wake();
void Sleep();
void KeyA();
void KeyMinus();
void ChangeVolume(int by);
void ToggleSilent();
void Open();
void Raise();
void PutAway();
int AlarmMinutes();
bool AlarmOn();
const config::Tone& Ringtone();
const config::Tone& AlarmTone();
bool SettingRow(float y, const char* icon, const std::string& title, const std::string& detail, bool first,
                bool last);

float& Scroll() { return g.scroll[g.screen]; }

// Settings kept beside the rest in the data file's store, so an older file
// reads as the defaults: on/off switches, and Auto-Lock in minutes (0 never).
bool Flag(const char* key, bool fallback) {
    const auto& store = phone_data::Get().store;
    const auto it = store.find(key);
    return it == store.end() ? fallback : it->second == "1";
}
void SetFlag(const char* key, bool on) {
    phone_data::Get().store[key] = on ? "1" : "0";
    Save();
}
bool LockSounds() { return Flag("locksounds", true) && !phone_data::Get().settings.silent; }
bool KeyClicks() { return Flag("keyclicks", true) && !phone_data::Get().settings.silent; }
constexpr int kAutoLockChoices[] = {1, 2, 5, 0};
int AutoLockMinutes() {
    const auto& store = phone_data::Get().store;
    const auto it = store.find("autolock");
    return it == store.end() ? 2 : std::clamp(atoi(it->second.c_str()), 0, 60);
}

void Go(Screen s) {
    // The screen being slid away is only a picture of itself.
    if (g_drawingFrom) return;
    const Screen from = g.screen;
    // Gone to another screen: a camera paused by lowering is not come back to.
    if (s != Screen::Camera) g.cameraPaused = false;
    g.screen = s;
    StartMove(from, s);
    g.scroll[s] = 0.0f;
    g.scrollMax[s] = 0.0f;
    g.scrollVel[s] = 0.0f;
    g.field = {};
    g.editingMessages = false;
}

void Notice(const std::string& text) {
    g.notice = text;
    g.noticeAt = GetTickCount64();
}

// ---------------------------------------------------------------------------
// The look: San Andreas' own
//
// Flat black panels with the world showing through, as the help box and the
// stats panel are. Text in the subtitle font with the black edge the game
// gives text drawn over the world. Titles and the selected line in the pause
// menu's pale blue. Money-green and wasted-red for go and stop. The clock in
// Pricedown, as the HUD clock is.
// ---------------------------------------------------------------------------

constexpr uint32_t kWhite = 0xFFFFFFFF;
constexpr uint32_t kBlack = 0xFF000000;
// The colours, as shipped; [Look] in the ini sets them (ApplyLook).
uint32_t kMenuBlue = 0xFFACCBF1;   // the pause menu's text
uint32_t kMenuBlueBar = 0x5AACCBF1;
uint32_t kGrey = 0xFFB4B4B4;
uint32_t kDimGrey = 0xFF8A8A8A;
uint32_t kPanel = 0xAA000000;
uint32_t kPanelDark = 0xDC000000;
uint32_t kMoneyGreen = 0xFF36682C;  // the HUD's money
uint32_t kWastedRed = 0xFFB4191D;   // the HUD's health bar
uint32_t kSeparator = 0x30FFFFFF;
uint32_t kBand = 0xFF4F86C6;     // the iFruit's title band
uint32_t kBandTop = 0xFF7AA9DE;

void ApplyLook() {
    const auto& l = config::Get().look;
    kMenuBlue = l.accent;
    kMenuBlueBar = l.accentBar;
    kGrey = l.grey;
    kDimGrey = l.dimGrey;
    kPanel = l.panel;
    kPanelDark = l.panelDark;
    kMoneyGreen = l.green;
    kWastedRed = l.red;
    kSeparator = l.separator;
    kBand = l.band;
    kBandTop = l.bandTop;
}

Font F(float size, uint32_t argb = kWhite, sprite::Align align = sprite::Align::Left, int edge = 1) {
    Font f;
    f.size = size;
    f.argb = argb;
    f.align = align;
    f.edge = edge;
    return f;
}

Font Pricedown(float size, uint32_t argb = kWhite, sprite::Align align = sprite::Align::Centre) {
    Font f = F(size, argb, align, 2);
    f.face = sprite::Face::Pricedown;
    return f;
}

// ---------------------------------------------------------------------------
// The keypad skin ([Phone] Skin=Keypad)
//
// The keypad phone: a black handset, and under its screen three round keys ringed
// in lit teal - A, the pad, and minus. The display is black with a cold blue
// haze; the menu is teal tiles three by three with the one lit named above
// them in capitals; the signal and the battery are in lime; titles are set in
// capitals; buttons are dark with teal edges.
// ---------------------------------------------------------------------------

constexpr uint32_t kSmTeal = 0xFF56D6EC;
constexpr uint32_t kSmLime = 0xFFBAD646;  // the signal and the battery

bool KeypadSkin() { return config::Get().keypad; }

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// The skin's lettering: the menu's upright capitals.
Font Caps(float size, uint32_t argb = kWhite, sprite::Align align = sprite::Align::Centre) {
    Font f = F(size, argb, align, 0);
    f.face = sprite::Face::Menu;
    return f;
}

// An arrowhead: its point at (x, y), pointing left or right.
void Chevron(float x, float y, bool left, float size, float width, uint32_t argb) {
    const float a = left ? 0.785f : -0.785f;
    ui::Needle(x, y, a, size, width * 0.5f, width, argb);
    ui::Needle(x, y, 3.14159265f - a, size, width * 0.5f, width, argb);
}

// The edge a button of this colour gets: its own colour lit up, or teal for
// a grey one.
uint32_t SmEdge(uint32_t fill) {
    const int r = (fill >> 16) & 255, gg = (fill >> 8) & 255, b = fill & 255;
    if (std::max({r, gg, b}) - std::min({r, gg, b}) < 30) return kSmTeal;
    auto up = [](int c) { return static_cast<uint32_t>(c + (255 - c) * 0.35f); };
    return 0xFF000000 | (up(r) << 16) | (up(gg) << 8) | up(b);
}

// The handset's textures: the skin's own in place of the iFruit's.
const char* BodyArt(const char* name) {
    if (!KeypadSkin()) return name;
    if (strcmp(name, "body") == 0) return "sm_body";
    if (strcmp(name, "phone_normal") == 0) return "sm_phone_normal";
    if (strcmp(name, "phone_material") == 0) return "sm_phone_material";
    return name;
}

// The first iPhone's bars: a 20 point status bar, 44 point navigation bars
// and toolbars, a 49 point tab bar.
constexpr float kStatusH = 20.0f;
constexpr float kHeaderH = 44.0f;
constexpr float kTabH = 49.0f;
constexpr float kContentTop = kStatusH + kHeaderH;

void Wallpaper(uint32_t argb = kWhite) {
    if (KeypadSkin()) {
        // The display's own haze, whatever wallpaper is chosen.
        ui::Image("sm_back", 0, 0, ui::kScreenW, ui::kScreenH, argb);
        return;
    }
    ui::Image(("wall_" + std::to_string(phone_data::Get().settings.wallpaper)).c_str(), 0, 0,
              ui::kScreenW, ui::kScreenH, argb);
}

// The wallpaper, dark enough to read over - what every app sits on.
void Backdrop() {
    if (KeypadSkin()) {
        ui::Image("sm_back", 0, 0, ui::kScreenW, ui::kScreenH, 0xFFB0B0B0);
        return;
    }
    Wallpaper(0xFF6E6E6E);
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, 0x96000000);
}

bool PlayerPlace(float& x, float& y, float& z, float& heading);

// The signal where CJ is: full in the three cities, fading out across the
// country between them, a bar less indoors and high up. Worked out a few
// times a second.
// The bars, from the masts on the map (coverage.cpp).
int SignalBars() { return coverage::Bars(); }

// The computer's battery, as the phone's (full on one without): polled
// every few seconds.
void BatteryLevel(int& percent, bool& charging) {
    static int level = 100;
    static bool plugged = false;
    static ULONGLONG polled = 0;
    if (!polled || GetTickCount64() - polled > 5000) {
        polled = GetTickCount64();
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryLifePercent <= 100 && !(ps.BatteryFlag & 128)) {
            level = ps.BatteryLifePercent;
            plugged = ps.ACLineStatus == 1 && level < 100;
        } else {
            level = 100;
            plugged = false;
        }
    }
    percent = level;
    charging = plugged;
}

// The keypad phone's: lime bars rising at the left, the battery
// in a white case at the right, over the display rather than a band.
void KeypadStatusBar() {
    ui::Gradient(0, 0, ui::kScreenW, kStatusH + 8.0f, 0xC0000000, 0x00000000);
    const int bars = SignalBars();
    if (bars == 0) {
        ui::Label(6.0f, 3.0f, "No Service", F(12.0f, kSmLime, sprite::Align::Left, 0));
    } else {
        for (int i = 0; i < 5; ++i) {
            const float h = 4.0f + i * 2.6f;
            ui::Fill(6.0f + i * 5.0f, 16.5f - h, 3.5f, h, i < bars ? kSmLime : 0x48BAD646);
        }
        if (g.screen == Screen::Internet || g.screen == Screen::Sites || g.screen == Screen::Maps)
            ui::Label(33.0f, 3.5f, "E", F(12.0f, kSmLime, sprite::Align::Left, 0));
    }
    if (g.screen != Screen::Lock && g.screen != Screen::Off) {
        ui::Label(ui::kScreenW / 2, 3.0f, phone_data::FormatTime(phone_data::Now()),
                  F(13.0f, 0xFFE4ECEE, sprite::Align::Centre, 0));
    }
    if (AlarmOn()) ui::Image("g_alarm", ui::kScreenW - 58.0f, 2.0f, 16.0f, 16.0f, 0xFFE4ECEE);
    if (phone_data::Get().settings.silent) {
        const float sx = ui::kScreenW - (AlarmOn() ? 78.0f : 58.0f);
        ui::Image("g_alarm", sx, 2.0f, 16.0f, 16.0f, 0xFFB0B0B0);
        ui::Needle(sx + 8.0f, 10.0f, 0.785f, 9.0f, 9.0f, 2.0f, 0xFFE03030);
    }
    int percent;
    bool charging;
    BatteryLevel(percent, charging);
    const float bx = ui::kScreenW - 36.0f, by = 5.0f, bw = 24.0f, bh = 11.0f;
    ui::Frame(bx, by, bw, bh, 1.6f, 0xFFE8E8E8);
    ui::Fill(bx + bw, by + 3.0f, 2.5f, bh - 6.0f, 0xFFE8E8E8);
    const bool low = percent <= 20 && !charging;
    ui::Fill(bx + 3.0f, by + 3.0f, (bw - 6.0f) * std::clamp(percent / 100.0f, 0.08f, 1.0f), bh - 6.0f,
             low ? kWastedRed : kSmLime);
    if (charging) {
        ui::Needle(bx + bw / 2 + 1.5f, by + bh / 2, 0.45f, 5.0f, 1.0f, 2.0f, kWhite);
        ui::Needle(bx + bw / 2 - 1.5f, by + bh / 2, 3.59f, 5.0f, 1.0f, 2.0f, kWhite);
    }
}

void StatusBar() {
    if (KeypadSkin()) {
        KeypadStatusBar();
        return;
    }
    ui::Fill(0, 0, ui::kScreenW, kStatusH, kPanelDark);
    // Signal: five rising bars with black edges, lit up to the signal there
    // is, then the carrier - and the EDGE "E" while an app is on the network.
    const int bars = SignalBars();
    if (bars == 0) {
        // No mast within reach: as the first iPhone says it, in place of the
        // bars and the carrier.
        ui::Label(5.0f, 3.0f, "No Service", F(13.0f, kWhite));
    } else {
        for (int i = 0; i < 5; ++i) {
            const float h = 4.0f + i * 2.5f;
            ui::Fill(5.0f + i * 5.0f, 16.0f - h, 4.0f, h, kBlack);
            ui::Fill(6.0f + i * 5.0f, 17.0f - h, 2.0f, h - 2.0f, i < bars ? kWhite : 0xFF505050);
        }
        ui::Label(32.0f, 3.0f, "iFruit", F(13.0f, kWhite));
    }
    if (bars > 0 && (g.screen == Screen::Internet || g.screen == Screen::Sites || g.screen == Screen::Maps)) {
        const float ex = 32.0f + ui::TextWidth("iFruit", F(13.0f)) + 5.0f;
        ui::Fill(ex, 4.0f, 11.0f, 12.0f, kWhite);
        ui::Label(ex + 5.5f, 3.5f, "E", F(11.0f, kBlack, sprite::Align::Centre, 0));
    }
    // Locked, a padlock stands where the time is: the time is big below it.
    if (g.screen == Screen::Lock || g.screen == Screen::Off) {
        const float lx = ui::kScreenW / 2;
        ui::Frame(lx - 4.5f, 2.5f, 9.0f, 9.0f, 1.8f, kWhite);
        ui::Fill(lx - 6.5f, 8.5f, 13.0f, 9.5f, kBlack);
        ui::Fill(lx - 5.5f, 9.5f, 11.0f, 7.5f, kWhite);
    } else {
        ui::Label(ui::kScreenW / 2, 3.0f, phone_data::FormatTime(phone_data::Now()),
                  F(14.0f, kWhite, sprite::Align::Centre));
    }
    // The alarm's bell while one is set, left of the battery.
    if (AlarmOn()) ui::Image("g_alarm", ui::kScreenW - 58.0f, 2.0f, 16.0f, 16.0f);
    // Silent: the bell struck through, left of the alarm's.
    if (phone_data::Get().settings.silent) {
        const float sx = ui::kScreenW - (AlarmOn() ? 78.0f : 58.0f);
        ui::Image("g_alarm", sx, 2.0f, 16.0f, 16.0f, 0xFFB0B0B0);
        ui::Needle(sx + 8.0f, 10.0f, 0.785f, 9.0f, 9.0f, 2.0f, 0xFFE03030);
    }
    // The battery, drawn like the HUD's bars: a black-edged case with a nub,
    // filled in the HUD's money green as far as the battery of the computer
    // the game runs on is charged (full on one without), red when low, and
    // a bolt through it while it charges.
    int percent;
    bool charging;
    BatteryLevel(percent, charging);
    const float bx = ui::kScreenW - 38.0f, by = 4.0f, bw = 28.0f, bh = 13.0f;
    ui::Fill(bx, by, bw, bh, kBlack);
    ui::Fill(bx + bw, by + 3.5f, 3.5f, bh - 7.0f, kBlack);
    ui::Fill(bx + 2.0f, by + 2.0f, bw - 4.0f, bh - 4.0f, 0xFF2A2A2A);
    const float fill = (bw - 6.0f) * std::clamp(percent / 100.0f, 0.08f, 1.0f);
    const bool low = percent <= 20 && !charging;
    ui::Fill(bx + 3.0f, by + 3.0f, fill, bh - 6.0f, low ? kWastedRed : 0xFF5CB84A);
    ui::Fill(bx + 3.0f, by + 3.0f, fill, 2.0f, low ? 0xFFE06060 : 0xFF9BE08A);
    if (charging) {
        ui::Needle(bx + bw / 2 + 1.5f, by + bh / 2, 0.45f, 5.5f, 1.0f, 2.2f, kWhite);
        ui::Needle(bx + bw / 2 - 1.5f, by + bh / 2, 3.59f, 5.5f, 1.0f, 2.2f, kWhite);
    }
}

bool TakeBack() {
    if (g_drawingFrom) return false;
    const bool pending = g.backPending;
    g.backPending = false;
    return pending;
}

// A navigation bar button. A back button is shaped like an arrow pointing
// the way it goes, the way the phone's are; anything else is a rounded
// block. A name starting "g_" is one of the phone's pictures instead of words.
bool BarButton(const char* text, bool right, bool back) {
    const bool picture = strncmp(text, "g_", 2) == 0;
    const Font f = F(13.0f, kWhite, sprite::Align::Centre);
    const float w = picture ? 40.0f : std::min(110.0f, ui::TextWidth(text, f) + (back ? 26.0f : 20.0f));
    const float h = 30.0f, y = kStatusH + 7.0f;
    const float x = right ? ui::kScreenW - w - 6.0f : 6.0f;
    const Rect r{x - 4.0f, kStatusH, w + 8.0f, kHeaderH};
    const bool hot = ui::Pressing(r);
    if (KeypadSkin()) {
        // A dark key with a teal edge, lit teal while pressed; a way back
        // has an arrowhead before its word.
        ui::Fill(x, y, w, h, hot ? kSmTeal : 0xB4000000);
        ui::Frame(x, y, w, h, 1.5f, kSmTeal);
        const uint32_t ink = hot ? kBlack : kWhite;
        if (picture) {
            ui::Image(text, x + (w - 24.0f) / 2, y + 3.0f, 24.0f, 24.0f, ink);
        } else if (back) {
            Chevron(x + 8.0f, y + h / 2, true, 7.0f, 2.2f, ink);
            const Font sf = F(13.0f, ink, sprite::Align::Centre, 0);
            ui::Label(x + 6.0f + w / 2, y + 8.0f, ui::Fit(text, w - 20.0f, sf), sf);
        } else {
            const Font sf = F(13.0f, ink, sprite::Align::Centre, 0);
            ui::Label(x + w / 2, y + 8.0f, ui::Fit(text, w - 8.0f, sf), sf);
        }
        return ui::Tapped(r);
    }
    const uint32_t fill = hot ? 0xFF1E3A66 : 0xFF2F5E9E;
    if (back) {
        // The point: a column of shrinking bars down the left edge.
        const float tip = 12.0f;
        for (float i = 0; i < tip; i += 1.0f) {
            const float inset = (tip - i) * (h / 2) / tip;
            ui::Fill(x + i, y + inset, 1.0f, h - inset * 2, kBlack);
            if (i > 1.5f) ui::Fill(x + i, y + inset + 1.5f, 1.0f, std::max(0.0f, h - inset * 2 - 3.0f), fill);
        }
        ui::Fill(x + tip, y, w - tip, h, kBlack);
        ui::Fill(x + tip, y + 1.5f, w - tip - 1.5f, h - 3.0f, fill);
        ui::Label(x + tip / 2 + w / 2, y + 8.0f, ui::Fit(text, w - tip - 6.0f, f), f);
    } else {
        ui::Fill(x, y, w, h, kBlack);
        ui::Fill(x + 1.5f, y + 1.5f, w - 3.0f, h - 3.0f, fill);
        if (picture) {
            ui::Image(text, x + (w - 24.0f) / 2, y + 3.0f, 24.0f, 24.0f);
        } else {
            ui::Label(x + w / 2, y + 8.0f, ui::Fit(text, w - 8.0f, f), f);
        }
    }
    return ui::Tapped(r);
}

// The navigation bar: the title in the middle, a back button on the left
// and an action on the right. Returns -1 for the left button tapped, 1 for
// the right, 0 for neither. A left button named Edit, Done or Cancel is an
// action, not a way back, and is drawn as one.
int Header(const std::string& title, const char* left = nullptr, const char* right = nullptr) {
    if (KeypadSkin()) {
        // No band: the title in capitals over the display, a teal hairline
        // under it.
        ui::Gradient(0, kStatusH, ui::kScreenW, kHeaderH, 0xD8000000, 0x90000000);
        ui::Fill(0, kContentTop - 1.0f, ui::kScreenW, 1.0f, 0x8056D6EC);
    } else {
        // The iFruit's band: light blue, a little lighter at the top.
        ui::Gradient(0, kStatusH, ui::kScreenW, kHeaderH, kBandTop, kBand);
        ui::Fill(0, kContentTop - 2.0f, ui::kScreenW, 2.0f, kBlack);
    }
    int result = 0;
    if (left) {
        const bool back = strcmp(left, "Edit") != 0 && strcmp(left, "Done") != 0 && strcmp(left, "Cancel") != 0;
        if (BarButton(left, false, back) || ((back || strcmp(left, "Cancel") == 0) && TakeBack())) result = -1;
    }
    if (right && (BarButton(right, true, false) || (strcmp(right, "Cancel") == 0 && TakeBack()))) result = 1;
    const float room = (left || right) ? 150.0f : 290.0f;
    if (KeypadSkin()) {
        const Font tf = Caps(17.0f);
        const std::string t = ui::Fit(Upper(title), room, tf);
        const float tw = ui::TextWidth(t, tf);
        ui::Image("glow", ui::kScreenW / 2 - tw / 2 - 26.0f, kStatusH + 2.0f, tw + 52.0f, 40.0f, 0x3056D6EC);
        ui::Label(ui::kScreenW / 2, kStatusH + 13.0f, t, tf);
        return result;
    }
    ui::Label(ui::kScreenW / 2, kStatusH + 11.0f, ui::Fit(title, room, F(20.0f)),
              F(20.0f, kWhite, sprite::Align::Centre));
    return result;
}

// A tab bar, as the phone's apps have along the bottom: a picture over each
// name, the one open lit. Returns the tab tapped, or -1.
struct Tab {
    const char* icon;
    const char* label;
};
int TabBar(const Tab* tabs, int count, int on) {
    const float y = ui::kScreenH - kTabH;
    ui::Fill(0, y, ui::kScreenW, kTabH, 0xF0101012);
    ui::Fill(0, y, ui::kScreenW, 1.5f, kBlack);
    const float w = ui::kScreenW / count;
    int tapped = -1;
    if (KeypadSkin()) {
        ui::Fill(0, y, ui::kScreenW, kTabH, 0xF4000000);
        ui::Fill(0, y, ui::kScreenW, 1.0f, 0x8056D6EC);
        for (int i = 0; i < count; ++i) {
            const Rect r{i * w, y, w, kTabH};
            const bool lit = i == on;
            if (lit) {
                ui::Fill(r.x + 4.0f, y + 4.0f, w - 8.0f, kTabH - 8.0f, 0x4056D6EC);
                ui::Frame(r.x + 4.0f, y + 4.0f, w - 8.0f, kTabH - 8.0f, 1.0f, kSmTeal);
            }
            ui::Image(tabs[i].icon, r.x + (w - 26.0f) / 2, y + 5.0f, 26.0f, 26.0f,
                      lit ? kWhite : (ui::Pressing(r) ? kSmTeal : 0xFF5C8E9C));
            ui::Label(r.x + w / 2, y + 32.0f, tabs[i].label, F(11.0f, lit ? kWhite : kDimGrey, sprite::Align::Centre, 0));
            if (ui::Tapped(r) && !lit) tapped = i;
        }
        return tapped;
    }
    for (int i = 0; i < count; ++i) {
        const Rect r{i * w, y, w, kTabH};
        const bool lit = i == on;
        if (lit) ui::Fill(r.x + 3.0f, y + 3.0f, w - 6.0f, kTabH - 6.0f, 0x40FFFFFF);
        ui::Image(tabs[i].icon, r.x + (w - 28.0f) / 2, y + 3.0f, 28.0f, 28.0f,
                  lit ? kWhite : (ui::Pressing(r) ? 0xFFB0B0B0 : 0xFF808080));
        ui::Label(r.x + w / 2, y + 32.0f, tabs[i].label, F(11.0f, lit ? kWhite : kDimGrey, sprite::Align::Centre));
        if (ui::Tapped(r) && !lit) tapped = i;
    }
    return tapped;
}

// The Phone app's tabs along the bottom.
void PhoneTabs() {
    static const Tab kTabs[] = {{"g_recents", "Recents"}, {"g_contacts", "Contacts"}, {"g_keypad", "Keypad"}};
    const Screen screens[] = {Screen::Recents, Screen::Contacts, Screen::Keypad};
    int on = -1;
    for (int i = 0; i < 3; ++i) {
        if (g.screen == screens[i]) on = i;
    }
    const int tapped = TabBar(kTabs, 3, on);
    if (tapped >= 0) {
        g.phoneTab = screens[tapped];
        g.pickingForCompose = false;
        Go(screens[tapped]);
    }
}

// Scroll a list area, remembering how far it can go.
// Dragged, a list follows the finger, and past its ends it gives, only half
// as far as the finger goes. Let go, it coasts on and slows, and springs back
// if it was past an end. The wheel is a push that coasts one row.
float BeginList(float top, float bottom) {
    float& s = Scroll();
    float& v = g.scrollVel[g.screen];
    const float most = g.scrollMax[g.screen];
    const float d = ui::ScrollDelta();
    const float dt = std::max(g.dt, 0.001f);
    if (ui::Dragging()) {
        s += (s < 0.0f || s > most) ? d * 0.45f : d;
        v = v * 0.5f + (d / dt) * 0.5f;
    } else {
        if (d != 0.0f) v += d * 7.0f;  // coasts d points in all
        s += v * dt;
        v *= std::exp(-7.0f * dt);
        if (std::fabs(v) < 4.0f) v = 0.0f;
        const float end = std::clamp(s, 0.0f, most);
        if (s != end) {
            if ((s < 0.0f && v < 0.0f) || (s > most && v > 0.0f)) v *= std::exp(-30.0f * dt);
            s += (end - s) * std::min(1.0f, dt * 14.0f);
            if (std::fabs(end - s) < 0.3f) s = end;
        }
    }
    s = std::clamp(s, -90.0f, most + 90.0f);
    ui::Clip(top, bottom);
    return top - s;
}

// While a list moves, a thin bar at its right edge shows where in it the
// screen is, as the phone's do, and fades once it stops.
void EndList(float top, float bottom, float contentHeight) {
    const float view = bottom - top;
    g.scrollMax[g.screen] = std::max(0.0f, contentHeight - view);
    ui::NoClip();
    if (g_drawingFrom || contentHeight <= view + 1.0f) return;
    struct Seen {
        float at = 0.0f;
        ULONGLONG moved = 0;
    };
    static std::map<Screen, Seen> seen;
    Seen& m = seen[g.screen];
    const float s = g.scroll[g.screen];
    const ULONGLONG now = GetTickCount64();
    if (std::fabs(s - m.at) > 0.25f) {
        m.at = s;
        m.moved = now;
    }
    const ULONGLONG since = now - m.moved;
    if (!m.moved || since > 900) return;
    const float alpha = since < 500 ? 1.0f : 1.0f - (since - 500) / 400.0f;
    // Squeezed while pulled past an end.
    const float most = g.scrollMax[g.screen];
    const float over = s < 0.0f ? -s : s > most ? s - most : 0.0f;
    const float len = std::max(18.0f, view * view / contentHeight - over);
    const float at = top + 3.0f + (view - 6.0f - len) * std::clamp(s / std::max(most, 1.0f), 0.0f, 1.0f);
    const uint32_t a = static_cast<uint32_t>(alpha * 150.0f) << 24;
    ui::Fill(ui::kScreenW - 6.0f, at, 3.5f, len, a | 0x101010);
    ui::Fill(ui::kScreenW - 5.5f, at + 0.5f, 2.5f, len - 1.0f, a | 0xE8E8E8);
}

// One line of a list, as a menu line: white text on the backdrop, the pale
// blue bar while it is pressed. Returns true when tapped.
bool Row(float y, float h, const std::string& title, const std::string& detail = {},
         uint32_t titleArgb = kWhite) {
    const Rect r{0, y, ui::kScreenW, h};
    if (ui::Pressing(r)) ui::Fill(r.x, r.y, r.w, r.h, kMenuBlueBar);
    const Font tf = F(18.0f, titleArgb);
    const Font df = F(14.0f, kMenuBlue, sprite::Align::Right);
    const float detailW = detail.empty() ? 0.0f : ui::TextWidth(detail, df) + 8.0f;
    ui::Label(14.0f, y + (h - 18.0f) / 2 - 1.0f, ui::Fit(title, ui::kScreenW - 34.0f - detailW, tf), tf);
    if (!detail.empty()) ui::Label(ui::kScreenW - 12.0f, y + (h - 14.0f) / 2, detail, df);
    ui::Fill(10.0f, y + h - 1.0f, ui::kScreenW - 20.0f, 1.0f, kSeparator);
    return ui::Tapped(r);
}

void Empty(const char* text) {
    ui::Label(ui::kScreenW / 2, kContentTop + 140.0f, text, F(20.0f, kDimGrey, sprite::Align::Centre));
}

// A black field with a thin frame that takes typing: pale blue while it has
// the keyboard.
void TextBox(float x, float y, float w, float h, std::string& value, const char* placeholder,
             bool focused, size_t max, bool digits, bool* tappedOut = nullptr) {
    ui::Fill(x, y, w, h, 0xC8000000);
    ui::Frame(x, y, w, h, 1.5f, focused ? kMenuBlue : 0xFF5A5A5A);
    const Font f = F(15.0f, kWhite, sprite::Align::Left, 0);
    std::string shown = value;
    if (focused) {
        g.field = {&value, max, digits};
        if ((GetTickCount64() / 500) % 2 == 0) shown += "_";
    }
    std::string visible = shown;
    while (!visible.empty() && ui::TextWidth(visible, f) > w - 20.0f) visible.erase(0, 1);
    if (value.empty() && !focused) {
        ui::Label(x + 10.0f, y + (h - 15.0f) / 2, placeholder, F(15.0f, kDimGrey, sprite::Align::Left, 0));
    } else {
        ui::Label(x + 10.0f, y + (h - 15.0f) / 2, visible, f);
    }
    if (tappedOut) *tappedOut = ui::Tapped(x, y, w, h);
}

// A flat block of colour with a black edge and white text on it.
bool Button(float x, float y, float w, float h, uint32_t fill, const std::string& text,
            float textSize = 18.0f) {
    const Rect r{x, y, w, h};
    const bool hot = ui::Pressing(r);
    if (KeypadSkin()) {
        const uint32_t edge = SmEdge(fill);
        ui::Fill(x, y, w, h, hot ? edge : 0xC8000000);
        if (!hot) ui::Fill(x, y, w, h, (fill & 0x00FFFFFF) | 0x48000000);
        ui::Frame(x, y, w, h, 1.5f, edge);
        ui::Label(x + w / 2, y + (h - textSize) / 2 - 1.0f, text,
                  F(textSize, hot ? kBlack : kWhite, sprite::Align::Centre, 0));
        return ui::Tapped(r);
    }
    ui::Fill(x, y, w, h, hot ? kWhite : fill);
    ui::Frame(x, y, w, h, 2.0f, kBlack);
    ui::Label(x + w / 2, y + (h - textSize) / 2 - 1.0f, text,
              F(textSize, hot ? kBlack : kWhite, sprite::Align::Centre, hot ? 0 : 1));
    return ui::Tapped(r);
}

// A button with one of the phone's pictures on it, and words beside it.
bool IconButton(float x, float y, float w, float h, uint32_t fill, const char* icon, const char* text = nullptr,
                bool enabled = true) {
    const Rect r{x, y, w, h};
    const bool hot = enabled && ui::Pressing(r);
    if (KeypadSkin()) {
        const uint32_t edge = SmEdge(fill);
        ui::Fill(x, y, w, h, hot ? edge : 0xC8000000);
        if (!hot) ui::Fill(x, y, w, h, (fill & 0x00FFFFFF) | 0x48000000);
        ui::Frame(x, y, w, h, 1.5f, enabled ? edge : 0xFF3A4A50);
    } else {
        ui::Fill(x, y, w, h, hot ? kWhite : fill);
        ui::Frame(x, y, w, h, 2.0f, kBlack);
    }
    const float size = std::min(32.0f, h - 10.0f);
    const uint32_t tint = enabled ? kWhite : 0x70FFFFFF;
    if (text) {
        const Font f = F(18.0f, hot ? kBlack : (enabled ? kWhite : kDimGrey), sprite::Align::Left, hot ? 0 : 1);
        const float tw = ui::TextWidth(text, f);
        const float x0 = x + (w - size - 6.0f - tw) / 2;
        ui::Image(icon, x0, y + (h - size) / 2, size, size, tint);
        ui::Label(x0 + size + 6.0f, y + (h - 18.0f) / 2 - 1.0f, text, f);
    } else {
        ui::Image(icon, x + (w - size) / 2, y + (h - size) / 2, size, size, tint);
    }
    return enabled && ui::Tapped(r);
}

// A line of a pause-menu style list of choices: centred, pale blue, white on
// the bar while pressed.
bool MenuChoice(float y, const std::string& text) {
    const Rect r{10.0f, y, ui::kScreenW - 20.0f, 40.0f};
    const bool hot = ui::Pressing(r);
    if (hot) ui::Fill(r.x, r.y, r.w, r.h, kMenuBlueBar);
    ui::Label(ui::kScreenW / 2, y + 9.0f, text, F(22.0f, hot ? kWhite : kMenuBlue, sprite::Align::Centre));
    return ui::Tapped(r);
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

std::string Dispatcher() {
    return kDispatcherNames[GetTickCount64() % (sizeof kDispatcherNames / sizeof kDispatcherNames[0])];
}

void Say(const std::string& who, const std::string& line) {
    g.call.lastLine = line;
    Subtitle(who + " says (phone): " + line);
}

void Stage(CallStage s);

void StartCall(const std::string& number) {
    if (number.empty() || g.call.active) return;
    if (!phone_data::Get().settings.poweredOn) return;
    if (!false && number == config::Get().save) {
        Go(Screen::SavePrompt);
        return;
    }
    if (!false && number == config::Get().trainer) {
        // The trainer, inside the phone, as an app of its own.
        g.dial.clear();
        g.trainerGroup.clear();
        g.trainerList = -1;
        Go(Screen::Trainer);
        return;
    }
    g.call = Call{};
    g.call.active = true;
    g.call.number = number;
    g.call.started = g.call.stageAt = GetTickCount64();
    auto& recents = phone_data::Get().recents;
    recents.insert(recents.begin(), {number, true, phone_data::Now()});
    if (recents.size() > phone_data::kMaxRecents) recents.resize(phone_data::kMaxRecents);
    Save();
    logfile::Line("phone: calling %s", number.c_str());
    if (!coverage::HasService()) {
        // Nothing to call out on.
        g.call.lastLine = "No Service";
        Stage(CallStage::Unreachable);
        return;
    }
    sound::Play(config::Get().ringback, true);
}

void EndCall() {
    if (g.call.active) logfile::Line("phone: call to %s ended", g.call.number.c_str());
    g.call.active = false;
    sound::Stop();
}

void Stage(CallStage s) {
    static const char* const kNames[] = {"dialing", "unreachable", "connected", "choose", "describe", "ending"};
    logfile::Line("phone: call to %s - %s", g.call.number.c_str(), kNames[static_cast<int>(s)]);
    g.call.stage = s;
    g.call.stageAt = GetTickCount64();
}

// The other end picks up. What each number does is the server's script for
// it, word for word where the server has words.
bool IsBuiltInContact(const std::string& number) {
    for (const auto& c : phone_data::Get().builtIn) {
        if (c.number == number) return true;
    }
    return false;
}

// The restaurant at a number, -1 for none.
int RestaurantAt(const std::string& number) {
    const config::Config& cfg = config::Get();
    if (!number.empty() && number == cfg.pizza) return 0;
    if (!number.empty() && number == cfg.burger) return 1;
    if (!number.empty() && number == cfg.chicken) return 2;
    return -1;
}

void Answer() {
    Call& c = g.call;
    c.connected = GetTickCount64();
    sound::Stop();
    const config::Config& cfg = config::Get();
    if (c.number == cfg.emergency) {
        Say("Line Operator", "9-1-1 Emergency, " + Dispatcher() +
                                 " speaking.  Which emergency service do you require?");
        Stage(CallStage::Choose);
    } else if (c.number == cfg.nonEmergency) {
        Say("Line Operator", "3-1-1 Non Emergency, " + Dispatcher() +
                                 " speaking.  Which department is your call for?");
        Stage(CallStage::Choose);
    } else if (c.number == cfg.hotline) {
        Say("Secretary", "San Andreas Network hotline, Karen speaking.  Can I take a message?");
        Stage(CallStage::Describe);
    } else if (c.number == cfg.hotel) {
        Say("A voice", "Hello, welcome to the V-Rock hotel in Las Venturas, how may I assist you?");
        Stage(CallStage::Connected);
    } else if (const int r = RestaurantAt(c.number); r >= 0) {
        static const char* const kGreetings[3] = {
            "Well Stacked Pizza Co., delivery line.  What can I get you?",
            "Thank you for calling Burger Shot, home of the Meat Stack.  What'll it be?",
            "Cluckin' Bell, delivery.  What can I get for you today?"};
        Say(services::NameOf(static_cast<services::Restaurant>(r)), kGreetings[r]);
        Stage(CallStage::Choose);
    } else if (IsBuiltInContact(c.number)) {
        c.lastLine = phone_data::NameFor(c.number) + " isn't answering.";
        Subtitle(c.lastLine);
        Stage(CallStage::Unreachable);
    } else {
        c.lastLine = "The number \"" + c.number + "\" can't be reached.";
        Subtitle(c.lastLine);
        Stage(CallStage::Unreachable);
    }
}

void Choose(int choice) {
    Call& c = g.call;
    c.choice = choice;
    if (const int r = RestaurantAt(c.number); r >= 0) {
        const auto restaurant = static_cast<services::Restaurant>(r);
        const services::Meal& meal = services::MealOf(restaurant, choice);
        const char* who = services::NameOf(restaurant);
        switch (services::Deliver(restaurant, choice)) {
            case services::Order::Sent:
                Say(who, "One " + std::string(meal.name) + ", that's $" + std::to_string(meal.price) +
                             ".  It's on its way to you now.");
                break;
            case services::Order::NoMoney:
                Say(who, "Sorry, that's $" + std::to_string(meal.price) + " and your card was declined.");
                break;
            case services::Order::Busy:
                Say(who, "You've already got an order on its way.  Hang tight.");
                break;
            case services::Order::Unreachable:
                Say(who, "Sorry, we can't deliver to where you are.  Call us from out on the street.");
                break;
        }
        Stage(CallStage::Ending);
        return;
    }
    if (c.number == config::Get().emergency) {
        Say("Line Operator", "And, what is your emergency?");
    } else {
        Say("Line Operator", "Thank you, please leave your message now.");
    }
    c.typed.clear();
    Stage(CallStage::Describe);
}

void Describe() {
    Call& c = g.call;
    const config::Config& cfg = config::Get();
    if (c.typed.size() < 4) {
        if (c.number == cfg.hotline) {
            Say("Secretary", "Sorry, I didn't hear you.  Please leave your message again.");
        } else if (c.number == cfg.emergency) {
            Say("Line Operator", "Sorry, I didn't hear you.  What is your emergency?");
        } else {
            Say("Line Operator", "Sorry, I didn't hear you.  Please leave your message again.");
        }
        return;
    }
    if (c.number == cfg.emergency) {
        if (!services::Reachable()) {
            Say("Line Operator", "Sorry, we can't trace where you are.  Please call again from outside.");
        } else if (c.choice == 0) {
            services::Request(services::Unit::Police);
            Say("Line Operator", "Thank you, the police have been notified of your location.");
        } else if (c.choice == 1) {
            services::Request(services::Unit::Fire);
            Say("Line Operator", "Thank you, the fire department has been notified of your location.");
        } else {
            services::Request(services::Unit::Medical);
            Say("Line Operator", "Thank you, an ambulance has been sent to your location.");
        }
    } else if (c.number == cfg.nonEmergency) {
        Say("Line Operator", "Thank you.  Your chosen department has been notified of your call.");
    } else {
        Say("Secretary", "Thank you, your message has been passed on to the news desk.");
    }
    Stage(CallStage::Ending);
}

void UpdateCall() {
    Call& c = g.call;
    if (!c.active) return;
    const ULONGLONG now = GetTickCount64();
    // A weak signal takes longer to get through, and at the edge of it the
    // call may not connect at all.
    const ULONGLONG ring = 2200 + static_cast<ULONGLONG>((1.0f - coverage::Quality()) * 2500.0f);
    if (c.stage == CallStage::Dialing && now - c.stageAt > ring) {
        static std::mt19937 rng(GetTickCount());
        if (!coverage::HasService() ||
            (coverage::Quality() < 0.25f && std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < 0.35f)) {
            sound::Stop();
            c.lastLine = "Call Failed";
            Stage(CallStage::Unreachable);
        } else {
            Answer();
        }
    } else if ((c.stage == CallStage::Unreachable || c.stage == CallStage::Ending) &&
               now - c.stageAt > 3000) {
        EndCall();
    } else if (c.stage == CallStage::Connected || c.stage == CallStage::Choose || c.stage == CallStage::Describe) {
        // Out of service for a few seconds, the call is lost.
        if (coverage::HasService()) {
            c.lostSince = 0;
        } else if (!c.lostSince) {
            c.lostSince = now;
        } else if (now - c.lostSince > 3000) {
            logfile::Line("phone: call to %s lost - no service", c.number.c_str());
            c.lastLine = "Call Lost";
            Stage(CallStage::Unreachable);
        }
    }
}

void CallScreen() {
    Call& c = g.call;
    Backdrop();
    StatusBar();
    ui::Label(ui::kScreenW / 2, kStatusH + 22.0f,
              ui::Fit(phone_data::NameFor(c.number), 290.0f, F(28.0f)), F(28.0f, kWhite, sprite::Align::Centre, 2));
    std::string status;
    switch (c.stage) {
        case CallStage::Dialing: status = std::string("Calling") + std::string((GetTickCount64() / 350) % 4, '.'); break;
        case CallStage::Unreachable: status = "Call Failed"; break;
        case CallStage::Ending: status = "Call Ended"; break;
        default: {
            const ULONGLONG secs = (GetTickCount64() - c.connected) / 1000;
            char buf[16];
            snprintf(buf, sizeof buf, "%llu:%02llu", secs / 60, secs % 60);
            status = buf;
        }
    }
    ui::Label(ui::kScreenW / 2, kStatusH + 60.0f, status, F(17.0f, kMenuBlue, sprite::Align::Centre));

    // What the other end last said, in a help box, in case subtitles are off.
    float y = 120.0f;
    if (!c.lastLine.empty()) {
        const auto lines = sprite::Wrap(c.lastLine.c_str(), 272.0f * ui::PixelsPerPoint(),
                                        sprite::TextStyle{sprite::Face::Subtitles,
                                                          15.0f * ui::PixelsPerPoint()});
        const float h = lines.size() * 19.0f + 16.0f;
        ui::Fill(14.0f, y, ui::kScreenW - 28.0f, h, kPanelDark);
        float ly = y + 8.0f;
        for (const auto& line : lines) {
            ui::Label(24.0f, ly, line, F(15.0f, kWhite, sprite::Align::Left, 0));
            ly += 19.0f;
        }
        y += h + 14.0f;
    }

    if (c.stage == CallStage::Choose && RestaurantAt(c.number) >= 0) {
        // The menu, at the shop's prices.
        const auto restaurant = static_cast<services::Restaurant>(RestaurantAt(c.number));
        for (int i = 0; i < services::kMeals; ++i) {
            const services::Meal& meal = services::MealOf(restaurant, i);
            if (MenuChoice(y + i * 44.0f, std::string(meal.name) + "  $" + std::to_string(meal.price))) {
                Choose(i);
                break;
            }
        }
    } else if (c.stage == CallStage::Choose) {
        const bool emergency = c.number == config::Get().emergency;
        const char* const choices911[] = {"Police", "Fire", "Medical"};
        const char* const choices311[] = {"Police Department", "Fire Department", "City Services"};
        for (int i = 0; i < 3; ++i) {
            if (MenuChoice(y + i * 44.0f, emergency ? choices911[i] : choices311[i])) Choose(i);
        }
    } else if (c.stage == CallStage::Describe) {
        TextBox(20.0f, y, 280.0f, 36.0f, c.typed, "Say it here", true, 128, false);
        if (Button(100.0f, y + 50.0f, 120.0f, 38.0f, 0xFF3A3A3E, "Speak", 17.0f) || g.enter) {
            Describe();
        } else if (c.number == config::Get().emergency) {
            // Or one tap, for when there is no time to type.
            static const char* const kQuick[3][3] = {{"Shots fired", "Robbery", "I'm being attacked"},
                                                     {"Fire", "Car on fire", "Explosion"},
                                                     {"I'm hurt", "Someone's hurt", "Car crash"}};
            const auto& quick = kQuick[std::clamp(c.choice, 0, 2)];
            for (int i = 0; i < 3; ++i) {
                if (MenuChoice(y + 100.0f + i * 40.0f, quick[i])) {
                    c.typed = quick[i];
                    Describe();
                    break;
                }
            }
        }
    }

    // The six buttons of the in-call screen, while no operator wants a
    // choice or an answer from the player.
    if (c.stage != CallStage::Choose && c.stage != CallStage::Describe) {
        struct CallKey {
            const char* icon;
            const char* label;
            bool* toggle;
        };
        const CallKey keys[6] = {{"g_mute", "mute", &c.mute},     {"g_keypad", "keypad", &c.keypad},
                                 {"g_speaker", "speaker", &c.speaker}, {"g_add", "add call", nullptr},
                                 {"g_hold", "hold", &c.hold},     {"g_book", "contacts", nullptr}};
        const float gx = 20.0f, gy = std::max(y, 190.0f), cw = 280.0f / 3, ch = 74.0f;
        ui::Fill(gx, gy, 280.0f, ch * 2, 0xC8101012);
        ui::Frame(gx, gy, 280.0f, ch * 2, 2.0f, kBlack);
        for (int i = 0; i < 6; ++i) {
            const CallKey& k = keys[i];
            const Rect r{gx + (i % 3) * cw, gy + (i / 3) * ch, cw, ch};
            const bool on = k.toggle && *k.toggle;
            const bool live = k.toggle != nullptr;
            if (on) ui::Fill(r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f, 0xFF4F86C6);
            else if (live && ui::Pressing(r)) ui::Fill(r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f, kMenuBlueBar);
            ui::Image(k.icon, r.x + (r.w - 34.0f) / 2, r.y + 10.0f, 34.0f, 34.0f, live ? kWhite : 0x60FFFFFF);
            ui::Label(r.x + r.w / 2, r.y + 50.0f, k.label, F(13.0f, live ? kWhite : kDimGrey, sprite::Align::Centre));
            if (live && ui::Tapped(r)) *k.toggle = !*k.toggle;
        }
        ui::Fill(gx + cw, gy + 6.0f, 1.0f, ch * 2 - 12.0f, 0x40FFFFFF);
        ui::Fill(gx + cw * 2, gy + 6.0f, 1.0f, ch * 2 - 12.0f, 0x40FFFFFF);
        ui::Fill(gx + 6.0f, gy + ch, 268.0f, 1.0f, 0x40FFFFFF);
    }
    if (IconButton(20.0f, ui::kScreenH - 70.0f, 280.0f, 52.0f, kWastedRed, "g_end", "End Call")) {
        EndCall();
    }
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

// "slide to unlock" as the phone writes it: dim words with a band of light
// sweeping through them, left to right, again and again. The band is the
// same words drawn bright and cut to a moving strip.
void Shimmer(float cx, float y, const char* text, float size, uint32_t rgb, float alpha) {
    if (alpha <= 0.0f) return;
    const Font dim = F(size, (static_cast<uint32_t>(alpha * 0x90) << 24) | rgb, sprite::Align::Centre, 0);
    ui::Label(cx, y, text, dim);
    ui::Flush();
    const float w = ui::TextWidth(text, dim);
    const float period = 2.4f;
    const float t = std::fmod(GetTickCount64() / 1000.0f, period) / period;
    const float band = 46.0f;
    const float bx = cx - w / 2 - band + t * (w + band * 2);
    sprite::BeginScissor(ui::ToPixelX(bx), ui::ToPixelY(y - 2.0f), ui::ToPixelX(bx + band), ui::ToPixelY(y + size + 6.0f));
    ui::Label(cx, y, text, F(size, (static_cast<uint32_t>(alpha * 0xFF) << 24) | 0xFFFFFF, sprite::Align::Centre, 0));
    ui::Flush();
    sprite::EndScissor();
}

void Unlock() {
    g.knob = 0.0f;
    g.sliding = false;
    if (LockSounds()) sound::Click(sound::Tone::Unlock);
    Go(g.resume == Screen::Lock ? Screen::Home : g.resume);
}

// Locked, the keypad phone shows the time large and the day,
// and Unlock as its left soft key (or the A key).
void KeypadLock() {
    Wallpaper();
    StatusBar();
    const phone_data::Stamp now = phone_data::Now();
    char time[8];
    snprintf(time, sizeof time, "%02d:%02d", now.hour, now.minute);
    ui::Image("glow", 40.0f, 96.0f, 240.0f, 110.0f, 0x3056D6EC);
    ui::Label(ui::kScreenW / 2, 118.0f, time, Caps(50.0f));
    ui::Label(ui::kScreenW / 2, 186.0f, Upper(phone_data::DayName(now.day)), Caps(15.0f, kSmTeal));
    const float sy = ui::kScreenH - 34.0f;
    ui::Gradient(0, sy - 14.0f, ui::kScreenW, 48.0f, 0x00000000, 0xC8000000);
    const Rect key{0, sy - 8.0f, 130.0f, 42.0f};
    ui::Label(14.0f, sy, "Unlock", F(17.0f, ui::Pressing(key) ? kSmTeal : kWhite, sprite::Align::Left, 0));
    if (ui::Tapped(key)) Unlock();
}

void LockScreen() {
    if (KeypadSkin()) {
        KeypadLock();
        return;
    }
    Wallpaper();
    StatusBar();
    // The time as the HUD clock shows it, and the day.
    ui::Fill(0, kStatusH, ui::kScreenW, 92.0f, kPanel);
    const phone_data::Stamp now = phone_data::Now();
    char time[8];
    snprintf(time, sizeof time, "%02d:%02d", now.hour, now.minute);
    ui::Label(ui::kScreenW / 2, kStatusH + 6.0f, time, Pricedown(52.0f));
    ui::Label(ui::kScreenW / 2, kStatusH + 66.0f, phone_data::DayName(now.day),
              F(17.0f, kWhite, sprite::Align::Centre));

    const float bandTop = ui::kScreenH - 96.0f;
    ui::Fill(0, bandTop, ui::kScreenW, 96.0f, kPanel);
    const Rect track{18.0f, bandTop + 22.0f, 284.0f, 52.0f};
    ui::Fill(track.x, track.y, track.w, track.h, kPanelDark);
    ui::Frame(track.x, track.y, track.w, track.h, 2.0f, kBlack);
    ui::Frame(track.x + 2.0f, track.y + 2.0f, track.w - 4.0f, track.h - 4.0f, 1.0f, 0xFF4A4A4E);
    const float knobW = 70.0f;
    const float travel = track.w - knobW - 8.0f;
    const Rect knob{track.x + 4.0f + g.knob * travel, track.y + 4.0f, knobW, track.h - 8.0f};
    if (!g.sliding && ui::Pressing(knob)) g.sliding = true;
    if (g.sliding) {
        if (ui::Dragging() || ui::Pressing(knob)) {
            g.knob = std::clamp((ui::CursorX() - track.x - 4.0f - knobW / 2) / travel, 0.0f, 1.0f);
        }
        if (!input::LeftHeld()) {
            g.sliding = false;
            if (g.knob > 0.92f) {
                Unlock();
                return;
            }
        }
    } else {
        // Let go short of the end, the knob springs back.
        g.knob = g.knob < 0.002f ? 0.0f : g.knob * std::exp(-g.dt * 14.0f);
    }
    // The words shimmer, as the phone's do, and fade as the knob covers them.
    Shimmer(track.x + track.w / 2 + 30.0f, track.y + 15.0f, "slide to unlock", 19.0f, 0x00ACCBF1,
            std::clamp(1.0f - g.knob * 2.5f, 0.0f, 1.0f));
    ui::Fill(knob.x, knob.y, knob.w, knob.h, 0xFFB8BAC0);
    ui::Frame(knob.x, knob.y, knob.w, knob.h, 2.0f, kBlack);
    ui::Label(knob.x + knob.w / 2, knob.y + 6.0f, ">", F(28.0f, kBlack, sprite::Align::Centre, 0));
}

Screen AppScreen(const std::string& id) {
    static const std::map<std::string, Screen> kScreens = {
        {"Phone", Screen::Keypad},   {"Text", Screen::Messages},     {"Contacts", Screen::Contacts},
        {"Camera", Screen::Home},    {"Photos", Screen::Photos},     {"Internet", Screen::Internet},
        {"Maps", Screen::Maps},
        {"Games", Screen::Games},    {"Clock", Screen::Clock},       {"Calculator", Screen::Calculator},
        {"Notes", Screen::Notes},    {"Settings", Screen::Settings},
        {"Weather", Screen::Weather}, {"Stocks", Screen::Stocks},    {"Radio", Screen::Radio},
        {"Calendar", Screen::Calendar}, {"Flashlight", Screen::Flashlight}};
    const auto it = kScreens.find(id);
    return it == kScreens.end() ? Screen::Home : it->second;
}

void StartCamera(bool selfie);

void OpenApp(const std::string& id) {
    if (id == "Camera") {
        StartCamera(false);
        return;
    }
    Screen s = AppScreen(id);
    if (s == Screen::Keypad) s = g.phoneTab;
    if (s == Screen::Contacts) g.phoneTab = Screen::Contacts;
    g.pickingForCompose = false;
    Go(s);
}

// An icon as the ini names it: hud:NAME from the game's radar icons, a
// picture file from the game folder, or a texture of the phone's own.
void Icon(const std::string& spec, float x, float y, float size, uint32_t tint) {
    static std::map<std::string, picture::Picture> files;
    if (spec.compare(0, 4, "hud:") == 0) {
        if (ui::GameImage(g.hud, spec.substr(4).c_str(), x, y, size, size, tint)) return;
    } else if (spec.find('.') != std::string::npos) {
        auto it = files.find(spec);
        if (it == files.end()) {
            const std::string path = spec.find(':') == std::string::npos ? g_gameDir + spec : spec;
            it = files.emplace(spec, picture::Load(path, 128)).first;
        }
        if (it->second.texture) {
            // Cut square from its middle, as the Camera Roll's tiles are.
            const picture::Picture& pic = it->second;
            const float aspect = pic.width > 0 ? static_cast<float>(pic.height) / pic.width : 1.0f;
            const float dw = aspect < 1.0f ? size / aspect : size, dh = aspect < 1.0f ? size : size * aspect;
            const float cx = x + size / 2, cy = y + size / 2;
            sprite::Flush();
            sprite::BeginScissor(ui::ToPixelX(x), ui::ToPixelY(y), ui::ToPixelX(x + size), ui::ToPixelY(y + size));
            sprite::Draw(pic.texture, ui::ToPixelX(cx - dw / 2), ui::ToPixelY(cy - dh / 2), ui::ToPixelX(cx + dw / 2),
                         ui::ToPixelY(cy + dh / 2), tint);
            sprite::Flush();
            sprite::EndScissor();
            return;
        }
    } else if (ui::Tex(spec.c_str())) {
        ui::Image(spec.c_str(), x, y, size, size, tint);
        return;
    }
    // Nothing by that name: a plain tile, so the app can still be opened.
    ui::Fill(x, y, size, size, 0xFF3A3A3E);
    ui::Frame(x, y, size, size, 2.0f, kBlack);
}

// The red count in an app icon's corner: unread texts.
int Badge(const std::string& id) {
    if (id != "Text") return 0;
    int unread = 0;
    for (const auto& m : phone_data::Get().messages) unread += (!m.outgoing && !m.read) ? 1 : 0;
    return unread;
}

void AppIcon(const config::App& app, float cx, float y, float size, bool label = true) {
    const Rect r{cx - 38.0f, y - 4.0f, 76.0f, size + (label ? 22.0f : 8.0f)};
    const uint32_t tint = ui::Pressing(r) ? 0xFF909090 : kWhite;
    Icon(app.icon, cx - size / 2, y, size, tint);
    if (const int n = Badge(app.id)) {
        const std::string count = std::to_string(n);
        const float bw = std::max(20.0f, ui::TextWidth(count, F(12.0f)) + 10.0f);
        const float bx = cx + size / 2 - bw + 6.0f, by = y - 6.0f;
        ui::Fill(bx, by, bw, 20.0f, kBlack);
        ui::Fill(bx + 2.0f, by + 2.0f, bw - 4.0f, 16.0f, kWastedRed);
        ui::Label(bx + bw / 2, by + 3.0f, count, F(12.0f, kWhite, sprite::Align::Centre));
    }
    if (label) {
        ui::Label(cx, y + size + 3.0f, ui::Fit(app.label, 78.0f, F(12.0f)), F(12.0f, kWhite, sprite::Align::Centre));
    }
    if (ui::Tapped(r)) {
        g.iconX = cx;
        g.iconY = y + size / 2;
        OpenApp(app.id);
    }
}

// The home screen as the phone has it: four across, and the dock along the
// bottom holding the four apps used most, on a glass shelf.
// The home screen's order: the ini's, as the player has rearranged it (kept
// in the phone's store as the apps' ids), with any app the ini adds later
// put at the end.
constexpr size_t kGridSlots = 16, kDockSlots = 4;

void HomeLists(std::vector<config::App>& grid, std::vector<config::App>& dock) {
    const auto& cfg = config::Get();
    std::vector<config::App> all = cfg.dock;
    for (const auto& a : cfg.apps) {
        if (std::none_of(all.begin(), all.end(), [&](const config::App& b) { return b.id == a.id; })) all.push_back(a);
    }
    auto& store = phone_data::Get().store;
    grid.clear();
    dock.clear();
    std::vector<std::string> placed;
    auto take = [&](const std::string& ids, std::vector<config::App>& into, size_t max) {
        size_t p = 0;
        while (p < ids.size()) {
            const size_t comma = std::min(ids.find(',', p), ids.size());
            const std::string id = ids.substr(p, comma - p);
            p = comma + 1;
            auto it = std::find_if(all.begin(), all.end(), [&](const config::App& a) { return a.id == id; });
            if (it == all.end() || into.size() >= max ||
                std::find(placed.begin(), placed.end(), id) != placed.end())
                continue;
            into.push_back(*it);
            placed.push_back(id);
        }
    };
    if (store.count("home.dock") || store.count("home.grid")) {
        take(store["home.dock"], dock, kDockSlots);
        take(store["home.grid"], grid, kGridSlots);
    } else {
        for (const auto& a : cfg.dock) {
            if (dock.size() < kDockSlots) {
                dock.push_back(a);
                placed.push_back(a.id);
            }
        }
    }
    for (const auto& a : all) {
        if (grid.size() < kGridSlots && std::find(placed.begin(), placed.end(), a.id) == placed.end()) {
            grid.push_back(a);
            placed.push_back(a.id);
        }
    }
}

void SaveHomeLists(const std::vector<config::App>& grid, const std::vector<config::App>& dock) {
    auto join = [](const std::vector<config::App>& list) {
        std::string out;
        for (const auto& a : list) out += (out.empty() ? "" : ",") + a.id;
        return out;
    };
    auto& store = phone_data::Get().store;
    store["home.grid"] = join(grid);
    store["home.dock"] = join(dock);
    Save();
}

void EndArranging() {
    if (!g.arranging) return;
    g.arranging = false;
    g.dragging.clear();
    logfile::Line("phone: home screen arranged");
}

// An icon and its name, and nothing else: drawn while the screen is being
// arranged, when a tap opens nothing.
void IconOnly(const config::App& app, float cx, float y, float size, float scale = 1.0f,
              uint32_t tint = 0xFFFFFFFF) {
    const float s = size * scale;
    Icon(app.icon, cx - s / 2, y + (size - s) / 2, s, tint);
    ui::Label(cx, y + size + 3.0f, ui::Fit(app.label, 78.0f, F(12.0f)), F(12.0f, kWhite, sprite::Align::Centre));
}

// The home screen as the phone has it: four across, and the dock along the
// bottom holding four, on a glass shelf. Held down, an icon starts the
// icons jiggling, and then any of them can be carried to another place -
// in the dock or out of it; the home button puts them down.
// The keypad phone's menu: the apps as teal tiles three by
// three, a page at a time, the one the cursor is on lit and named above them.
// Its soft keys along the bottom: Select opens the lit one, Off locks the
// phone. The wheel, or the arrows either side, turn the page.
std::vector<config::App> MenuApps() {
    std::vector<config::App> grid, dock;
    HomeLists(grid, dock);
    std::vector<config::App> apps = grid;
    apps.insert(apps.end(), dock.begin(), dock.end());
    // The keypad phone's order first - phone book, call, pictures; map,
    // messages, camera; settings - then the rest.
    static const char* const kOrder[] = {"Contacts", "Phone",    "Photos",   "Maps",  "Text",  "Camera",
                                         "Settings", "Calendar", "Notes",    "Internet", "Games", "Radio",
                                         "Weather",  "Stocks",   "Clock",    "Calculator", "Flashlight"};
    auto rank = [](const std::string& id) {
        for (size_t i = 0; i < sizeof kOrder / sizeof kOrder[0]; ++i) {
            if (id == kOrder[i]) return static_cast<int>(i);
        }
        return 100;
    };
    std::stable_sort(apps.begin(), apps.end(),
                     [&](const config::App& a, const config::App& b) { return rank(a.id) < rank(b.id); });
    return apps;
}

void KeypadMenu() {
    Wallpaper();
    StatusBar();
    const std::vector<config::App> apps = MenuApps();
    constexpr size_t kPerPage = 9;
    const int pages = std::max(1, static_cast<int>((apps.size() + kPerPage - 1) / kPerPage));
    const float W = ui::kScreenW;
    const float tile = 84.0f, gap = 8.0f, gridW = tile * 3 + gap * 2;
    const float x0 = (W - gridW) / 2, y0 = 90.0f;
    if (!g_drawingFrom && g.focused) {
        const int wheel = input::Wheel();
        if (wheel < 0) ++g.menuPage;
        if (wheel > 0) --g.menuPage;
    }
    g.menuPage = std::clamp(g.menuPage, 0, pages - 1);
    const size_t first = static_cast<size_t>(g.menuPage) * kPerPage;
    const size_t last = std::min(apps.size(), first + kPerPage);
    // The lit one: the tile under the cursor, else the one lit before if it
    // is on this page, else the page's first.
    bool litHere = false;
    for (size_t i = first; i < last; ++i) {
        const float tx = x0 + ((i - first) % 3) * (tile + gap), ty = y0 + ((i - first) / 3) * (tile + gap);
        if (!g_drawingFrom && g.focused && Rect{tx, ty, tile, tile}.Contains(ui::CursorX(), ui::CursorY()))
            g.menuSel = apps[i].id;
        litHere = litHere || apps[i].id == g.menuSel;
    }
    if (!litHere && first < last) g.menuSel = apps[first].id;
    // Unlocked, the tiles come up out of the dark.
    const float fly = g.homeFlyAt ? 1.0f - Ease(Progress(g.homeFlyAt, 420.0f)) : 0.0f;
    auto faded = [&](uint32_t argb) {
        const uint32_t a = static_cast<uint32_t>(((argb >> 24) & 255) * (1.0f - fly));
        return (a << 24) | (argb & 0xFFFFFF);
    };

    const config::App* lit = nullptr;
    for (size_t i = first; i < last; ++i) {
        const config::App& app = apps[i];
        const Rect r{x0 + ((i - first) % 3) * (tile + gap), y0 + ((i - first) / 3) * (tile + gap), tile, tile};
        const bool on = app.id == g.menuSel;
        if (on) lit = &app;
        const bool pressed = ui::Pressing(r);
        if (on) ui::Image("glow", r.x - 24.0f, r.y - 24.0f, tile + 48.0f, tile + 48.0f, faded(0x8056D6EC));
        ui::Image(on ? "sm_tile_on" : "sm_tile", r.x, r.y, tile, tile, faded(pressed ? 0xFFB8B8B8 : 0xFFFFFFFF));
        std::string glyph = "sm_" + app.id;
        for (char& c : glyph) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        const float in = 15.0f;
        if (ui::Tex(glyph.c_str())) {
            ui::Image(glyph.c_str(), r.x + in, r.y + in, tile - in * 2, tile - in * 2,
                      faded(on ? 0xFFFFFFFF : 0xE80C2838));
        } else {
            Icon(app.icon, r.x + in, r.y + in, tile - in * 2, faded(0xFFFFFFFF));
        }
        if (const int n = Badge(app.id)) {
            const std::string count = std::to_string(n);
            const float bw = std::max(20.0f, ui::TextWidth(count, F(12.0f)) + 10.0f);
            ui::Fill(r.x + tile - bw + 5.0f, r.y - 5.0f, bw, 20.0f, faded(0xFF000000));
            ui::Fill(r.x + tile - bw + 7.0f, r.y - 3.0f, bw - 4.0f, 16.0f, faded(kWastedRed));
            ui::Label(r.x + tile - bw / 2 + 5.0f, r.y - 2.0f, count, F(12.0f, kWhite, sprite::Align::Centre));
        }
        if (ui::Tapped(r)) {
            g.menuSel = app.id;
            g.iconX = r.x + tile / 2;
            g.iconY = r.y + tile / 2;
            OpenApp(app.id);
        }
    }
    // Its name, over the tiles.
    if (lit) {
        const Font tf = Caps(22.0f, faded(kWhite));
        const std::string name = ui::Fit(Upper(lit->label), 290.0f, tf);
        const float tw = ui::TextWidth(name, tf);
        ui::Image("glow", W / 2 - tw / 2 - 30.0f, 30.0f, tw + 60.0f, 50.0f, faded(0x3856D6EC));
        ui::Label(W / 2, 44.0f, name, tf);
    }
    // The pages: the arrows either side of the tiles, and a mark for each
    // under them.
    if (pages > 1) {
        const float mid = y0 + (tile * 3 + gap * 2) / 2;
        const Rect prev{0, mid - 40.0f, x0 - 1.0f, 80.0f}, next{x0 + gridW + 1.0f, mid - 40.0f, x0 - 1.0f, 80.0f};
        if (g.menuPage > 0) {
            Chevron(6.0f, mid, true, 11.0f, 3.0f, faded(ui::Pressing(prev) ? kWhite : kSmTeal));
            if (ui::Tapped(prev)) --g.menuPage;
        }
        if (g.menuPage < pages - 1) {
            Chevron(W - 6.0f, mid, false, 11.0f, 3.0f, faded(ui::Pressing(next) ? kWhite : kSmTeal));
            if (ui::Tapped(next)) ++g.menuPage;
        }
        const float dy = y0 + tile * 3 + gap * 2 + 16.0f;
        for (int p = 0; p < pages; ++p) {
            const float dx = W / 2 + (p - (pages - 1) / 2.0f) * 14.0f;
            ui::Fill(dx - 3.0f, dy, 6.0f, 6.0f, faded(p == g.menuPage ? kSmTeal : 0x5056D6EC));
        }
    }
    // The soft keys.
    const float sy = ui::kScreenH - 34.0f;
    ui::Gradient(0, sy - 14.0f, W, 48.0f, 0x00000000, 0xC8000000);
    const Rect select{0, sy - 8.0f, 130.0f, 42.0f}, off{W - 130.0f, sy - 8.0f, 130.0f, 42.0f};
    ui::Label(14.0f, sy, "Select", F(17.0f, ui::Pressing(select) ? kSmTeal : kWhite, sprite::Align::Left, 0));
    ui::Label(W - 14.0f, sy, "Off", F(17.0f, ui::Pressing(off) ? kSmTeal : kWhite, sprite::Align::Right, 0));
    if (ui::Tapped(select) && lit) {
        const size_t index = static_cast<size_t>(lit - apps.data()) - first;
        g.iconX = x0 + (index % 3) * (tile + gap) + tile / 2;
        g.iconY = y0 + (index / 3) * (tile + gap) + tile / 2;
        OpenApp(lit->id);
    }
    if (ui::Tapped(off)) Sleep();
}

void HomeScreen() {
    if (KeypadSkin()) {
        KeypadMenu();
        return;
    }
    Wallpaper();
    StatusBar();
    std::vector<config::App> grid, dock;
    HomeLists(grid, dock);
    const float cell = ui::kScreenW / 4, size = 57.0f;
    const ULONGLONG now = GetTickCount64();
    // Unlocked: the icons fly in from the edges, the dock rises.
    const float fly = g.homeFlyAt ? 1.0f - Ease(Progress(g.homeFlyAt, 480.0f)) : 0.0f;
    const float dockTop = ui::kScreenH - 92.0f + fly * 100.0f;
    const float gridTop = kStatusH + 14.0f;
    auto gridAt = [&](size_t i, float& cx, float& iy) {
        cx = cell * (i % 4) + cell / 2;
        iy = gridTop + (i / 4) * 90.0f;
    };
    const float dockCell = ui::kScreenW / (g.arranging ? kDockSlots : std::max<size_t>(dock.size(), 1));
    auto dockAt = [&](size_t d, float& cx, float& iy) {
        cx = dockCell * d + dockCell / 2;
        iy = dockTop + 10.0f;
    };

    if (g.arranging) {
        // Carrying one: it follows the finger, and the others make room
        // where it would go.
        if (!g.dragging.empty() && input::LeftHeld()) {
            const float x = ui::CursorX(), y = ui::CursorY();
            const bool toDock = y > dockTop - 16.0f;
            auto inGrid = std::find_if(grid.begin(), grid.end(), [&](const config::App& a) { return a.id == g.dragging; });
            auto inDock = std::find_if(dock.begin(), dock.end(), [&](const config::App& a) { return a.id == g.dragging; });
            const bool fromDock = inDock != dock.end();
            config::App carried = fromDock ? *inDock : (inGrid != grid.end() ? *inGrid : config::App{});
            if (!carried.id.empty()) {
                size_t target;
                if (toDock) {
                    target = std::min<size_t>(static_cast<size_t>(std::max(0.0f, x) / (ui::kScreenW / kDockSlots)), kDockSlots - 1);
                } else {
                    const int col = std::clamp(static_cast<int>(x / cell), 0, 3);
                    const int row = std::clamp(static_cast<int>((y - gridTop + 20.0f) / 90.0f), 0, 3);
                    target = static_cast<size_t>(row * 4 + col);
                }
                if (fromDock) dock.erase(inDock);
                else grid.erase(inGrid);
                std::vector<config::App>& into = toDock ? dock : grid;
                const size_t max = toDock ? kDockSlots : kGridSlots;
                if (into.size() >= max) {
                    // Full: the one in its place trades places with it.
                    const size_t at = std::min(target, into.size() - 1);
                    std::vector<config::App>& back = toDock ? grid : dock;
                    back.push_back(into[at]);
                    into.erase(into.begin() + at);
                }
                into.insert(into.begin() + std::min(target, into.size()), carried);
                SaveHomeLists(grid, dock);
            }
        } else if (!g.dragging.empty()) {
            g.dragging.clear();
        }
        // Picking one up.
        if (g.dragging.empty() && input::LeftPressed()) {
            const float x = ui::CursorX(), y = ui::CursorY();
            for (size_t i = 0; i < grid.size() && g.dragging.empty(); ++i) {
                float cx, iy;
                gridAt(i, cx, iy);
                if (std::fabs(x - cx) < 34.0f && y > iy - 4.0f && y < iy + size + 18.0f) g.dragging = grid[i].id;
            }
            for (size_t d = 0; d < dock.size() && g.dragging.empty(); ++d) {
                float cx, iy;
                dockAt(d, cx, iy);
                if (std::fabs(x - cx) < 34.0f && y > iy - 4.0f && y < iy + size + 10.0f) g.dragging = dock[d].id;
            }
        }
        // Touches are not cancelled here: the icons take no taps while
        // arranging, and the home button's tap is what ends it.
    }

    // Jiggling, each a little out of step with its neighbours.
    auto jiggle = [&](size_t k, float& cx, float& iy) {
        if (!g.arranging) return;
        const float t = now * 0.018f + k * 1.7f;
        cx += std::sin(t) * 1.3f;
        iy += std::cos(t * 1.13f) * 1.0f;
    };
    bool held = false;
    auto normal = [&](const config::App& app, float cx, float iy, bool label) {
        // Held down long enough, the screen goes into arranging.
        const Rect r{cx - 38.0f, iy - 4.0f, 76.0f, size + (label ? 22.0f : 8.0f)};
        if (ui::Pressing(r) && config::Get().features.arrange) {
            held = true;
            if (g.iconHeld != app.id) {
                g.iconHeld = app.id;
                g.iconHeldSince = now;
            } else if (now - g.iconHeldSince > 750) {
                g.arranging = true;
                g.dragging = app.id;
                g.iconHeld.clear();
                ui::CancelTouch();
                if (!phone_data::Get().settings.silent) sound::Click();
                return;
            }
        }
        AppIcon(app, cx, iy, size, label);
    };
    for (size_t i = 0; i < grid.size(); ++i) {
        float cx, iy;
        gridAt(i, cx, iy);
        if (fly > 0.0f) {
            const float dx = cx - ui::kScreenW / 2, dy = iy + size / 2 - ui::kScreenH / 2 + 40.0f;
            const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
            cx += dx / len * 260.0f * fly;
            iy += dy / len * 260.0f * fly;
        }
        if (g.arranging) {
            if (grid[i].id == g.dragging) continue;
            jiggle(i, cx, iy);
            IconOnly(grid[i], cx, iy, size);
        } else {
            normal(grid[i], cx, iy, true);
        }
    }
    ui::Gradient(0, dockTop, ui::kScreenW, 92.0f, 0x90606670, 0xC8202226);
    ui::Fill(0, dockTop, ui::kScreenW, 2.0f, 0xA0C8CCD4);
    for (size_t d = 0; d < dock.size(); ++d) {
        float cx, iy;
        dockAt(d, cx, iy);
        if (g.arranging) {
            if (dock[d].id == g.dragging) continue;
            jiggle(20 + d, cx, iy);
            IconOnly(dock[d], cx, iy, size);
        } else {
            normal(dock[d], cx, iy, true);
        }
    }
    if (!held) g.iconHeld.clear();
    // The one being carried, a little larger, over everything.
    if (g.arranging && !g.dragging.empty()) {
        for (const auto* list : {&grid, &dock}) {
            for (const auto& a : *list) {
                if (a.id == g.dragging) {
                    IconOnly(a, ui::CursorX(), ui::CursorY() - size / 2, size, 1.18f, 0xC0FFFFFF);
                }
            }
        }
    }
    if (g.arranging) {
        ui::Label(ui::kScreenW / 2, dockTop - 22.0f, "Press the home button when done",
                  F(12.0f, kWhite, sprite::Align::Centre, 1));
    }
}

// A notice drops down under the status bar over whatever screen is up, and
// lifts away again. Long ones wrap to a second line.
void NoticeDraw() {
    const ULONGLONG age = GetTickCount64() - g.noticeAt;
    if (g.notice.empty() || !g.noticeAt || age >= 3400) return;
    const Font f = F(14.0f, kWhite, sprite::Align::Centre);
    auto lines = sprite::Wrap(g.notice.c_str(), 296.0f * ui::PixelsPerPoint(),
                              sprite::TextStyle{sprite::Face::Subtitles, 14.0f * ui::PixelsPerPoint()});
    if (lines.size() > 2) lines.resize(2);
    const float h = 14.0f + 18.0f * std::max<size_t>(1, lines.size());
    const float in = Ease(age / 240.0f), out = age > 3100 ? Ease((age - 3100) / 300.0f) : 0.0f;
    const float y = kStatusH - h + (in - out) * h;
    ui::Fill(0, y, ui::kScreenW, h, 0xE8101012);
    ui::Fill(0, y + h - 2.0f, ui::kScreenW, 2.0f, kBlack);
    float ly = y + 7.0f;
    for (const auto& l : lines) {
        ui::Label(ui::kScreenW / 2, ly, l, f);
        ly += 18.0f;
    }
    ui::Flush();
    // The status bar stays on top of it.
    StatusBar();
}

// --- Phone: keypad ----------------------------------------------------------

// An asterisk, drawn: three bars crossing at (cx, cy), `r` points out.
void Star(float cx, float cy, float r, uint32_t argb) {
    for (int i = 0; i < 3; ++i) ui::Needle(cx, cy, i * 3.14159265f / 3.0f, r, r, 3.0f, argb);
}

void KeypadScreen() {
    Backdrop();
    StatusBar();
    // The number being dialled.
    ui::Fill(0, kStatusH, ui::kScreenW, 68.0f, kPanel);
    const Font nf = F(32.0f, kWhite, sprite::Align::Centre, 2);
    std::string shown = g.dial;
    while (!shown.empty() && ui::TextWidth(shown, nf) > 290.0f) shown.erase(0, 1);
    // The game's font has no '*', so the number is set a character at a
    // time, the stars drawn.
    {
        float width = 0.0f;
        for (char ch : shown) width += ch == '*' ? 20.0f : ui::TextWidth(std::string(1, ch), nf);
        float cx = ui::kScreenW / 2 - width / 2;
        for (char ch : shown) {
            if (ch == '*') {
                Star(cx + 10.0f, kStatusH + 32.0f, 9.0f, kWhite);
                cx += 20.0f;
            } else {
                const std::string one(1, ch);
                ui::Label(cx, kStatusH + 16.0f, one, F(32.0f, kWhite, sprite::Align::Left, 2));
                cx += ui::TextWidth(one, nf);
            }
        }
    }
    g.field = {&g.dial, phone_data::kMaxNumberLength, true};

    const char* const digits[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
    const char* const letters[12] = {"", "ABC", "DEF", "GHI", "JKL", "MNO",
                                     "PQRS", "TUV", "WXYZ", "", "", ""};
    const float top = kStatusH + 72.0f;
    // Three keys and two gaps between the 8 point margins, so the grid sits
    // in the middle of the screen with the buttons under it.
    const float gap = 4.0f, keyH = 56.0f;
    const float keyW = (ui::kScreenW - 16.0f - 2 * gap) / 3;
    for (int i = 0; i < 12; ++i) {
        const Rect r{8.0f + (i % 3) * (keyW + gap), top + (i / 3) * keyH, keyW, keyH - 4.0f};
        const bool hot = ui::Pressing(r);
        ui::Fill(r.x, r.y, r.w, r.h, hot ? kMenuBlue : kPanel);
        ui::Frame(r.x, r.y, r.w, r.h, 1.5f, kBlack);
        if (i == 9) Star(r.x + r.w / 2, r.y + 22.0f, 10.0f, hot ? kBlack : kWhite);
        else ui::Label(r.x + r.w / 2, r.y + 6.0f, digits[i], F(28.0f, hot ? kBlack : kWhite, sprite::Align::Centre, hot ? 0 : 1));
        ui::Label(r.x + r.w / 2, r.y + 36.0f, letters[i], F(10.0f, hot ? kBlack : kMenuBlue, sprite::Align::Centre, 0));
        // * and # too, for codes like the trainer's; the server's own
        // numbers are digits only and simply cannot be reached with them.
        if (ui::Tapped(r) && g.dial.size() < phone_data::kMaxNumberLength) {
            g.dial += digits[i];
            PlayAlert(config::Get().keyTone);
        }
    }
    const float barTop = top + 4 * keyH + 2.0f;
    const float bh = ui::kScreenH - kTabH - barTop - 8.0f;
    if (IconButton(8.0f, barTop, keyW, bh, 0xFF3A3A3E, "g_addcontact") && !g.dial.empty()) {
        g.contact = -1;
        g.builtIn = -1;
        g.editName.clear();
        g.editNumber = g.dial;
        g.editPicture.clear();
        g.editFocus = 0;
        g.editReturn = Screen::Keypad;
        Go(Screen::ContactEdit);
        return;
    }
    if (IconButton(8.0f + keyW + gap, barTop, keyW, bh, kMoneyGreen, "g_call", "Call") || g.enter) {
        if (g.dial.empty()) {
            const auto& recents = phone_data::Get().recents;
            if (!recents.empty()) g.dial = recents.front().number;
        } else {
            StartCall(g.dial);
            g.dial.clear();
        }
    }
    if (IconButton(8.0f + 2 * (keyW + gap), barTop, keyW, bh, 0xFF3A3A3E, "g_backspace") && !g.dial.empty()) {
        g.dial.pop_back();
    }
    PhoneTabs();
}

// --- Phone: recents ---------------------------------------------------------

void RecentsScreen() {
    auto& recents = phone_data::Get().recents;
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH - kTabH;
    float y = BeginList(top, bottom);
    const float y0 = y;
    std::string call;
    for (const auto& r : recents) {
        if (Row(y, 44.0f, phone_data::NameFor(r.number), phone_data::FormatDayTime(r.at))) call = r.number;
        y += 44.0f;
    }
    if (recents.empty()) Empty("No Recents");
    EndList(top, bottom, y - y0);
    ui::Flush();
    StatusBar();
    if (Header("Recents", nullptr, recents.empty() ? nullptr : "Clear") == 1) {
        recents.clear();
        Save();
    }
    PhoneTabs();
    if (!call.empty()) StartCall(call);
}

// --- Phone: contacts --------------------------------------------------------

// CJ's people from the ini that the player has deleted, or edited into
// their own contacts, by name; kept in the phone's store.
bool BuiltInHidden(const std::string& name) {
    const std::string& hidden = phone_data::Get().store["contacts.hidden"];
    size_t p = 0;
    while (p <= hidden.size()) {
        const size_t bar = std::min(hidden.find('|', p), hidden.size());
        if (hidden.compare(p, bar - p, name) == 0 && bar - p == name.size()) return true;
        p = bar + 1;
    }
    return false;
}

void HideBuiltIn(int index) {
    auto& d = phone_data::Get();
    if (index < 0 || index >= static_cast<int>(d.builtIn.size())) return;
    std::string& hidden = d.store["contacts.hidden"];
    if (!hidden.empty()) hidden += '|';
    hidden += d.builtIn[index].name;
    d.builtIn.erase(d.builtIn.begin() + index);
}

void ContactsScreen() {
    auto& contacts = phone_data::Get().contacts;
    const auto& builtIn = phone_data::Get().builtIn;
    Backdrop();
    const float top = kContentTop;
    const float bottom = g.pickingForCompose ? ui::kScreenH : ui::kScreenH - kTabH;
    float y = BeginList(top, bottom);
    const float y0 = y;
    // The player's own and CJ's from the ini, in one list by name.
    struct Entry {
        const Contact* c;
        int index;  // the player's: its index; CJ's: -2 - its index
    };
    std::vector<Entry> all;
    for (size_t i = 0; i < contacts.size(); ++i) all.push_back({&contacts[i], static_cast<int>(i)});
    for (size_t i = 0; i < builtIn.size(); ++i) all.push_back({&builtIn[i], -2 - static_cast<int>(i)});
    std::stable_sort(all.begin(), all.end(),
                     [](const Entry& a, const Entry& b) { return _stricmp(a.c->name.c_str(), b.c->name.c_str()) < 0; });
    int opened = -1, openedBuiltIn = -1;
    std::string openedNumber;
    for (const Entry& e : all) {
        if (!e.c->picture.empty() && y + 44.0f > top && y < bottom) {
            Icon(e.c->picture, ui::kScreenW - 46.0f, y + 6.0f, 32.0f, kWhite);
        }
        if (Row(y, 44.0f, e.c->name)) {
            openedNumber = e.c->number;
            if (e.index >= 0) opened = e.index;
            else openedBuiltIn = -2 - e.index;
        }
        y += 44.0f;
    }
    if (all.empty()) Empty("No Contacts");
    EndList(top, bottom, y - y0);
    ui::Flush();
    StatusBar();
    const bool full = contacts.size() >= phone_data::kMaxContacts;
    const int nav = Header(g.pickingForCompose ? "Choose" : "Contacts",
                           g.pickingForCompose ? "Cancel" : nullptr,
                           g.pickingForCompose || full ? nullptr : "New");
    if (!g.pickingForCompose) PhoneTabs();

    if (g.pickingForCompose) {
        if (nav == -1) {
            g.pickingForCompose = false;
            Go(Screen::Compose);
        } else if (!openedNumber.empty()) {
            g.pickingForCompose = false;
            g.composeTo = openedNumber;
            g.composeToFocus = false;
            Go(Screen::Compose);
        }
        return;
    }
    if (nav == 1) {
        g.contact = -1;
        g.editName.clear();
        g.editNumber.clear();
        g.editPicture.clear();
        g.editFocus = 0;
        g.editReturn = Screen::Contacts;
        g.builtIn = -1;
        Go(Screen::ContactEdit);
    } else if (opened >= 0 || openedBuiltIn >= 0) {
        g.contact = opened;
        g.builtIn = openedBuiltIn;
        Go(Screen::ContactView);
    }
}

void ContactViewScreen() {
    auto& contacts = phone_data::Get().contacts;
    const auto& builtIn = phone_data::Get().builtIn;
    const bool fromIni = g.builtIn >= 0 && g.builtIn < static_cast<int>(builtIn.size());
    if (!fromIni && (g.contact < 0 || g.contact >= static_cast<int>(contacts.size()))) {
        Go(Screen::Contacts);
        return;
    }
    const Contact c = fromIni ? builtIn[g.builtIn] : contacts[g.contact];
    Backdrop();
    const float top = kContentTop;
    Icon(c.picture.empty() ? "app_contacts" : c.picture, 14.0f, top + 14.0f, 64.0f, kWhite);
    ui::Label(90.0f, top + 22.0f, ui::Fit(c.name, 220.0f, F(24.0f)), F(24.0f, kWhite, sprite::Align::Left, 2));
    ui::Label(90.0f, top + 52.0f, phone_data::FormatNumber(c.number), F(17.0f, kMenuBlue));

    float y = top + 100.0f;
    if (MenuChoice(y, "Call")) { StartCall(c.number); return; }
    if (MenuChoice(y + 44.0f, "Text Message")) {
        g.threadNumber = c.number;
        g.draft.clear();
        Go(Screen::Thread);
        return;
    }
    ui::Flush();
    StatusBar();
    const int nav = Header("Contact", "Contacts", "Edit");
    if (nav == -1) {
        Go(Screen::Contacts);
    } else if (nav == 1) {
        g.editName = c.name;
        g.editNumber = c.number;
        g.editPicture = c.picture;
        g.editFocus = 0;
        g.editReturn = Screen::ContactView;
        g.deleteArmedAt = 0;
        Go(Screen::ContactEdit);
    }
}

void ContactEditScreen() {
    Backdrop();
    const float top = kContentTop;
    if (g.tab) g.editFocus = 1 - g.editFocus;
    ui::Label(14.0f, top + 16.0f, "Name", F(15.0f, kMenuBlue));
    bool tapped = false;
    TextBox(10.0f, top + 38.0f, 300.0f, 40.0f, g.editName, "First and last name", g.editFocus == 0,
            phone_data::kMaxNameLength, false, &tapped);
    if (tapped) g.editFocus = 0;
    ui::Label(14.0f, top + 94.0f, "Number", F(15.0f, kMenuBlue));
    TextBox(10.0f, top + 116.0f, 300.0f, 40.0f, g.editNumber, "Digits only", g.editFocus == 1,
            phone_data::kMaxNumberLength, true, &tapped);
    if (tapped) g.editFocus = 1;
    // The picture: tapped, the icons and photos to choose it from.
    const Rect pic{10.0f, top + 172.0f, 300.0f, 64.0f};
    ui::Fill(pic.x, pic.y, pic.w, pic.h, ui::Pressing(pic) ? 0xFF3B5B8A : 0xC8000000);
    ui::Frame(pic.x, pic.y, pic.w, pic.h, 1.5f, 0xFF5A5A5A);
    Icon(g.editPicture.empty() ? "app_contacts" : g.editPicture, pic.x + 8.0f, pic.y + 8.0f, 48.0f, kWhite);
    ui::Label(pic.x + 68.0f, pic.y + 13.0f, "Picture", F(17.0f));
    ui::Label(pic.x + 68.0f, pic.y + 35.0f, g.editPicture.empty() ? "None - tap to choose" : "Tap to change",
              F(14.0f, kGrey, sprite::Align::Left, 0));
    const bool choosePicture = ui::Tapped(pic);
    // Deleting is here, as the original iPhone has it, not one tap from the
    // contact itself - and it takes a second tap to go through.
    const bool fromIniHere = g.builtIn >= 0 && g.builtIn < static_cast<int>(phone_data::Get().builtIn.size());
    bool deleted = false;
    if (g.contact >= 0 || fromIniHere) {
        const ULONGLONG now = GetTickCount64();
        const bool armed = g.deleteArmedAt && now - g.deleteArmedAt < 3000;
        if (Button(20.0f, ui::kScreenH - 130.0f, 280.0f, 48.0f, kWastedRed,
                   armed ? "Tap Again to Delete" : "Delete Contact", 18.0f)) {
            if (!armed) {
                g.deleteArmedAt = now;
            } else {
                auto& contacts = phone_data::Get().contacts;
                if (fromIniHere) HideBuiltIn(g.builtIn);
                else if (g.contact < static_cast<int>(contacts.size())) contacts.erase(contacts.begin() + g.contact);
                g.builtIn = -1;
                g.contact = -1;
                g.deleteArmedAt = 0;
                Save();
                deleted = true;
            }
        }
    }
    if (deleted) {
        ui::Flush();
        Go(Screen::Contacts);
        return;
    }
    if (!g.notice.empty() && GetTickCount64() - g.noticeAt < 3000) {
        ui::Label(ui::kScreenW / 2, top + 252.0f, g.notice, F(15.0f, kWastedRed, sprite::Align::Centre));
    }
    ui::Flush();
    StatusBar();
    // One of CJ's from the ini, once edited, is one of the player's own.
    const bool fromIni = g.builtIn >= 0 && g.builtIn < static_cast<int>(phone_data::Get().builtIn.size());
    const bool isNew = g.contact < 0 && !fromIni;
    const int nav = Header(isNew ? "New Contact" : "Edit Contact", "Cancel", "Save");
    const bool save = nav == 1 || (g.enter && g.editFocus == 1);
    if (g.enter && g.editFocus == 0) g.editFocus = 1;
    if (nav == -1) {
        Go(g.editReturn);
    } else if (choosePicture) {
        Go(Screen::ContactIcon);
    } else if (save) {
        if (g.editName.empty() || g.editNumber.empty()) {
            Notice("A contact needs a name and a number.");
            return;
        }
        auto& contacts = phone_data::Get().contacts;
        if (isNew || fromIni) {
            if (contacts.size() >= phone_data::kMaxContacts) {
                Notice("Your phonebook is full.");
                return;
            }
            if (fromIni) HideBuiltIn(g.builtIn);
            g.builtIn = -1;
            contacts.push_back({g.editName, g.editNumber, g.editPicture});
        } else if (g.contact < static_cast<int>(contacts.size())) {
            contacts[g.contact] = {g.editName, g.editNumber, g.editPicture};
        }
        phone_data::SortContacts();
        g.contact = phone_data::FindContact(g.editNumber);
        Save();
        Go(g.editReturn == Screen::Keypad ? Screen::Keypad : Screen::ContactView);
    }
}

std::string GalleryFolder();
std::vector<std::string> GalleryFolders() { return {GalleryFolder()}; }

void ScanGallery();

// A contact's picture: none, one of the game's radar icons - its people
// first, then its places - or a photo from the Camera Roll.
const char* const kContactIcons[] = {
    "radar_CJ", "radar_SWEET", "radar_BIGSMOKE", "radar_RYDER", "radar_OGLOC", "radar_CESARVIAPANDO",
    "radar_CATALINAPINK", "radar_THETRUTH", "radar_WOOZIE", "radar_TORENO", "radar_MADDOG", "radar_ZERO",
    "radar_emmetGun", "radar_LocoSyndicate", "radar_girlfriend", "radar_triads", "radar_MCSTRAP",
    "radar_LOVEFIST", "radar_PHILS", "radar_salieri", "radar_ted", "radar_doherty", "radar_BIKERS",
    "radar_hobo", "radar_police", "radar_hostpital", "radar_fire", "radar_phone", "radar_house",
    "radar_cash", "radar_burgerShot", "radar_pizza", "radar_chicken", "radar_diner", "radar_gym",
    "radar_barbers", "radar_tattoo", "radar_ammugun", "radar_modGarage", "radar_spray", "radar_casino",
    "radar_StripClb", "radar_race", "radar_boxing", "radar_school", "radar_train", "radar_heli",
    "radar_Flag", "radar_qmark",
};

void ContactIconScreen() {
    Backdrop();
    if (GetTickCount64() - g.roll.scanned > 3000) {
        ScanGallery();
        g.roll.scanned = GetTickCount64();
    }
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom) + 10.0f;
    const float y0 = y;
    std::string picked;
    bool chose = false;
    // Five across, as the home screen's icons are spaced.
    const float cell = 60.0f, icon = 44.0f, left = (ui::kScreenW - 5 * cell) / 2;
    auto tile = [&](int i, float rowY, const std::string& spec) {
        const float x = left + (i % 5) * cell, ty = rowY + (i / 5) * cell;
        if (ty + cell < top || ty > bottom) return;
        const Rect r{x + 2.0f, ty + 2.0f, cell - 4.0f, cell - 4.0f};
        const bool on = spec == g.editPicture;
        if (on) ui::Fill(r.x, r.y, r.w, r.h, kMenuBlueBar);
        else if (ui::Pressing(r)) ui::Fill(r.x, r.y, r.w, r.h, 0x60FFFFFF);
        Icon(spec.empty() ? "app_contacts" : spec, x + (cell - icon) / 2, ty + (cell - icon) / 2, icon, kWhite);
        if (ui::Tapped(r)) {
            picked = spec;
            chose = true;
        }
    };
    ui::Label(14.0f, y, "Icons", F(15.0f, kMenuBlue));
    y += 24.0f;
    std::vector<std::string> icons{""};
    for (const char* name : kContactIcons) {
        if (sprite::Find(g.hud, name)) icons.push_back(std::string("hud:") + name);
    }
    for (size_t i = 0; i < icons.size(); ++i) tile(static_cast<int>(i), y, icons[i]);
    y += ((icons.size() + 4) / 5) * cell + 16.0f;
    ui::Label(14.0f, y, "Photos", F(15.0f, kMenuBlue));
    y += 24.0f;
    if (g.roll.files.empty()) {
        ui::Label(ui::kScreenW / 2, y + 10.0f, "No photos yet", F(15.0f, kDimGrey, sprite::Align::Centre));
        y += 44.0f;
    } else {
        for (size_t i = 0; i < g.roll.files.size(); ++i) tile(static_cast<int>(i), y, g.roll.files[i]);
        y += ((g.roll.files.size() + 4) / 5) * cell + 16.0f;
    }
    EndList(top, bottom, y - y0);
    ui::Flush();
    StatusBar();
    const int nav = Header("Picture", "Cancel");
    if (chose) g.editPicture = picked;
    if (chose || nav == -1) Go(Screen::ContactEdit);
}

// --- Text messages ----------------------------------------------------------

void MessagesScreen() {
    const auto& messages = phone_data::Get().messages;
    std::vector<std::string> order;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (std::find(order.begin(), order.end(), it->number) == order.end()) order.push_back(it->number);
    }
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom);
    const float y0 = y;
    std::string open, remove;
    for (const auto& number : order) {
        const Message* last = nullptr;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->number == number) { last = &*it; break; }
        }
        const Rect r{0, y, ui::kScreenW, 64.0f};
        if (ui::Pressing(r) && !g.editingMessages) ui::Fill(r.x, r.y, r.w, r.h, kMenuBlueBar);
        ui::Label(14.0f, y + 8.0f, ui::Fit(phone_data::NameFor(number), 190.0f, F(18.0f)), F(18.0f));
        if (last && !g.editingMessages) {
            ui::Label(ui::kScreenW - 12.0f, y + 10.0f, phone_data::FormatTime(last->at),
                      F(13.0f, kMenuBlue, sprite::Align::Right));
        }
        if (last) {
            ui::Label(14.0f, y + 36.0f, ui::Fit(last->text, 290.0f, F(14.0f, kGrey, sprite::Align::Left, 0)),
                      F(14.0f, kGrey, sprite::Align::Left, 0));
        }
        ui::Fill(10.0f, y + 63.0f, ui::kScreenW - 20.0f, 1.0f, kSeparator);
        if (g.editingMessages) {
            if (Button(ui::kScreenW - 84.0f, y + 12.0f, 72.0f, 30.0f, kWastedRed, "Delete", 14.0f)) remove = number;
        } else if (ui::Tapped(r)) {
            open = number;
        }
        y += 64.0f;
    }
    if (order.empty()) Empty("No Messages");
    EndList(top, bottom, y - y0);
    ui::Flush();
    StatusBar();
    const int nav = Header("Text", order.empty() ? nullptr : (g.editingMessages ? "Done" : "Edit"),
                           g.editingMessages ? nullptr : "g_compose");
    if (!remove.empty()) {
        auto& all = phone_data::Get().messages;
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [&](const Message& m) { return m.number == remove; }),
                  all.end());
        Save();
        if (order.size() == 1) g.editingMessages = false;
    } else if (nav == -1) {
        g.editingMessages = !g.editingMessages;
    } else if (nav == 1) {
        g.composeTo.clear();
        g.composeToFocus = true;
        g.draft.clear();
        Go(Screen::Compose);
    } else if (!open.empty()) {
        g.threadNumber = open;
        g.draft.clear();
        Go(Screen::Thread);
    }
}

// Send what is in the draft to `number`. The server delivers to a player with
// that number or to the SAN hotline; in single player only the hotline is
// there.
void Send(const std::string& number) {
    if (g.draft.empty() || number.empty()) return;
    if (!phone_data::Get().settings.poweredOn) return;
    const ULONGLONG now = GetTickCount64();
    const std::string& hotline = config::Get().hotline;
    if (number == hotline && g.hotlineSentAt && now - g.hotlineSentAt < 60000) {
        Notice("You must wait " + std::to_string(60 - (now - g.hotlineSentAt) / 1000) +
               " seconds before sending another message to the hotline.");
        return;
    }
    Message m;
    m.number = number;
    m.text = g.draft;
    m.outgoing = true;
    m.delivered = number == hotline && coverage::HasService();
    if (!coverage::HasService()) Notice("Message Failed - No Service");
    m.at = phone_data::Now();
    auto& messages = phone_data::Get().messages;
    messages.push_back(m);
    if (messages.size() > phone_data::kMaxMessages) messages.erase(messages.begin());
    if (number == hotline) g.hotlineSentAt = now;
    g.draft.clear();
    Save();
    PlayAlert(config::Get().sentTone, 200);
}

// The message bar along the bottom: a field and a Send button.
void Composer(const std::string& to, bool focused) {
    const float barTop = ui::kScreenH - 46.0f;
    ui::Fill(0, barTop, ui::kScreenW, 46.0f, kPanelDark);
    ui::Fill(0, barTop, ui::kScreenW, 2.0f, kBlack);
    bool tapped = false;
    TextBox(6.0f, barTop + 7.0f, 240.0f, 32.0f, g.draft, "Text message", focused,
            phone_data::kMaxMessageLength, false, &tapped);
    if (tapped) g.composeToFocus = false;
    if (Button(252.0f, barTop + 7.0f, 62.0f, 32.0f, kMoneyGreen, "Send", 15.0f) || (g.enter && focused)) {
        Send(to);
    }
}

// The conversation: what the player sent on the right in dark green, what
// came back on the left in black - help boxes, not balloons.
void Bubbles(float top, float bottom, const std::string& number) {
    const auto& messages = phone_data::Get().messages;
    float y = BeginList(top, bottom) + 10.0f;
    const float y0 = y;
    const Font f = F(15.0f, kWhite, sprite::Align::Left, 0);
    size_t count = 0;
    for (const Message& m : messages) count += m.number == number ? 1 : 0;
    if (g.bubbleThread != number) {
        g.bubbleThread = number;
        g.bubbleCount = count;
        g.bubbleAt = 0;
    } else if (count > g.bubbleCount) {
        g.bubbleCount = count;
        g.bubbleAt = GetTickCount64();
    } else {
        g.bubbleCount = count;
    }
    size_t index = 0;
    for (const Message& m : messages) {
        if (m.number != number) continue;
        ++index;
        const auto lines = sprite::Wrap(m.text.c_str(), 200.0f * ui::PixelsPerPoint(),
                                        sprite::TextStyle{sprite::Face::Subtitles, 15.0f * ui::PixelsPerPoint()});
        float textW = 0.0f;
        for (const auto& l : lines) textW = std::max(textW, ui::TextWidth(l, f));
        const float w = std::max(50.0f, textW + 24.0f);
        const float h = lines.size() * 19.0f + 16.0f;
        float x = m.outgoing ? ui::kScreenW - w - 10.0f : 10.0f;
        // The newest one slides in from its own side.
        if (index == count && g.bubbleAt) {
            const float off = (1.0f - Ease(Progress(g.bubbleAt, 260.0f))) * (w + 20.0f);
            x += m.outgoing ? off : -off;
        }
        ui::Fill(x, y, w, h, m.outgoing ? 0xD01E4A18 : kPanelDark);
        ui::Frame(x, y, w, h, 1.5f, kBlack);
        float ly = y + 8.0f;
        for (const auto& l : lines) {
            ui::Label(x + 12.0f, ly, l, f);
            ly += 19.0f;
        }
        y += h + 4.0f;
        if (m.outgoing && !m.delivered) {
            ui::Label(ui::kScreenW - 12.0f, y, "Not Delivered", F(12.0f, kWastedRed, sprite::Align::Right));
            y += 16.0f;
        }
        y += 6.0f;
    }
    // A conversation opens at its newest end, and stays there as messages
    // are added unless the player has scrolled back up.
    const float content = y - y0 + 10.0f;
    const float max = std::max(0.0f, content - (bottom - top));
    const float oldMax = g.scrollMax[g.screen];
    if (max > oldMax && Scroll() >= oldMax - 1.0f) Scroll() = max;
    EndList(top, bottom, content);
}

void ThreadScreen() {
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH - 46.0f;
    Bubbles(top, bottom, g.threadNumber);
    if (!g.notice.empty() && GetTickCount64() - g.noticeAt < 4000) {
        ui::Fill(0, bottom - 38.0f, ui::kScreenW, 38.0f, kPanelDark);
        const auto lines = sprite::Wrap(g.notice.c_str(), 300.0f * ui::PixelsPerPoint(),
                                        sprite::TextStyle{sprite::Face::Subtitles, 13.0f * ui::PixelsPerPoint()});
        for (size_t i = 0; i < lines.size() && i < 2; ++i) {
            ui::Label(ui::kScreenW / 2, bottom - 34.0f + i * 16.0f, lines[i], F(13.0f, kWhite, sprite::Align::Centre, 0));
        }
    }
    ui::Flush();
    Composer(g.threadNumber, true);
    ui::Flush();
    StatusBar();
    const int nav = Header(phone_data::NameFor(g.threadNumber), "Text", "Call");
    if (nav == -1) Go(Screen::Messages);
    else if (nav == 1) StartCall(g.threadNumber);
}

void ComposeScreen() {
    Backdrop();
    const float top = kContentTop;
    ui::Fill(0, top, ui::kScreenW, 46.0f, kPanel);
    ui::Label(12.0f, top + 14.0f, "To:", F(16.0f, kMenuBlue));
    bool tapped = false;
    TextBox(46.0f, top + 6.0f, 224.0f, 34.0f, g.composeTo, "Number", g.composeToFocus,
            phone_data::kMaxNumberLength, true, &tapped);
    if (tapped) g.composeToFocus = true;
    if (Button(276.0f, top + 6.0f, 38.0f, 34.0f, 0xFF3A3A3E, "+", 20.0f)) {
        g.pickingForCompose = true;
        Go(Screen::Contacts);
        return;
    }
    if (!g.composeTo.empty()) Bubbles(top + 46.0f, ui::kScreenH - 46.0f, g.composeTo);
    if (!g.notice.empty() && GetTickCount64() - g.noticeAt < 4000) {
        ui::Label(ui::kScreenW / 2, ui::kScreenH - 72.0f, ui::Fit(g.notice, 300.0f, F(13.0f)),
                  F(13.0f, kWastedRed, sprite::Align::Centre));
    }
    ui::Flush();
    if (g.enter && g.composeToFocus && !g.composeTo.empty()) {
        g.composeToFocus = false;
        g.enter = false;
    }
    const size_t before = phone_data::Get().messages.size();
    Composer(g.composeTo, !g.composeToFocus);
    if (phone_data::Get().messages.size() != before) {
        g.threadNumber = g.composeTo;
        Go(Screen::Thread);
        return;
    }
    ui::Flush();
    StatusBar();
    if (Header("New Message", nullptr, "Cancel") == 1) Go(Screen::Messages);
}

// --- Photos -----------------------------------------------------------------

void PhotosScreen() {
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom);
    if (Row(y, 48.0f, "Camera Roll", "Photos from the camera")) {
        EndList(top, bottom, 0.0f);
        g.roll.scanned = 0;
        Go(Screen::CameraRoll);
        return;
    }
    ui::Label(14.0f, y + 58.0f, "Wallpapers", F(15.0f, kMenuBlue));
    y += 82.0f;
    const float y0 = y - 82.0f;
    const float tw = 98.0f, th = 147.0f, gap = 6.5f;
    for (int i = 0; i < 27; ++i) {
        const float x = gap + (i % 3) * (tw + gap);
        const float ty = y + (i / 3) * (th + gap);
        const Rect r{x, ty, tw, th};
        ui::Image(("wall_" + std::to_string(i)).c_str(), x, ty, tw, th, ui::Pressing(r) ? 0xFF909090 : kWhite);
        ui::Frame(x, ty, tw, th, 2.0f, i == phone_data::Get().settings.wallpaper ? kMenuBlue : kBlack);
        if (ui::Tapped(r)) {
            g.photo = i;
            Go(Screen::PhotoView);
        }
    }
    EndList(top, bottom, 9 * (th + gap) + 12.0f + (y - y0));
    ui::Flush();
    StatusBar();
    Header("Photos");
}

void PhotoViewScreen() {
    ui::Image(("wall_" + std::to_string(g.photo)).c_str(), 0, 0, ui::kScreenW, ui::kScreenH);
    StatusBar();
    ui::Fill(0, kStatusH, ui::kScreenW, 40.0f, kPanel);
    ui::Label(ui::kScreenW / 2, kStatusH + 10.0f, kWallpaperNames[g.photo], F(18.0f, kWhite, sprite::Align::Centre));
    const float barTop = ui::kScreenH - 66.0f;
    ui::Fill(0, barTop, ui::kScreenW, 66.0f, kPanel);
    if (Button(12.0f, barTop + 12.0f, 140.0f, 42.0f, 0xFF3A3A3E, "Cancel", 17.0f)) {
        Go(Screen::Photos);
    } else if (Button(168.0f, barTop + 12.0f, 140.0f, 42.0f, kMoneyGreen, "Set Wallpaper", 16.0f)) {
        phone_data::Get().settings.wallpaper = g.photo;
        Save();
        Go(Screen::Home);
    }
}

// --- Clock ------------------------------------------------------------------

void ClockTabs() {
    static const Tab kTabs[] = {{"g_world", "World Clock"}, {"g_alarm", "Alarm"}, {"g_stopwatch", "Stopwatch"},
                                {"g_timer", "Timer"}};
    const Screen screens[] = {Screen::Clock, Screen::Alarm, Screen::Stopwatch, Screen::Timer};
    int on = 0;
    for (int i = 0; i < 4; ++i) {
        if (g.screen == screens[i]) on = i;
    }
    const int tapped = TabBar(kTabs, 4, on);
    if (tapped >= 0) Go(screens[tapped]);
}

// The time in San Andreas, on the game's clock.
void ClockScreen() {
    Backdrop();
    const phone_data::Stamp now = phone_data::Now();
    const float cx = ui::kScreenW / 2, cy = 190.0f, r = 104.0f;
    ui::Image("clock_face", cx - r, cy - r, 2 * r, 2 * r);
    const float pi = 3.14159265f;
    const float minutes = static_cast<float>(now.minute);
    const float hours = static_cast<float>(now.hour % 12) + minutes / 60.0f;
    ui::Needle(cx, cy, hours / 12.0f * 2 * pi, r * 0.5f, 10.0f, 8.0f, kBlack);
    ui::Needle(cx, cy, minutes / 60.0f * 2 * pi, r * 0.78f, 12.0f, 5.0f, kBlack);
    ui::Fill(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, kBlack);
    ui::Label(cx, cy + r + 12.0f, "San Andreas", F(18.0f, kMenuBlue, sprite::Align::Centre));
    ui::Label(cx, cy + r + 36.0f, phone_data::DayName(now.day) + "  " + phone_data::FormatTime(now),
              F(18.0f, kWhite, sprite::Align::Centre));
    ui::Flush();
    StatusBar();
    Header("World Clock");
    ClockTabs();
}

// The alarm: its time, set an hour or a minute at a time, and on or off.
void AlarmScreenSet() {
    Backdrop();
    auto& store = phone_data::Get().store;
    int alarm = AlarmMinutes();
    char at[8];
    snprintf(at, sizeof at, "%02d:%02d", alarm / 60, alarm % 60);
    ui::Label(ui::kScreenW / 2, kContentTop + 30.0f, at, Pricedown(64.0f));
    ui::Label(ui::kScreenW / 2, kContentTop + 104.0f, "Every day", F(14.0f, kGrey, sprite::Align::Centre, 0));
    int step = 0;
    const float by = kContentTop + 136.0f;
    if (Button(20.0f, by, 64.0f, 40.0f, 0xFF3A3A3E, "-1 h", 15.0f)) step = -60;
    if (Button(92.0f, by, 64.0f, 40.0f, 0xFF3A3A3E, "+1 h", 15.0f)) step = 60;
    if (Button(164.0f, by, 64.0f, 40.0f, 0xFF3A3A3E, "-5 m", 15.0f)) step = -5;
    if (Button(236.0f, by, 64.0f, 40.0f, 0xFF3A3A3E, "+5 m", 15.0f)) step = 5;
    if (step) {
        alarm = (alarm + step + 24 * 60) % (24 * 60);
        store["alarm"] = std::to_string(alarm);
        Save();
    }
    const bool on = AlarmOn();
    if (SettingRow(by + 64.0f, "g_alarm", "Alarm", on ? "On" : "Off", true, true)) {
        store["alarm.on"] = on ? "0" : "1";
        Save();
    }
    if (SettingRow(by + 122.0f, "g_sounds", "Sound", AlarmTone().name, true, true)) {
        g.toneKind = 2;
        g.toneBack = Screen::Alarm;
        Go(Screen::Tones);
    }
    ui::Flush();
    StatusBar();
    Header("Alarm");
    ClockTabs();
}

std::string Elapsed(ULONGLONG ms, bool tenths) {
    char buf[32];
    const ULONGLONG s = ms / 1000;
    if (tenths) snprintf(buf, sizeof buf, "%02llu:%02llu.%llu", s / 60, s % 60, (ms / 100) % 10);
    else snprintf(buf, sizeof buf, "%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    return buf;
}

// The stopwatch runs on the real clock, as a stopwatch does.
void StopwatchScreen() {
    Backdrop();
    auto& w = g.stopwatch;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG shown = w.total + (w.running ? now - w.since : 0);
    ui::Label(ui::kScreenW / 2, kContentTop + 30.0f, Elapsed(shown, true), Pricedown(56.0f));
    const float by = kContentTop + 110.0f;
    if (Button(20.0f, by, 130.0f, 48.0f, w.running ? kWastedRed : kMoneyGreen, w.running ? "Stop" : "Start", 19.0f)) {
        if (w.running) w.total += now - w.since;
        else w.since = now;
        w.running = !w.running;
    }
    if (Button(170.0f, by, 130.0f, 48.0f, 0xFF3A3A3E, w.running ? "Lap" : "Reset", 19.0f)) {
        if (w.running) w.laps.insert(w.laps.begin(), shown);
        else w = {};
    }
    float y = by + 64.0f;
    for (size_t i = 0; i < w.laps.size() && y < ui::kScreenH - kTabH - 30.0f; ++i, y += 30.0f) {
        ui::Label(24.0f, y, "Lap " + std::to_string(w.laps.size() - i), F(16.0f));
        ui::Label(ui::kScreenW - 24.0f, y, Elapsed(w.laps[i], true), F(16.0f, kMenuBlue, sprite::Align::Right));
        ui::Fill(14.0f, y + 25.0f, ui::kScreenW - 28.0f, 1.0f, kSeparator);
    }
    ui::Flush();
    StatusBar();
    Header("Stopwatch");
    ClockTabs();
}

// The timer counts down on the real clock and rings with the ringtone.
void TimerScreen() {
    Backdrop();
    auto& t = g.timer;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG left = t.running ? (t.end > now ? t.end - now : 0) : t.length;
    ui::Label(ui::kScreenW / 2, kContentTop + 30.0f, Elapsed(left, false), Pricedown(56.0f));
    if (!t.running) {
        const float by = kContentTop + 110.0f;
        const struct { const char* label; LONGLONG ms; } steps[4] = {
            {"-1 m", -60000}, {"+1 m", 60000}, {"-10 s", -10000}, {"+10 s", 10000}};
        for (int i = 0; i < 4; ++i) {
            if (Button(20.0f + i * 72.0f, by, 64.0f, 40.0f, 0xFF3A3A3E, steps[i].label, 15.0f)) {
                t.length = static_cast<ULONGLONG>(std::clamp<LONGLONG>(static_cast<LONGLONG>(t.length) + steps[i].ms,
                                                                       0, 24LL * 3600 * 1000 - 1000));
            }
        }
    }
    const float sy = kContentTop + 170.0f;
    if (Button(20.0f, sy, 280.0f, 48.0f, t.running ? kWastedRed : kMoneyGreen, t.running ? "Cancel" : "Start",
               19.0f)) {
        if (t.running) {
            t.running = false;
        } else if (t.length > 0) {
            t.running = true;
            t.end = now + t.length;
        }
    }
    if (SettingRow(sy + 64.0f, "g_sounds", "When Timer Ends", AlarmTone().name, true, true)) {
        g.toneKind = 2;
        g.toneBack = Screen::Timer;
        Go(Screen::Tones);
    }
    ui::Flush();
    StatusBar();
    Header("Timer");
    ClockTabs();
}

// --- Games ------------------------------------------------------------------

struct Machine {
    const char* name;
    const char* label;  // under its icon
    const char* icon;
    std::unique_ptr<games::Game> (*make)();
};

const Machine kMachines[] = {
    {"Duality", "Duality", "game_duality", &games::MakeDuality},
    {"Let's Get Ready To Bumble", "Bumble", "game_bumble", &games::MakeBumble},
    {"They Crawled From Uranus", "Uranus", "game_uranus", &games::MakeUranus},
    {"Go Go Space Monkey", "Space Monkey", "game_spacemonkey", &games::MakeSpaceMonkey},
};

void StartGame(const Machine& m) {
    g.game = m.make();
    g.gameLast = 0;
    logfile::Line("phone: playing %s", m.name);
}

// The machines as icons, laid out as the home screen's apps are.
void GamesScreen() {
    Backdrop();
    const float cell = ui::kScreenW / 4, size = 57.0f;
    const float top = kContentTop + 16.0f;
    int i = 0;
    for (const auto& m : kMachines) {
        const float cx = cell * (i % 4) + cell / 2, y = top + (i / 4) * 90.0f;
        const Rect r{cx - 38.0f, y - 4.0f, 76.0f, size + 22.0f};
        Icon(m.icon, cx - size / 2, y, size, ui::Pressing(r) ? 0xFF909090 : kWhite);
        ui::Label(cx, y + size + 3.0f, ui::Fit(m.label, 78.0f, F(12.0f)), F(12.0f, kWhite, sprite::Align::Centre));
        if (ui::Tapped(r)) StartGame(m);
        ++i;
    }
    ui::Flush();
    StatusBar();
    Header("Games");
}

// A machine running on the whole screen. The games are laid out for the
// upright glass, so their 448 x 672 canvas fills it exactly.
void PlayScreen() {
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, kBlack);
    ui::Flush();
    const ULONGLONG now = GetTickCount64();
    const float dt = g.gameLast ? static_cast<float>(std::min<ULONGLONG>(now - g.gameLast, 100)) : 20.0f;
    g.gameLast = now;

    const float l = ui::ToPixelX(0), t = ui::ToPixelY(0);
    const float r = ui::ToPixelX(ui::kScreenW), b = ui::ToPixelY(ui::kScreenH);
    arcade::Pad pad{};
    if (g.focused) {
        pad = arcade::ReadPad();
        // A click fires only on the glass, so pressing the home button does
        // not - judged where the touch screen puts the cursor, which on the
        // 3D phone is mapped onto its turned glass, not the game's screen.
        const float cx = ui::CursorX(), cy = ui::CursorY();
        if (cx < 0.0f || cx >= ui::kScreenW || cy < 0.0f || cy >= ui::kScreenH) pad.circle = false;
    }
    sprite::BeginScissor(l, t, r, b);
    arcade::SetViewport(l, t, r - l, b - t);
    const bool running = g.game->Frame(pad, static_cast<unsigned>(now), dt);
    sprite::Flush();
    sprite::EndScissor();
    if (!running) g.game.reset();
}

// --- Internet ---------------------------------------------------------------
//
// GTA IV's websites, as its in-game browser shows them, on the 2007 phone's
// browser: the whole page fitted across the screen, dragged to scroll, a
// double tap to zoom in on a column and out again, and the links tapped.

constexpr float kToolbarH = 44.0f;
constexpr float kPageTop = kStatusH + kHeaderH;
constexpr float kPageBottom = ui::kScreenH - kToolbarH;
constexpr ULONGLONG kDoubleTapMs = 350;

std::string Address(int page) {
    if (page == web::kNoPage) return {};
    const web::Page& p = web::Info(page);
    return p.name == "index" ? p.site : p.site + "/" + p.name + ".html";
}

void Toast(const std::string& text) {
    g.browser.toast = text;
    g.browser.toastAt = GetTickCount64();
}

// A page comes down as the first iPhone's browser got it over EDGE, at a
// few tens of kilobytes a second: a moment connecting with the page blank,
// then the page arriving from the top down in uneven pieces as the data
// does, the blue bar in the address field creeping along with it. A big
// page takes longer than a small one; one already seen this session comes
// from the cache, much faster.
void ShowPage(int page) {
    auto& b = g.browser;
    b.page = page;
    b.scrollX = b.scrollY = 0.0f;
    b.zoomed = false;
    b.loadingSince = GetTickCount64();
    b.address = Address(page);
    static std::mt19937 rng(GetTickCount());
    const bool cached = std::find(b.visited.begin(), b.visited.end(), page) != b.visited.end();
    b.loadMs = 0;
    for (ULONGLONG& at : b.chunkAt) at = 0;
    b.loaded = 0.0f;
    b.tick = GetTickCount64();
    b.stalled = 0.0f;
    b.failed = false;
    if (page != web::kNoPage && config::Get().features.slowPages) {
        const web::Page& p = web::Info(page);
        // About a second and a half, plus a second for every 800 000 pixels
        // of page, up to eight; a quarter of that from the cache.
        const float size = static_cast<float>(p.width) * static_cast<float>(p.height);
        float ms = std::clamp(1500.0f + size / 800.0f, 1800.0f, 8000.0f);
        ms *= std::uniform_real_distribution<float>(0.8f, 1.25f)(rng);
        if (cached) ms *= 0.25f;
        b.loadMs = static_cast<ULONGLONG>(ms);
        // Connecting first (the first eighth, or less from the cache), then
        // the ten pieces at uneven times after.
        const float connect = cached ? 0.05f : 0.14f;
        float times[10];
        for (float& t : times) t = std::uniform_real_distribution<float>(connect, 1.0f)(rng);
        std::sort(times, times + 10);
        times[9] = 1.0f;
        for (int i = 0; i < 10; ++i) b.chunkAt[i] = static_cast<ULONGLONG>(times[i] * ms);
        if (!cached) b.visited.push_back(page);
    }
    logfile::Line("phone: browsing %s (%llu ms%s)", b.address.c_str(), b.loadMs, cached ? ", cached" : "");
}

// How much of the page has come down: the bar's progress, and the share of
// the page (from the top) that can be seen.
// The page coming down, a frame on: at EDGE's speed for the signal there
// is - a fifth of it on one bar - and not at all with none, when after ten
// seconds the browser gives up.
void AdvanceLoad() {
    auto& b = g.browser;
    const ULONGLONG now = GetTickCount64();
    const float dt = static_cast<float>(std::min<ULONGLONG>(now - b.tick, 250));
    b.tick = now;
    if (!b.loadMs || b.failed || b.loaded >= b.loadMs) return;
    const float rate = coverage::HasService() ? 0.12f + 0.88f * coverage::Quality() : 0.0f;
    b.loaded += dt * rate;
    b.stalled = rate > 0.0f ? 0.0f : b.stalled + dt;
    if (b.stalled > 10000.0f) {
        b.failed = true;
        logfile::Line("phone: %s could not load - no service", b.address.c_str());
    }
}
float LoadProgress() {
    const auto& b = g.browser;
    if (!b.loadMs) return 1.0f;
    return std::min(1.0f, b.loaded / b.loadMs);
}
float PageArrived() {
    const auto& b = g.browser;
    const ULONGLONG since = static_cast<ULONGLONG>(b.loaded);
    int pieces = 0;
    for (ULONGLONG at : b.chunkAt) pieces += since >= at ? 1 : 0;
    return pieces / 10.0f;
}

void Navigate(int page) {
    auto& b = g.browser;
    if (page == web::kNoPage) return;
    if (page == b.page) {
        // The same page again: only to try once more when it failed.
        if (b.failed) ShowPage(page);
        return;
    }
    if (b.page != web::kNoPage) b.back.push_back(b.page);
    b.forward.clear();
    ShowPage(page);
}

void Back() {
    auto& b = g.browser;
    if (b.back.empty()) return;
    b.forward.push_back(b.page);
    const int page = b.back.back();
    b.back.pop_back();
    ShowPage(page);
}

void Forward() {
    auto& b = g.browser;
    if (b.forward.empty()) return;
    b.back.push_back(b.page);
    const int page = b.forward.back();
    b.forward.pop_back();
    ShowPage(page);
}

// Eyefind, the start page GTA IV's browser opens on.
void OpenLink(const char* url);

int HomePage() {
    const int eyefind = web::Find("www.eyefind.info", "index");
    if (eyefind != web::kNoPage) return eyefind;
    const int sprp = web::Find("www.sp-rp.com", "index");
    if (sprp != web::kNoPage) return sprp;
    return web::Sites().empty() ? web::kNoPage : web::Sites()[0].home;
}

// What was typed into the address bar, as a page: "burgershot.net",
// "www.burgershot.net/menu.html", with or without the http://.
int Resolve(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    for (const std::string prefix : {"http://", "https://"}) {
        if (text.compare(0, prefix.size(), prefix) == 0) text.erase(0, prefix.size());
    }
    while (!text.empty() && (text.back() == '/' || text.back() == ' ')) text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    const size_t slash = text.find('/');
    const std::string site = text.substr(0, slash);
    std::string name = slash == std::string::npos ? "index" : text.substr(slash + 1);
    for (const std::string ext : {".html", ".htm"}) {
        if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            name.erase(name.size() - ext.size());
        }
    }
    for (const auto& s : web::Sites()) {
        if (s.name == site || s.name == "www." + site) {
            return name == "index" ? s.home : web::Find(s.name, name);
        }
    }
    return web::kNoPage;
}

// Points per page pixel: the page fitted across the screen, or twice that.
float PageScale(const web::Page& p) {
    const float fit = ui::kScreenW / static_cast<float>(std::max(p.width, 1));
    return g.browser.zoomed ? fit * 2.0f : fit;
}

void ClampScroll(const web::Page& p, float scale) {
    auto& b = g.browser;
    const float viewW = ui::kScreenW / scale, viewH = (kPageBottom - kPageTop) / scale;
    b.scrollX = std::clamp(b.scrollX, 0.0f, std::max(0.0f, p.width - viewW));
    b.scrollY = std::clamp(b.scrollY, 0.0f, std::max(0.0f, p.height - viewH));
}

void PageView() {
    auto& b = g.browser;
    ui::Fill(0, kPageTop, ui::kScreenW, kPageBottom - kPageTop, 0xFF4A4A4A);
    if (b.page == web::kNoPage) return;
    AdvanceLoad();
    if (b.failed) {
        // As the browser says it when there is no network.
        ui::Fill(0, kPageTop, ui::kScreenW, kPageBottom - kPageTop, kWhite);
        ui::Label(ui::kScreenW / 2, kPageTop + 120.0f, "Cannot Open Page", F(20.0f, 0xFF303030, sprite::Align::Centre, 0));
        ui::Label(ui::kScreenW / 2, kPageTop + 152.0f, "The phone is not connected", F(14.0f, 0xFF606060, sprite::Align::Centre, 0));
        ui::Label(ui::kScreenW / 2, kPageTop + 172.0f, "to the Internet.", F(14.0f, 0xFF606060, sprite::Align::Centre, 0));
        return;
    }
    const web::Page& p = web::Info(b.page);
    const bool decoded = web::Load(b.page);
    const float arrived = PageArrived();
    const bool ready = decoded && arrived >= 1.0f;
    float scale = PageScale(p);

    // Dragging and the wheel scroll. A tap on a link follows it; a double
    // tap anywhere else zooms about where it was.
    const Rect view{0, kPageTop, ui::kScreenW, kPageBottom - kPageTop};
    b.scrollY += ui::ScrollDelta() / scale;
    b.scrollX += ui::ScrollDeltaX() / scale;
    float tx = 0.0f, ty = 0.0f;
    if (ready && !b.typing && ui::TappedAt(view, tx, ty)) {
        const float px = b.scrollX + tx / scale, py = b.scrollY + (ty - kPageTop) / scale;
        for (const auto& l : web::Links(b.page)) {
            if (px >= l.x && px < l.x + l.w && py >= l.y && py < l.y + l.h) {
                if (l.target == web::kBack) Back();
                else if (l.target == web::kExternal) {
                    Toast("Opening in your browser...");
                    OpenLink(l.url.c_str());
                }
                else if (l.target == web::kNoPage) Toast("This page can't be opened.");
                else Navigate(l.target);
                return;
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now - b.lastTap < kDoubleTapMs && std::hypot(tx - b.lastTapX, ty - b.lastTapY) < 30.0f) {
            b.zoomed = !b.zoomed;
            scale = PageScale(p);
            b.scrollX = px - tx / scale;
            b.scrollY = py - (ty - kPageTop) / scale;
            b.lastTap = 0;
        } else {
            b.lastTap = now;
            b.lastTapX = tx;
            b.lastTapY = ty;
        }
    }
    ClampScroll(p, scale);

    // Still connecting: the page blank, as the browser shows it before the
    // first of it arrives.
    const float pageW = std::min(ui::kScreenW, (p.width - b.scrollX) * scale);
    const float pageH = std::min(kPageBottom - kPageTop, (p.height - b.scrollY) * scale);
    ui::Fill(0, kPageTop, pageW, pageH, kWhite);
    if (!decoded || arrived <= 0.0f) {
        ui::Flush();
        return;
    }
    // The page, cut to the view, to its own edges, and to as far down as it
    // has arrived.
    const float ppp = ui::PixelsPerPoint();
    const float left = ui::ToPixelX(-b.scrollX * scale), top = ui::ToPixelY(kPageTop - b.scrollY * scale);
    const float viewL = ui::ToPixelX(0), viewT = ui::ToPixelY(kPageTop);
    const float viewR = ui::ToPixelX(ui::kScreenW), viewB = ui::ToPixelY(kPageBottom);
    const float pageR = std::min(viewR, left + p.width * scale * ppp);
    const float pageB = std::min(viewB, top + p.height * arrived * scale * ppp);
    ui::Flush();
    if (pageB > viewT) {
        sprite::BeginScissor(viewL, viewT, pageR, pageB);
        web::Draw(left, top, scale * ppp, viewT, viewB);
        sprite::EndScissor();
    }

    // A thin scroll indicator down the right while the page is dragged.
    const float viewH = (kPageBottom - kPageTop) / scale;
    if (p.height > viewH && ui::Dragging()) {
        const float track = kPageBottom - kPageTop - 6.0f;
        const float h = std::max(24.0f, track * viewH / p.height);
        const float y = kPageTop + 3.0f + (track - h) * (b.scrollY / (p.height - viewH));
        ui::Fill(ui::kScreenW - 6.0f, y, 3.0f, h, 0x96000000);
    }
}

// The address bar: the page's title over its address, which can be tapped
// and typed over.
void AddressBar() {
    auto& b = g.browser;
    ui::Fill(0, kStatusH, ui::kScreenW, kHeaderH, kPanelDark);
    ui::Fill(0, kPageTop - 2.0f, ui::kScreenW, 2.0f, kBlack);
    if (b.page != web::kNoPage) {
        const web::Page& p = web::Info(b.page);
        const Font tf = F(11.0f, kGrey, sprite::Align::Centre, 0);
        // The title only once the page has told the browser it.
        const std::string title = PageArrived() > 0.0f ? (p.title.empty() ? Address(b.page) : p.title) : "Loading...";
        ui::Label(ui::kScreenW / 2, kStatusH + 2.0f, ui::Fit(title, 280.0f, tf), tf);
    }
    const float fx = 8.0f, fy = kStatusH + 16.0f, fw = ui::kScreenW - 16.0f, fh = 24.0f;
    bool tapped = false;
    if (b.typing) {
        TextBox(fx, fy, fw, fh, b.address, "", true, 120, false, &tapped);
        if (g.enter) {
            b.typing = false;
            const int page = Resolve(b.address);
            if (page != web::kNoPage) {
                Navigate(page);
            } else {
                Toast("Can't find the server \"" + b.address + "\".");
                b.address = Address(b.page);
            }
        }
    } else {
        std::string shown = Address(b.page);
        TextBox(fx, fy, fw, fh, shown, "Go to this address", false, 120, false, &tapped);
        if (tapped) {
            b.typing = true;
            b.address.clear();
        }
    }
    // The load bar, filling the address field the way the 2007 browser's
    // does: a little way in at once, then along with the page.
    if (b.page != web::kNoPage && (LoadProgress() < 1.0f || !web::Load(b.page))) {
        const float t = std::max(LoadProgress() * 0.85f + PageArrived() * 0.15f, 0.08f);
        ui::Fill(fx + 1.5f, fy + fh - 4.0f, (fw - 3.0f) * std::min(t, 0.98f), 2.5f, kMenuBlue);
    }
}

bool ToolButton(float x, float w, const char* icon, bool enabled) {
    const Rect r{x, kPageBottom, w, kToolbarH};
    const bool hot = enabled && ui::Pressing(r);
    ui::Image(icon, x + (w - 28.0f) / 2, kPageBottom + 8.0f, 28.0f, 28.0f,
              !enabled ? 0x50FFFFFF : hot ? 0xFF909090 : kWhite);
    return enabled && ui::Tapped(r);
}

void Toolbar() {
    auto& b = g.browser;
    ui::Fill(0, kPageBottom, ui::kScreenW, kToolbarH, kPanelDark);
    ui::Fill(0, kPageBottom, ui::kScreenW, 2.0f, kBlack);
    const float w = ui::kScreenW / 4;
    if (ToolButton(0, w, "g_back", !b.back.empty())) Back();
    if (ToolButton(w, w, "g_forward", !b.forward.empty())) Forward();
    if (ToolButton(w * 2, w, "g_home", true)) Navigate(HomePage());
    if (ToolButton(w * 3, w, "g_book", true)) Go(Screen::Sites);
}

void InternetScreen() {
    auto& b = g.browser;
    if (!web::Available()) {
        Backdrop();
        // As the browser says it with no connection. (Setting the pages up
        // is in the README.)
        ui::Label(ui::kScreenW / 2, kContentTop + 90.0f, "Cannot Open Page", F(20.0f, kWhite, sprite::Align::Centre));
        ui::Label(ui::kScreenW / 2, kContentTop + 124.0f, "The phone is not connected", F(14.0f, kGrey, sprite::Align::Centre, 0));
        ui::Label(ui::kScreenW / 2, kContentTop + 144.0f, "to the Internet.", F(14.0f, kGrey, sprite::Align::Centre, 0));
        ui::Flush();
        StatusBar();
        Header("Internet");
        return;
    }
    if (b.page == web::kNoPage) ShowPage(HomePage());
    PageView();
    ui::Flush();
    AddressBar();
    Toolbar();
    if (!b.toast.empty() && GetTickCount64() - b.toastAt < 2500) {
        const Font f = F(14.0f, kWhite, sprite::Align::Centre, 0);
        const std::string text = ui::Fit(b.toast, 280.0f, f);
        const float w = ui::TextWidth(text, f) + 24.0f;
        ui::Fill((ui::kScreenW - w) / 2, kPageBottom - 44.0f, w, 30.0f, 0xDC000000);
        ui::Label(ui::kScreenW / 2, kPageBottom - 37.0f, text, f);
    }
    ui::Flush();
    StatusBar();
    // A tap off the address bar while typing gives the typing up.
    if (b.typing && ui::AnyTap()) {
        b.typing = false;
        b.address = Address(b.page);
    }
}

void SitesScreen() {
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    const float rowH = 52.0f;
    float y = BeginList(top, bottom);
    int open = web::kNoPage;
    for (const auto& site : web::Sites()) {
        if (y + rowH > top && y < bottom) {
            const Rect r{0, y, ui::kScreenW, rowH};
            if (ui::Pressing(r)) ui::Fill(r.x, r.y, r.w, r.h, kMenuBlueBar);
            const std::string title = site.title.empty() ? site.name : site.title;
            ui::Label(14.0f, y + 7.0f, ui::Fit(title, 292.0f, F(17.0f)), F(17.0f));
            ui::Label(14.0f, y + 29.0f, site.name, F(13.0f, kMenuBlue, sprite::Align::Left, 0));
            ui::Fill(10.0f, y + rowH - 1.0f, ui::kScreenW - 20.0f, 1.0f, kSeparator);
            if (ui::Tapped(r)) open = site.home;
        }
        y += rowH;
    }
    EndList(top, bottom, web::Sites().size() * rowH);
    ui::Flush();
    StatusBar();
    if (open != web::kNoPage) {
        Navigate(open);
        Go(Screen::Internet);
        return;
    }
    if (Header("Sites", "Internet") == -1) Go(Screen::Internet);
}

// --- Maps -------------------------------------------------------------------
//
// valkyrie-radar's own 3D world - the website's baked map tiles - seen from
// above, anywhere: drag to move, the wheel to zoom, 3D to lean the view,
// the arrow to go back to CJ. The radar renders it (ValkyrieRadarPhoneMap)
// and says where things fall on it; the phone draws it and CJ's arrow.

using PhoneMapFn = IDirect3DTexture9*(__cdecl*)(const float*, unsigned, unsigned);
using PhoneMapProjectFn = BOOL(__cdecl*)(const float*, float*);

struct Radar {
    PhoneMapFn render = nullptr;
    PhoneMapProjectFn project = nullptr;
    bool looked = false;
};

}  // namespace
}  // namespace phone
// The radar this ASI carries (valkyrie-radar/src/radar3d.cpp).
extern "C" IDirect3DTexture9* __cdecl ValkyrieRadarPhoneMap(const float* view, unsigned w, unsigned h);
extern "C" BOOL __cdecl ValkyrieRadarPhoneMapProject(const float* world, float* out);
namespace phone {
namespace {

// The radar valkyrie-radar.asi runs when it is installed too, else the one
// the phone carries, once it is up.
Radar& RadarApi() {
    static Radar r;
    if (!r.looked || !r.render) {
        HMODULE m = GetModuleHandleA("valkyrie-radar.asi");
        if (m && GetProcAddress(m, "ValkyrieRadarPhoneMap")) {
            r.looked = true;
            r.render = reinterpret_cast<PhoneMapFn>(GetProcAddress(m, "ValkyrieRadarPhoneMap"));
            r.project = reinterpret_cast<PhoneMapProjectFn>(GetProcAddress(m, "ValkyrieRadarPhoneMapProject"));
        } else if (radar3d::Running()) {
            r.looked = true;
            r.render = &ValkyrieRadarPhoneMap;
            r.project = &ValkyrieRadarPhoneMapProject;
        }
    }
    return r;
}

// A Direct3D texture (not one of the game's own) drawn into a screen
// rectangle, with the device put back exactly as it was.
void DrawDeviceTexture(IDirect3DTexture9* tex, float x0, float y0, float x1, float y1) {
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    if (!device || !tex) return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) return;
    saved->Capture();
    struct V {
        float x, y, z, rhw, u, v;
    } quad[4] = {{x0 - 0.5f, y0 - 0.5f, 0, 1, 0, 0}, {x1 - 0.5f, y0 - 0.5f, 0, 1, 1, 0},
                 {x0 - 0.5f, y1 - 0.5f, 0, 1, 0, 1}, {x1 - 0.5f, y1 - 0.5f, 0, 1, 1, 1}};
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetTexture(0, tex);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    saved->Apply();
    saved->Release();
}

bool PlayerPlace(float& x, float& y, float& z, float& heading) {
    const uintptr_t ped = PlayerPed();
    if (!ped) return false;
    const uintptr_t m = *reinterpret_cast<const uintptr_t*>(ped + 0x14);  // CPlaceable's matrix
    if (!m) return false;
    const float* f = reinterpret_cast<const float*>(m);
    x = f[12];
    y = f[13];
    z = f[14];
    heading = std::atan2(-f[4], f[5]);
    return true;
}

// As far out as the map zooms: about as much as the radar draws around its
// middle (kPhoneReach in radar3d.cpp), so zooming out never asks it for
// more of the world than it can hold.
constexpr float kMapFarthest = 1800.0f;

void MapsScreen() {
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, 0xFFAECBD1);
    auto& m = g.map;
    const float top = kContentTop, bottom = ui::kScreenH - 48.0f;
    float px = 0, py = 0, pz = 0, ph = 0;
    const bool player = PlayerPlace(px, py, pz, ph);
    if (!m.placed && player) {
        m.placed = true;
        m.x = px;
        m.y = py;
        m.z = pz;
    }
    Radar& radar = RadarApi();
    if (!radar.render) {
        Backdrop();
        StatusBar();
        Header("Maps");
        // The map is the phone's own, drawn from the tiles: missing, they are
        // what is asked for; there, the map is still starting.
        const DWORD tiles = GetFileAttributesA((g_gameDir + "Valkyrie-radar-tiles").c_str());
        if (tiles != INVALID_FILE_ATTRIBUTES && (tiles & FILE_ATTRIBUTE_DIRECTORY)) {
            ui::Label(ui::kScreenW / 2, 190.0f, "Loading the map...", F(18.0f, kWhite, sprite::Align::Centre));
        } else {
            // (The tiles themselves are in the README.)
            ui::Label(ui::kScreenW / 2, 190.0f, "Maps Unavailable", F(18.0f, kWhite, sprite::Align::Centre));
            ui::Label(ui::kScreenW / 2, 216.0f, "No map data could be loaded.", F(15.0f, kGrey, sprite::Align::Centre, 0));
        }
        return;
    }

    // Drag moves the map with the finger; the wheel zooms about the middle.
    const float span = 2.0f * m.dist * std::tan(1.0472f * 0.5f);  // world units top to bottom
    const float perPoint = span / (bottom - top);
    if (!g_drawingFrom) {
        if (ui::Dragging() && ui::CursorY() > top && ui::CursorY() < bottom) {
            const float c = std::cos(m.heading), sn = std::sin(m.heading);
            const float dx = ui::ScrollDeltaX() * perPoint, dy = ui::ScrollDelta() * perPoint;
            // Screen right is (cos, sin) in the world, screen up (-sin, cos).
            m.x += c * dx + sn * dy;
            m.y += sn * dx - c * dy;
            m.follow = false;
        } else if (ui::ScrollDelta() != 0.0f) {
            m.dist = std::clamp(m.dist * std::exp(ui::ScrollDelta() / 44.0f * 0.18f), 40.0f, kMapFarthest);
        }
    }
    if (m.follow && player) {
        m.x = px;
        m.y = py;
        m.z = pz;
    }
    m.lean01 += ((m.lean ? 1.0f : 0.0f) - m.lean01) * std::min(1.0f, g.dt * 8.0f);

    // The picture, at the size it is shown, from the radar.
    const float x0 = ui::ToPixelX(0), y0 = ui::ToPixelY(top), x1 = ui::ToPixelX(ui::kScreenW), y1 = ui::ToPixelY(bottom);
    static IDirect3DTexture9* last = nullptr;
    if (!g_drawingFrom && !Moving()) {
        const float view[6] = {m.x, m.y, m.z, m.heading, m.lean01 * 0.95f, m.dist};
        last = radar.render(view, static_cast<unsigned>(x1 - x0), static_cast<unsigned>(y1 - y0));
    }
    static ULONGLONG since = 0;
    if (last) {
        since = 0;
        DrawDeviceTexture(last, x0, y0, x1, y1);
    } else {
        if (!since) since = GetTickCount64();
        const bool stuck = GetTickCount64() - since > 3000;
        ui::Label(ui::kScreenW / 2, (top + bottom) / 2 - 10.0f, stuck ? "Maps Unavailable" : "Loading the map...",
                  F(16.0f, kWhite, sprite::Align::Centre));
    }

    // CJ: the game's own radar arrow, turned the way he faces.
    float at[2];
    const float where[3] = {px, py, pz};
    if (last && player && radar.project && radar.project(where, at) && at[0] > 0.0f && at[0] < 1.0f &&
        at[1] > 0.0f && at[1] < 1.0f) {
        const float cx = x0 + at[0] * (x1 - x0), cy = y0 + at[1] * (y1 - y0);
        const float r = 11.0f * ui::PixelsPerPoint();
        const float a = -(ph - m.heading);
        auto P = [&](float lx, float ly) {
            return sprite::Corner{cx + lx * std::cos(a) - ly * std::sin(a), cy + lx * std::sin(a) + ly * std::cos(a)};
        };
        uintptr_t arrow = sprite::Find(g.hud, "radar_centre");
        if (!arrow) arrow = ui::Tex("white");
        sprite::Quad(arrow, P(-r, r), P(r, r), P(-r, -r), P(r, -r), kWhite);
    }

    // The bar: back to CJ, zoom out and in, lean.
    ui::Fill(0, bottom, ui::kScreenW, ui::kScreenH - bottom, kPanelDark);
    ui::Fill(0, bottom, ui::kScreenW, 1.5f, kBlack);
    const float bw = ui::kScreenW / 4;
    if (Button(4.0f, bottom + 5.0f, bw - 8.0f, 38.0f, m.follow ? 0xFF2F5E9E : 0xFF3A3A3E, "Me", 16.0f)) {
        m.follow = true;
    }
    if (Button(bw + 4.0f, bottom + 5.0f, bw - 8.0f, 38.0f, 0xFF3A3A3E, "-", 22.0f)) {
        m.dist = std::min(kMapFarthest, m.dist * 1.5f);
    }
    if (Button(bw * 2 + 4.0f, bottom + 5.0f, bw - 8.0f, 38.0f, 0xFF3A3A3E, "+", 22.0f)) {
        m.dist = std::max(40.0f, m.dist / 1.5f);
    }
    if (Button(bw * 3 + 4.0f, bottom + 5.0f, bw - 8.0f, 38.0f, m.lean ? 0xFF2F5E9E : 0xFF3A3A3E, m.lean ? "3D" : "2D",
               16.0f)) {
        m.lean = !m.lean;
    }
    StatusBar();
    Header("Maps");
}

// --- Sounds -----------------------------------------------------------------

// The tone picked in Settings, by name, from the ini's list; the first one
// when nothing is picked or the picked one has gone from the ini.
const config::Tone& PickedTone(const std::vector<config::Tone>& tones, const char* key) {
    static const config::Tone kSilent{"None", ""};
    if (tones.empty()) return kSilent;
    const auto& store = phone_data::Get().store;
    const auto it = store.find(key);
    if (it != store.end()) {
        // The phone's own tone was called SP-RP.
        const std::string want = it->second == "SP-RP" ? "Valkyrie" : it->second;
        for (const auto& t : tones) {
            if (t.name == want) return t;
        }
    }
    return tones.front();
}

const config::Tone& Ringtone() { return PickedTone(config::Get().ringtones, "ringtone"); }
const config::Tone& TextTone() { return PickedTone(config::Get().textTones, "texttone"); }

// A text arriving from `number` - an order that did not make it, from the
// restaurant.
std::vector<std::pair<std::string, std::string>> g_waitingTexts;

void IncomingText(const char* number, const char* body) {
    // Out of service, a text waits for the signal to come back.
    if (!coverage::HasService()) {
        g_waitingTexts.emplace_back(number, body);
        return;
    }
    Message m;
    m.number = number;
    m.text = body;
    m.outgoing = false;
    m.delivered = true;
    m.read = false;
    m.at = phone_data::Now();
    auto& messages = phone_data::Get().messages;
    messages.push_back(m);
    if (messages.size() > phone_data::kMaxMessages) messages.erase(messages.begin());
    Save();
    if (phone_data::Get().settings.poweredOn) PlayAlert(TextTone().sound, 300);
    logfile::Line("phone: a text from %s", number);
}
const config::Tone& AlarmTone() { return PickedTone(config::Get().ringtones, "alarmtone"); }

// --- Calculator -------------------------------------------------------------

std::string CalcText(double v) {
    if (!std::isfinite(v)) return "Error";
    char buf[32];
    snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

void CalcApply() {
    auto& c = g.calc;
    const double shown = atof(c.shown.c_str());
    switch (c.op) {
        case '+': c.acc += shown; break;
        case '-': c.acc -= shown; break;
        case '*': c.acc *= shown; break;
        case '/': c.acc = shown == 0.0 ? NAN : c.acc / shown; break;
        default: c.acc = shown;
    }
    c.shown = CalcText(c.acc);
}

void CalcKey(const std::string& k) {
    auto& c = g.calc;
    if (k == "C") {
        c = {};
    } else if (k == "+/-") {
        if (c.shown != "0") c.shown = c.shown[0] == '-' ? c.shown.substr(1) : "-" + c.shown;
    } else if (k == "%") {
        c.shown = CalcText(atof(c.shown.c_str()) / 100.0);
    } else if (k == "+" || k == "-" || k == "*" || k == "/") {
        if (!c.fresh) CalcApply();
        else c.acc = atof(c.shown.c_str());
        c.op = k[0];
        c.fresh = true;
    } else if (k == "=") {
        CalcApply();
        c.op = 0;
        c.fresh = true;
    } else {
        // A digit or the point.
        if (c.fresh || c.shown == "0" || c.shown == "Error") c.shown = k == "." ? "0" : "";
        c.fresh = false;
        if (k == "." && c.shown.find('.') != std::string::npos) return;
        if (c.shown.size() < 12) c.shown += k;
    }
}

void CalculatorScreen() {
    Backdrop();
    auto& c = g.calc;
    const float top = kContentTop + 8.0f;
    ui::Fill(10.0f, top, ui::kScreenW - 20.0f, 64.0f, kPanelDark);
    ui::Frame(10.0f, top, ui::kScreenW - 20.0f, 64.0f, 2.0f, kBlack);
    const Font df = Pricedown(40.0f, kWhite, sprite::Align::Right);
    ui::Label(ui::kScreenW - 22.0f, top + 10.0f, ui::Fit(c.shown, 270.0f, df), df);
    // Typing works as well as tapping. The keys come in before the screen is
    // drawn, into the field it named last frame, so the field has to last.
    static std::string typed;
    const std::string keysTyped = typed;
    typed.clear();
    g.field = {&typed, 8, false};
    const char* const keys[5][4] = {{"C", "+/-", "%", "/"},
                                    {"7", "8", "9", "*"},
                                    {"4", "5", "6", "-"},
                                    {"1", "2", "3", "+"},
                                    {"0", "", ".", "="}};
    const float kx = 10.0f, ky = top + 76.0f, kw = (ui::kScreenW - 20.0f) / 4, kh = (ui::kScreenH - ky - 10.0f) / 5;
    for (int r = 0; r < 5; ++r) {
        for (int col = 0; col < 4; ++col) {
            std::string k = keys[r][col];
            if (k.empty()) continue;
            // "0" takes two keys' width, as on the phone.
            const float w = (r == 4 && col == 0) ? kw * 2 : kw;
            const Rect rect{kx + col * kw + 2.0f, ky + r * kh + 2.0f, w - 4.0f, kh - 4.0f};
            const bool op = col == 3 || r == 0;
            const bool hot = ui::Pressing(rect) || (c.op && k.size() == 1 && k[0] == c.op && c.fresh);
            ui::Fill(rect.x, rect.y, rect.w, rect.h, hot ? kMenuBlue : op ? 0xDC2A2A30 : kPanel);
            ui::Frame(rect.x, rect.y, rect.w, rect.h, 1.5f, kBlack);
            const char* shown = k == "*" ? "x" : k.c_str();
            ui::Label(rect.x + rect.w / 2, rect.y + (rect.h - 26.0f) / 2, shown,
                      F(26.0f, hot ? kBlack : kWhite, sprite::Align::Centre, hot ? 0 : 1));
            if (ui::Tapped(rect)) CalcKey(k);
        }
    }
    ui::Flush();
    StatusBar();
    Header("Calculator");
    for (const char ch : keysTyped) {
        if (isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '+' || ch == '-' || ch == '*' ||
            ch == '/' || ch == '%') {
            CalcKey(std::string(1, ch));
        } else if (ch == '=') {
            CalcKey("=");
        } else if (ch == 'c' || ch == 'C') {
            CalcKey("C");
        }
    }
    if (g.enter) CalcKey("=");
}

// --- Notes ------------------------------------------------------------------

std::vector<std::string> LoadNotes() {
    std::vector<std::string> notes;
    const auto& store = phone_data::Get().store;
    for (int i = 0;; ++i) {
        const auto it = store.find("note." + std::to_string(i));
        if (it == store.end()) break;
        notes.push_back(it->second);
    }
    return notes;
}

void SaveNotes(const std::vector<std::string>& notes) {
    auto& store = phone_data::Get().store;
    for (int i = 0; store.count("note." + std::to_string(i)); ++i) store.erase("note." + std::to_string(i));
    for (size_t i = 0; i < notes.size(); ++i) store["note." + std::to_string(i)] = notes[i];
    Save();
}

std::string NoteTitle(const std::string& text) {
    std::string t = text.substr(0, text.find('\n'));
    return t.empty() ? "New Note" : t;
}

void NotesScreen() {
    const auto notes = LoadNotes();
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom);
    const float y0 = y;
    int opened = -1;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (Row(y, 44.0f, NoteTitle(notes[i]))) opened = static_cast<int>(i);
        y += 44.0f;
    }
    if (notes.empty()) Empty("No Notes");
    EndList(top, bottom, y - y0);
    ui::Flush();
    StatusBar();
    const int nav = Header("Notes", nullptr, "g_compose");
    if (nav == 1 || opened >= 0) {
        g.note = nav == 1 ? -1 : opened;
        g.noteText = nav == 1 ? std::string() : notes[opened];
        Go(Screen::NoteEdit);
    }
}

void NoteEditScreen() {
    // A yellow pad, written on in the subtitle font. Enter starts a new line.
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, 0xFFF3E08A);
    const float top = kContentTop;
    for (float ly = top + 30.0f; ly < ui::kScreenH - 60.0f; ly += 22.0f) {
        ui::Fill(12.0f, ly, ui::kScreenW - 24.0f, 1.0f, 0x505A78AA);
    }
    if (g.enter) g.noteText += '\n';
    g.field = {&g.noteText, 2000, false};
    const Font f = F(16.0f, 0xFF2A2A2A, sprite::Align::Left, 0);
    const sprite::TextStyle ts{sprite::Face::Subtitles, 16.0f * ui::PixelsPerPoint()};
    float y = top + 12.0f;
    std::string text = g.noteText + (((GetTickCount64() / 500) % 2 == 0) ? "_" : " ");
    size_t start = 0;
    while (start <= text.size() && y < ui::kScreenH - 70.0f) {
        const size_t nl = text.find('\n', start);
        const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        auto lines = sprite::Wrap(para.c_str(), (ui::kScreenW - 36.0f) * ui::PixelsPerPoint(), ts);
        if (lines.empty()) lines.push_back("");
        for (const auto& l : lines) {
            ui::Label(18.0f, y, l, f);
            y += 22.0f;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (Button(20.0f, ui::kScreenH - 56.0f, 280.0f, 42.0f, kWastedRed, g.note >= 0 ? "Delete Note" : "Discard",
               17.0f)) {
        auto notes = LoadNotes();
        if (g.note >= 0 && g.note < static_cast<int>(notes.size())) notes.erase(notes.begin() + g.note);
        SaveNotes(notes);
        Go(Screen::Notes);
        return;
    }
    ui::Flush();
    StatusBar();
    if (Header(NoteTitle(g.noteText), "Notes", "Done") != 0) {
        auto notes = LoadNotes();
        const bool blank = g.noteText.find_first_not_of(" \n") == std::string::npos;
        if (g.note >= 0 && g.note < static_cast<int>(notes.size())) {
            if (blank) notes.erase(notes.begin() + g.note);
            else notes[g.note] = g.noteText;
        } else if (!blank) {
            notes.insert(notes.begin(), g.noteText);
        }
        SaveNotes(notes);
        Go(Screen::Notes);
    }
}

// --- Weather ----------------------------------------------------------------
//
// San Andreas' own weather, as the game keeps it (CWeather): what the sky is
// doing where CJ is, and what is coming. Each part of the state has a list
// of weathers the game steps through an hour at a time, one step shared by
// all of them; the forecast is the next steps of those lists, so it comes
// true unless a mission forces the weather. The temperatures are the
// phone's own, from the weather, the place and the hour.

constexpr uintptr_t kWeatherTypeInList = 0xC81310;     // uint32: the step the lists are at
constexpr uintptr_t kWeatherRegion = 0xC81314;         // int16: where CJ is, 0 the country
constexpr uintptr_t kForcedWeather = 0xC81318;         // int16: set by a mission, else -1
constexpr uintptr_t kNewWeather = 0xC8131C;            // int16
constexpr uintptr_t kOldWeather = 0xC81320;            // int16
constexpr uintptr_t kWeatherBlend = 0xC8130C;          // float: old to new, 0 to 1
constexpr uintptr_t kFindWeatherTypesList = 0x72A520;  // CWeather::FindWeatherTypesList, by the region
constexpr int kWeatherSteps = 64;                      // each list's length
constexpr uintptr_t kClockMonthDay = 0xB70154;         // CClock::ms_nGameClockDays, 1-31
constexpr uintptr_t kClockMonth = 0xB70155;            // CClock::ms_nGameClockMonth, 1-12

struct Sky {
    const char* name;
    const char* icon;
    int warmth;  // degrees F on the place's usual
};

Sky SkyOf(int type, int hour) {
    const bool night = hour < 6 || hour >= 20;
    const char* clear = night ? "w_moon" : "w_sun";
    const char* fair = night ? "w_moon" : "w_partly";
    switch (type) {
        case 0: case 6: case 11: case 13: case 17: return {"Sunny", clear, 3};
        case 1: case 5: case 10: case 14: case 18: return {"Mostly Sunny", fair, 1};
        case 2: return {"Hazy Sunshine", night ? "w_moon" : "w_haze", 3};
        case 3: return {"Smog", "w_haze", 1};
        case 4: case 7: case 12: case 15: return {"Cloudy", "w_cloud", -4};
        case 8: return {"Rain", "w_rain", -8};
        case 9: return {"Fog", "w_fog", -6};
        case 16: return {"Thunderstorms", "w_storm", -9};
        case 19: return {"Sandstorm", "w_sand", 2};
        default: return {"Strange Skies", "w_cloud", 0};
    }
}

struct Region {
    const char* name;
    int usual;  // degrees F, mid afternoon, fair
};
const Region kRegions[5] = {
    {"Countryside", 76}, {"Los Santos", 80}, {"San Fierro", 66}, {"Las Venturas", 90}, {"Bone County", 97},
};

int Temperature(int region, int type, float hour) {
    const float day = std::cos((hour - 15.0f) / 24.0f * 6.2831853f);  // 1 mid afternoon, -1 at three at night
    return static_cast<int>(std::lround(kRegions[region].usual + SkyOf(type, static_cast<int>(hour)).warmth +
                                        9.0f * day - 9.0f));
}

// A region's list of weathers. The game picks the list by where CJ is; for
// anywhere else it is asked with the region set there for the call.
const int16_t* WeatherList(int region) {
    auto& where = *reinterpret_cast<int16_t*>(kWeatherRegion);
    const int16_t was = where;
    where = static_cast<int16_t>(region);
    const int16_t* list = reinterpret_cast<const int16_t*(__cdecl*)()>(kFindWeatherTypesList)();
    where = was;
    return list;
}

// The weather `ahead` hours from now in a region (0 now).
int WeatherAt(int region, int ahead) {
    const int16_t forced = *reinterpret_cast<const int16_t*>(kForcedWeather);
    if (forced >= 0) return forced;
    const int here = std::clamp<int>(*reinterpret_cast<const int16_t*>(kWeatherRegion), 0, 4);
    if (ahead == 0 && region == here) {
        const float blend = *reinterpret_cast<const float*>(kWeatherBlend);
        return *reinterpret_cast<const int16_t*>(blend < 0.5f ? kOldWeather : kNewWeather);
    }
    const int16_t* list = WeatherList(region);
    if (!list) return *reinterpret_cast<const int16_t*>(kNewWeather);
    const uint32_t step = *reinterpret_cast<const uint32_t*>(kWeatherTypeInList);
    return list[(step + static_cast<uint32_t>(ahead)) % kWeatherSteps];
}

// A temperature, and the degree mark after it as a small dot (the game's
// fonts have no degree sign).
void Degrees(float x, float y, int value, const Font& f) {
    const std::string text = std::to_string(value);
    const float w = ui::TextWidth(text, f), dot = std::max(3.0f, f.size * 0.16f);
    float left = x;
    if (f.align == sprite::Align::Right) left = x - dot * 1.6f - w;
    else if (f.align == sprite::Align::Centre) left = x - (w + dot * 1.6f) / 2;
    Font l = f;
    l.align = sprite::Align::Left;
    ui::Label(left, y, text, l);
    ui::Image("disc", left + w + dot * 0.5f, y + f.size * 0.08f, dot, dot, f.argb);
}

void WeatherScreen() {
    const auto now = phone_data::Now();
    const float hour = now.hour + now.minute / 60.0f;
    const bool night = now.hour < 6 || now.hour >= 20;
    // The first iPhone's weather cards: blue by day, a deep purple by night.
    const uint32_t cardTop = night ? 0xFF2C2350 : 0xFF3F8EDB, cardBottom = night ? 0xFF140F2A : 0xFF1D5FA8;
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, 0xFF0B0B0E);
    const int here = std::clamp<int>(*reinterpret_cast<const int16_t*>(kWeatherRegion), 0, 4);
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom);
    const float y0 = y;

    // Where CJ is: the sky now, and the next six hours.
    {
        const int type = WeatherAt(here, 0);
        const Sky sky = SkyOf(type, now.hour);
        const float x = 8.0f, w = ui::kScreenW - 16.0f, h = 228.0f;
        ui::Gradient(x, y + 8.0f, w, h, cardTop, cardBottom);
        ui::Frame(x, y + 8.0f, w, h, 2.0f, kBlack);
        ui::Label(x + 14.0f, y + 18.0f, kRegions[here].name, F(22.0f));
        ui::Label(x + 14.0f, y + 46.0f, sky.name, F(15.0f, 0xFFD8E4F4));
        Degrees(x + w - 14.0f, y + 16.0f, Temperature(here, type, hour), Pricedown(54.0f, kWhite, sprite::Align::Right));
        ui::Image(sky.icon, x + w - 150.0f, y + 16.0f, 64.0f, 64.0f);
        // Today's high and low, from the day ahead.
        int hi = -999, lo = 999;
        for (int k = 0; k < 24; ++k) {
            const int t = Temperature(here, WeatherAt(here, k), static_cast<float>((now.hour + k) % 24));
            hi = std::max(hi, t);
            lo = std::min(lo, t);
        }
        ui::Label(x + 14.0f, y + 70.0f, "High " + std::to_string(hi) + "  Low " + std::to_string(lo),
                  F(15.0f, 0xFFD8E4F4));
        ui::Fill(x + 10.0f, y + 100.0f, w - 20.0f, 1.0f, 0x40FFFFFF);
        const float cw = (w - 20.0f) / 6.0f;
        for (int k = 1; k <= 6; ++k) {
            const int at = (now.hour + k) % 24;
            const int t = WeatherAt(here, k);
            const float cx = x + 10.0f + (k - 0.5f) * cw;
            const int h12 = at % 12 == 0 ? 12 : at % 12;
            ui::Label(cx, y + 110.0f, std::to_string(h12) + (at < 12 ? "a" : "p"),
                      F(13.0f, 0xFFD8E4F4, sprite::Align::Centre));
            ui::Image(SkyOf(t, at).icon, cx - 18.0f, y + 132.0f, 36.0f, 36.0f);
            Degrees(cx, y + 174.0f, Temperature(here, t, static_cast<float>(at)), F(16.0f, kWhite, sprite::Align::Centre));
        }
        const bool forced = *reinterpret_cast<const int16_t*>(kForcedWeather) >= 0;
        ui::Label(x + w / 2, y + 204.0f, forced ? "Unusual conditions - the forecast holds for now" : "Next six hours",
                  F(12.0f, 0xC0FFFFFF, sprite::Align::Centre, 0));
        y += 8.0f + h + 10.0f;
    }

    // The rest of the state, now.
    for (int r = 1; r <= 5; ++r) {
        const int region = r % 5;  // the three cities, the desert, then the country
        if (region == here) continue;
        const int type = WeatherAt(region, 0);
        const Sky sky = SkyOf(type, now.hour);
        const float x = 8.0f, w = ui::kScreenW - 16.0f, h = 64.0f;
        ui::Gradient(x, y, w, h, cardTop, cardBottom);
        ui::Frame(x, y, w, h, 2.0f, kBlack);
        ui::Label(x + 12.0f, y + 10.0f, kRegions[region].name, F(19.0f));
        ui::Label(x + 12.0f, y + 36.0f, sky.name, F(13.0f, 0xFFD8E4F4));
        ui::Image(sky.icon, x + w - 128.0f, y + 12.0f, 40.0f, 40.0f);
        Degrees(x + w - 12.0f, y + 12.0f, Temperature(region, type, hour), Pricedown(34.0f, kWhite, sprite::Align::Right));
        y += h + 8.0f;
    }
    ui::Label(ui::kScreenW / 2, y + 4.0f, "Weather from the San Andreas Weather Service",
              F(12.0f, kDimGrey, sprite::Align::Centre, 0));
    y += 30.0f;
    EndList(top, bottom, y - y0);
    ui::Flush();
    StatusBar();
    Header("Weather");
}

// --- Stocks -----------------------------------------------------------------
//
// BAWSAQ, the San Andreas exchange: the state's companies, their shares
// moving through the trading day on the game's clock and holding still
// while the market is shut (half past nine to four). Every phone sees the
// same prices at the same game time - they are worked out, not stored.

// The companies and the hours are [Stocks] in the ini.
using Company = config::Config::Company;
#define kCompanies (config::Get().companies)
#define kCompanyCount (static_cast<int>(config::Get().companies.size()))
#define kOpen (config::Get().marketOpen)
#define kClose (config::Get().marketClose)

// A smooth wander between -1 and 1: noise by the hour, eased between.
float Wander(uint32_t seed, double t) {
    auto hash = [](uint32_t x) {
        x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
        return static_cast<float>(x & 0xFFFF) / 32767.5f - 1.0f;
    };
    const double f = std::floor(t);
    const float k = static_cast<float>(t - f), e = k * k * (3.0f - 2.0f * k);
    const uint32_t i = static_cast<uint32_t>(static_cast<int64_t>(f));
    return hash(seed * 7919u + i) * (1.0f - e) + hash(seed * 7919u + i + 1) * e;
}

// The clock the prices move on: trading hours since the year began, which
// stands still overnight and over the weekend. The game keeps a day and a
// month, and today's day of the week, which gives every other day's.
struct Trading {
    double now;      // on that clock
    double session;  // where the session shown began: today's, or the last one
    bool open;
};
Trading TradingClock() {
    static const int kDaysBefore[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const auto clock = phone_data::Now();
    const int month = std::clamp<int>(*reinterpret_cast<const uint8_t*>(kClockMonth), 1, 12);
    const int day = std::clamp<int>(*reinterpret_cast<const uint8_t*>(kClockMonthDay), 1, 31);
    const int doy = kDaysBefore[month - 1] + day - 1;  // today, from 0
    const int weekday = clock.day - 1;                 // 0 Sunday
    const int jan1 = ((weekday - doy) % 7 + 7) % 7;
    // The weekdays before day n of the year.
    auto weekdaysBefore = [&](int n) {
        int count = (n / 7) * 5;
        for (int i = 0; i < n % 7; ++i) {
            const int wd = (jan1 + (n / 7) * 7 + i) % 7;
            count += wd != 0 && wd != 6;
        }
        return count;
    };
    const float span = kClose - kOpen, hour = clock.hour + clock.minute / 60.0f;
    const double start = weekdaysBefore(doy) * static_cast<double>(span);
    const bool weekday5 = weekday != 0 && weekday != 6;
    Trading t{};
    t.open = weekday5 && hour >= kOpen && hour < kClose;
    if (weekday5 && hour >= kOpen) {
        t.now = start + std::min(hour, kClose) - kOpen;
        t.session = start;
    } else {
        // Before the bell, or the weekend: the last session, closed.
        t.now = start;
        t.session = std::max(0.0, start - span);
    }
    return t;
}

float Price(int company, double t) {
    const Company& c = kCompanies[company];
    const uint32_t s = static_cast<uint32_t>(company + 1);
    const float w = 0.8f * Wander(s, t / 60.0) + 0.4f * Wander(s + 100, t / 9.0) + 0.12f * Wander(s + 200, t * 1.5);
    return c.base * std::exp(c.swing * w);
}

void StocksScreen() {
    const Trading clock = TradingClock();
    const double t = clock.now, prevClose = clock.session;
    const bool open = clock.open;
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, kBlack);
    const float chartH = 176.0f;
    const float top = kContentTop, bottom = ui::kScreenH - chartH;
    float y = BeginList(top, bottom);
    const float y0 = y;
    int& picked = g.stock;
    picked = std::clamp(picked, 0, kCompanyCount - 1);
    for (int i = 0; i < kCompanyCount; ++i) {
        const float p = Price(i, t), was = Price(i, prevClose);
        const float change = p - was;
        const Rect row{0, y, ui::kScreenW, 46.0f};
        if (i == picked) ui::Fill(0, y, ui::kScreenW, 46.0f, 0xFF1E2A3C);
        else if (ui::Pressing(row)) ui::Fill(0, y, ui::kScreenW, 46.0f, 0xFF1A1A1E);
        ui::Label(12.0f, y + 12.0f, kCompanies[i].ticker, F(19.0f));
        char text[32];
        snprintf(text, sizeof text, "%.2f", p);
        ui::Label(200.0f, y + 13.0f, text, F(18.0f, kWhite, sprite::Align::Right));
        // The change, in a green or red pill; tapped, it shows the other way.
        const Rect pill{216.0f, y + 8.0f, 94.0f, 30.0f};
        ui::Fill(pill.x, pill.y, pill.w, pill.h, change >= 0.0f ? 0xFF3AA33A : 0xFFD0342C);
        ui::Frame(pill.x, pill.y, pill.w, pill.h, 1.5f, kBlack);
        if (g.stockPercent) snprintf(text, sizeof text, "%+.2f%%", was > 0.0f ? change / was * 100.0f : 0.0f);
        else snprintf(text, sizeof text, "%+.2f", change);
        ui::Label(pill.x + pill.w - 8.0f, pill.y + 6.0f, text, F(16.0f, kWhite, sprite::Align::Right));
        if (ui::Tapped(pill)) g.stockPercent = !g.stockPercent;
        else if (ui::Tapped(row)) picked = i;
        ui::Fill(0, y + 45.0f, ui::kScreenW, 1.0f, 0xFF2A2A30);
        y += 46.0f;
    }
    EndList(top, bottom, y - y0);

    // The picked company's day, drawn from the open to now.
    {
        const float x = 0.0f, cy = bottom, w = ui::kScreenW;
        ui::Gradient(x, cy, w, chartH, 0xFF2A2C33, 0xFF121317);
        ui::Fill(x, cy, w, 2.0f, kBlack);
        ui::Label(12.0f, cy + 8.0f, kCompanies[picked].name, F(17.0f));
        ui::Label(w - 12.0f, cy + 10.0f, open ? "Market Open" : "Market Closed",
                  F(13.0f, open ? 0xFF7ED67E : kDimGrey, sprite::Align::Right));
        const float gx = 44.0f, gy = cy + 36.0f, gw = w - 56.0f, gh = chartH - 64.0f;
        const double span = kClose - kOpen;
        const double from = prevClose, to = std::max(t, from + 0.05);
        constexpr int kPoints = 40;
        float values[kPoints + 1];
        float lo = 1e9f, hi = -1e9f;
        for (int k = 0; k <= kPoints; ++k) {
            values[k] = Price(picked, from + (to - from) * k / kPoints);
            lo = std::min(lo, values[k]);
            hi = std::max(hi, values[k]);
        }
        if (hi - lo < 0.02f) { hi += 0.01f; lo -= 0.01f; }
        for (int r = 0; r <= 2; ++r) {
            const float ly = gy + gh * r / 2.0f;
            ui::Fill(gx, ly, gw, 1.0f, 0x30FFFFFF);
            char text[16];
            snprintf(text, sizeof text, "%.2f", hi - (hi - lo) * r / 2.0f);
            ui::Label(gx - 4.0f, ly - 7.0f, text, F(11.0f, kDimGrey, sprite::Align::Right, 0));
        }
        const float up = values[kPoints] >= values[0] ? 1.0f : 0.0f;
        const uint32_t line = up > 0.5f ? 0xFF7ED67E : 0xFFFF6A5E;
        const float used = static_cast<float>((to - from) / span);  // of the day's width
        for (int k = 0; k < kPoints; ++k) {
            const float x0 = gx + gw * used * k / kPoints, x1 = gx + gw * used * (k + 1) / kPoints;
            const float ya = gy + gh * (hi - values[k]) / (hi - lo), yb = gy + gh * (hi - values[k + 1]) / (hi - lo);
            const float dx = x1 - x0, dy = yb - ya;
            ui::Needle(x0, ya, std::atan2(dx, -dy), std::sqrt(dx * dx + dy * dy), 0.0f, 2.0f, line);
        }
        ui::Label(gx, gy + gh + 4.0f, "9:30", F(11.0f, kDimGrey, sprite::Align::Left, 0));
        ui::Label(gx + gw, gy + gh + 4.0f, "4:00", F(11.0f, kDimGrey, sprite::Align::Right, 0));
    }
    ui::Flush();
    StatusBar();
    Header("BAWSAQ");
}

// --- Radio ------------------------------------------------------------------
//
// San Andreas' radio, from the phone: on foot a station plays as it does
// from the pause menu's audio settings; in a car, a station picked here
// retunes the car's own radio. A call silences it until it is over.

constexpr uintptr_t kAudioEngine = 0xB6BC90;         // CAudioEngine AudioEngine
constexpr uintptr_t kStartRadio = 0x507DC0;          // (int station, char bass)
constexpr uintptr_t kStopRadio = 0x506F70;           // (tVehicleAudioSettings*, bool)
constexpr uintptr_t kRetuneRadio = 0x507E10;         // (char station)
constexpr uintptr_t kIsRadioOn = 0x506FD0;           // bool ()
constexpr uintptr_t kCurrentRadioStation = 0x507040; // char ()

struct Station {
    int id;
    const char* name;
    const char* kind;
    const char* logo;  // in models\fronten1.txd
    uint32_t colour;   // for the disc drawn when the logo is not there
};
const Station kStations[] = {
    {1, "Playback FM", "Classic Hip-Hop", "radio_playback", 0xFF3A6FD8},
    {2, "K-Rose", "Country", "radio_KROSE", 0xFFD84A7A},
    {3, "K-DST", "Classic Rock", "radio_KDST", 0xFFC8A030},
    {4, "Bounce FM", "Funk", "radio_BOUNCE", 0xFFD8602A},
    {5, "SF-UR", "House", "radio_SFUR", 0xFF30A8C8},
    {6, "Radio Los Santos", "Gangsta Rap", "radio_RLS", 0xFF2E9A42},
    {7, "Radio X", "Alternative", "radio_RADIOX", 0xFF8A8A8A},
    {8, "CSR 103.9", "New Jack Swing", "radio_CSR", 0xFFB040C0},
    {9, "K-Jah West", "Reggae", "radio_KJAH", 0xFF3CA83C},
    {10, "Master Sounds 98.3", "Rare Groove", "radio_MASTERSOUNDS", 0xFFE0B040},
    {11, "WCTR", "Talk Radio", "radio_WCTR", 0xFF5070B0},
    {12, "User Track Player", "Your own music", "radio_TPLAYER", 0xFF606068},
};

void* Audio() { return reinterpret_cast<void*>(kAudioEngine); }
bool RadioOn() { return reinterpret_cast<bool(__thiscall*)(void*)>(kIsRadioOn)(Audio()); }
int RadioStation() {
    return static_cast<signed char>(reinterpret_cast<char(__thiscall*)(void*)>(kCurrentRadioStation)(Audio()));
}
void StopPhoneRadio() {
    if (!g.radio) return;
    reinterpret_cast<void(__thiscall*)(void*, void*, bool)>(kStopRadio)(Audio(), nullptr, false);
    g.radio = 0;
}
bool InVehicle() {
    game::VehicleState vehicle{};
    return game::PlayerVehicleState(vehicle);
}
void PlayStation(int id) {
    if (InVehicle()) {
        reinterpret_cast<void(__thiscall*)(void*, char)>(kRetuneRadio)(Audio(), static_cast<char>(id));
        g.radio = 0;
        return;
    }
    if (g.radio) StopPhoneRadio();
    reinterpret_cast<void(__thiscall*)(void*, int, char)>(kStartRadio)(Audio(), id, 0);
    g.radio = id;
    g.radioAt = GetTickCount64();
    logfile::Line("phone: radio station %d started", id);
}

// The interior ambience manager (the stations heard in shops and bars)
// switches off any station that is on while CJ stands somewhere without one
// - three calls to CAudioEngine::StopRadio in its service, found in the
// game's code at these addresses. While the phone's own station plays on
// foot those three are let go past; everything else still reaches the
// radio as before, and whatever the calls went to (another mod's hook
// included) is what they go on to.
constexpr uintptr_t kAmbienceRadioStops[3] = {0x4D71D2, 0x4D71FD, 0x4D7221};
uintptr_t g_ambienceStopNext[3] = {};

template <int I>
void __fastcall AmbienceStopRadio(void* audio, void*, void* settings, bool arg) {
    if (g.radio && !InVehicle()) {
        static ULONGLONG told = 0;
        if (GetTickCount64() - told > 10000) {
            told = GetTickCount64();
            logfile::Line("phone: the ambience manager asked to stop the phone's radio - left playing");
        }
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, void*, bool)>(g_ambienceStopNext[I])(audio, settings, arg);
}

// The radio's own pause check (CAERadioTrackManager::CheckForPause, run by
// its service every frame) switches the station off once CJ is on foot
// with no radio car and no bar's radio around him - a second after the
// phone started it. Its question "is an ambience radio playing here?" is
// answered yes while the phone's station plays on foot, as it is in a bar.
constexpr uintptr_t kPauseCheckAmbienceAsk = 0x4EA5C3;
uintptr_t g_pauseCheckAskNext = 0;

bool __fastcall PauseCheckAmbienceActive(void* audio, void*) {
    if (g.radio && !InVehicle()) return true;
    return reinterpret_cast<bool(__thiscall*)(void*)>(g_pauseCheckAskNext)(audio);
}

void GuardPhoneRadio() {
    if (auto* at = reinterpret_cast<uint8_t*>(kPauseCheckAmbienceAsk); at[0] == 0xE8) {
        g_pauseCheckAskNext = kPauseCheckAmbienceAsk + 5 + *reinterpret_cast<const int32_t*>(at + 1);
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PauseCheckAmbienceActive) - (kPauseCheckAmbienceAsk + 5));
        DWORD old = 0;
        if (VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<int32_t*>(at + 1) = rel;
            VirtualProtect(at + 1, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 5);
        }
    } else {
        logfile::Line("phone: the radio's pause check at %08X is not a call - left alone", kPauseCheckAmbienceAsk);
    }
    void* const ours[3] = {reinterpret_cast<void*>(&AmbienceStopRadio<0>), reinterpret_cast<void*>(&AmbienceStopRadio<1>),
                           reinterpret_cast<void*>(&AmbienceStopRadio<2>)};
    for (int i = 0; i < 3; ++i) {
        auto* at = reinterpret_cast<uint8_t*>(kAmbienceRadioStops[i]);
        if (at[0] != 0xE8) {
            logfile::Line("phone: the radio guard's call at %08X is not a call - left alone", kAmbienceRadioStops[i]);
            continue;
        }
        g_ambienceStopNext[i] = kAmbienceRadioStops[i] + 5 + *reinterpret_cast<const int32_t*>(at + 1);
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(ours[i]) - (kAmbienceRadioStops[i] + 5));
        DWORD old = 0;
        if (VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            *reinterpret_cast<int32_t*>(at + 1) = rel;
            VirtualProtect(at + 1, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 5);
        }
    }
    logfile::Line("phone: radio guarded from the ambience manager");
}

// Each frame: a car takes the radio over; nobody left to listen, it stops;
// a call silences it and gives it back after.
void RadioFrame() {
    if (g.radio && InVehicle()) g.radio = 0;
    if (g.radio && (!PlayerAble() || !phone_data::Get().settings.poweredOn)) StopPhoneRadio();
    if (g.radio && g.call.active) {
        const int was = g.radio;
        StopPhoneRadio();
        g.radioHeld = was;
    } else if (g.radioHeld && !g.call.active) {
        const int was = g.radioHeld;
        g.radioHeld = 0;
        if (PlayerAble() && !InVehicle()) PlayStation(was);
    }
    // Stopped by the game (a cutscene, the menu's own radio): forgotten -
    // but not in the first seconds, while the game is still loading the
    // station's stream and does not yet call it on.
    if (g.radio) {
        const ULONGLONG since = GetTickCount64() - g.radioAt;
        static ULONGLONG logged = 0;
        if (since < 6000 && GetTickCount64() - logged > 500) {
            logged = GetTickCount64();
            logfile::Line("phone: radio %d after %llu ms: the game says %s, station %d", g.radio, since,
                          RadioOn() ? "on" : "off", RadioStation());
        }
        if (since > 5000 && !RadioOn()) {
            logfile::Line("phone: radio %d stopped by the game", g.radio);
            g.radio = 0;
        }
    }
}

void RadioScreen() {
    static int logos = -2;
    if (logos == -2) logos = sprite::LoadDictionary("valkyrie_phone_frontend1", (g_gameDir + "models\\fronten1.txd").c_str());
    const bool car = InVehicle();
    const int playing = car ? (RadioOn() ? RadioStation() : 0) : g.radio;
    Backdrop();
    const float barH = playing ? 60.0f : 0.0f;
    const float top = kContentTop, bottom = ui::kScreenH - barH;
    float y = BeginList(top, bottom);
    const float y0 = y;
    for (const Station& s : kStations) {
        // Only those [Radio] Stations lists.
        const auto& listed = config::Get().radioStations;
        if (std::find(listed.begin(), listed.end(), s.id) == listed.end()) continue;
        const Rect row{0, y, ui::kScreenW, 58.0f};
        const bool on = s.id == playing;
        if (on) ui::Fill(0, y, ui::kScreenW, 58.0f, 0x604F86C6);
        else if (ui::Pressing(row)) ui::Fill(0, y, ui::kScreenW, 58.0f, kMenuBlueBar);
        if (logos < 0 || !ui::GameImage(logos, s.logo, 10.0f, y + 5.0f, 48.0f, 48.0f)) {
            ui::Image("disc", 12.0f, y + 7.0f, 44.0f, 44.0f, s.colour);
            ui::Label(34.0f, y + 18.0f, std::string(s.name, 2), F(16.0f, kWhite, sprite::Align::Centre));
        }
        ui::Label(68.0f, y + 9.0f, s.name, F(18.0f));
        ui::Label(68.0f, y + 32.0f, s.kind, F(13.0f, kMenuBlue, sprite::Align::Left, 0));
        if (on) {
            // A little level meter, moving.
            const float t = static_cast<float>(GetTickCount64() % 100000) / 1000.0f;
            for (int b = 0; b < 4; ++b) {
                const float h = 6.0f + 14.0f * (0.5f + 0.5f * std::sin(t * (5.0f + b * 1.7f) + b));
                ui::Fill(ui::kScreenW - 44.0f + b * 7.0f, y + 40.0f - h, 5.0f, h, kWhite);
            }
        }
        if (ui::Tapped(row) && !on) {
            PlayStation(s.id);
            if (!phone_data::Get().settings.silent) sound::Click();
        }
        ui::Fill(10.0f, y + 57.0f, ui::kScreenW - 20.0f, 1.0f, kSeparator);
        y += 58.0f;
    }
    EndList(top, bottom, y - y0);
    if (playing) {
        const float by = ui::kScreenH - barH;
        ui::Gradient(0, by, ui::kScreenW, barH, 0xFF2A2C33, 0xFF121317);
        ui::Fill(0, by, ui::kScreenW, 2.0f, kBlack);
        const Station* s = nullptr;
        for (const Station& st : kStations) {
            if (st.id == playing) s = &st;
        }
        ui::Label(12.0f, by + 10.0f, s ? s->name : "Radio", F(17.0f));
        ui::Label(12.0f, by + 34.0f, car ? "On the car's radio" : "Now playing", F(12.0f, kDimGrey, sprite::Align::Left, 0));
        if (!car && Button(ui::kScreenW - 102.0f, by + 12.0f, 90.0f, 36.0f, kWastedRed, "Stop", 17.0f)) {
            StopPhoneRadio();
        }
    }
    ui::Flush();
    StatusBar();
    Header("Radio");
}

// --- Flashlight -------------------------------------------------------------
//
// The phone's light, shone the way CJ faces while the phone is in his hand -
// up, lowered at his side, or in another app. It is built from the game's
// own lights, the ones its cars' headlights are made of: a pool of light
// laid on the ground and walls ahead, a point light for the people and cars
// in it, and a small glare at the phone. It shows best at night; in the day
// it is only just there, as a torch is.

int PhoneWeaponType();

constexpr uintptr_t kAddPointLight = 0x7000E0;   // CPointLights::AddLight
constexpr uintptr_t kStoreShadow = 0x707390;     // CShadows::StoreShadowToBeRendered
// gpShadowHeadLightsTex2, "headlight1": one lamp's pool (the first is a
// car's pair).
constexpr uintptr_t kHeadlightTexture = 0xC403FC;
constexpr uintptr_t kRegisterCorona = 0x6FC580;  // CCoronas::RegisterCorona, by corona type
constexpr uintptr_t kBonePosition = 0x5E4280;    // CPed::GetBonePosition
constexpr uintptr_t kClockHours = 0xB70153, kClockMinutes = 0xB70152;
constexpr int kRightHandBone = 25, kLeftHandBone = 35;

struct V3 { float x, y, z; };

// How dark it is, 0 at midday to 1 at night, from the game's clock: full
// from nine at night to five in the morning, easing in and out round dusk
// and dawn.
float Darkness() {
    const float h = *reinterpret_cast<const uint8_t*>(kClockHours) + *reinterpret_cast<const uint8_t*>(kClockMinutes) / 60.0f;
    if (h >= 21.0f || h < 5.0f) return 1.0f;
    if (h >= 19.0f) return (h - 19.0f) / 2.0f;
    if (h < 7.0f) return 1.0f - (h - 5.0f) / 2.0f;
    return 0.0f;
}

void FlashlightFrame() {
    if (!g.flashlight) return;
    if (!g.out) {
        g.flashlight = false;
        return;
    }
    const uintptr_t ped = PlayerPed();
    game::VehicleState vehicle{};
    if (!ped || !g.holding || !PlayerCanUsePhone() || game::PlayerVehicleState(vehicle)) return;
    const uintptr_t matrix = *reinterpret_cast<const uintptr_t*>(ped + 0x14);
    if (!matrix) return;
    const float* forward = reinterpret_cast<const float*>(matrix + 0x10);
    const float* at = reinterpret_cast<const float*>(matrix + 0x30);
    float fx = forward[0], fy = forward[1];
    const float len = std::hypot(fx, fy);
    if (len < 0.01f) return;
    fx /= len;
    fy /= len;
    // The lamp: the lens on the phone's back, at his left hand - where the
    // phone is drawn, weapon or not.
    V3 hand{at[0], at[1], at[2] + 0.4f};
    {
        float origin[3]{}, direction[3]{};
        const auto& cfg = config::Get();
        if (!phone_model::LeftHandLamp(ped, cfg.handTurn, cfg.handOffset, cfg.handFlip, origin, direction)) return;
        hand = {origin[0], origin[1], origin[2]};
        // Keep the beam aimed along the player's heading even when the idle
        // animation rolls the handset sideways. Only its origin follows the lens.
    }
    const float k = 0.25f + 0.75f * Darkness();

    // The pool of light on the ground ahead, reaching up the walls it meets.
    constexpr float kReach = 6.0f, kWidth = 3.2f;
    V3 pool{hand.x + fx * kReach, hand.y + fy * kReach, hand.z - 0.8f};
    const uintptr_t texture = *reinterpret_cast<const uintptr_t*>(kHeadlightTexture);
    if (texture) {
        reinterpret_cast<void(__cdecl*)(uint8_t, uintptr_t, const V3*, float, float, float, float, short, uint8_t,
                                        uint8_t, uint8_t, float, bool, float, void*, bool)>(kStoreShadow)(
            2, texture, &pool, fx * kReach, fy * kReach, fy * kWidth, -fx * kWidth, static_cast<short>(255 * k),
            static_cast<uint8_t>(200 * k), static_cast<uint8_t>(200 * k), static_cast<uint8_t>(175 * k), 4.0f, false,
            1.0f, nullptr, true);
    }
    // The people and cars out in the beam - reaching back not quite as far
    // as CJ, so it is not his own jeans that light up.
    const V3 lit{hand.x + fx * 5.0f, hand.y + fy * 5.0f, hand.z};
    reinterpret_cast<void(__cdecl*)(uint8_t, V3, V3, float, float, float, float, uint8_t, bool, void*)>(kAddPointLight)(
        0, lit, V3{0, 0, 0}, 4.5f, 0.85f * k, 0.85f * k, 0.72f * k, 0, false, nullptr);
    // The little lamp itself, on the lens.
    const V3 lamp = hand;
    reinterpret_cast<void(__cdecl*)(unsigned, void*, uint8_t, uint8_t, uint8_t, uint8_t, const V3*, float, float, int,
                                    int, bool, bool, int, float, bool, float, uint8_t, float, bool, bool)>(kRegisterCorona)(
        0x56504C54u, nullptr, 255, 250, 225, static_cast<uint8_t>(90 + 140 * Darkness()), &lamp, 0.22f, 60.0f, 0, 0,
        false, true, 0, 0.0f, false, 0.3f, 0, 15.0f, false, false);
}

void FlashlightScreen() {
    Backdrop();
    const bool on = g.flashlight;
    // The switch: a big round button in the middle, lit while the light is.
    const float size = 150.0f, cx = ui::kScreenW / 2, cy = kContentTop + 150.0f;
    const Rect r{cx - size / 2, cy - size / 2, size, size};
    if (on) ui::Image("disc", cx - size / 2 - 18.0f, cy - size / 2 - 18.0f, size + 36.0f, size + 36.0f, 0x40FFF2B0);
    ui::Image("disc", cx - size / 2 - 4.0f, cy - size / 2 - 4.0f, size + 8.0f, size + 8.0f, kBlack);
    ui::Image("disc", cx - size / 2, cy - size / 2, size, size, ui::Pressing(r) ? 0xFF909090 : on ? 0xFFFFE890 : 0xFF3A3A3E);
    ui::Image("g_power", cx - 36.0f, cy - 36.0f, 72.0f, 72.0f, on ? 0xFF6A5A10 : kWhite);
    if (ui::Tapped(r)) {
        g.flashlight = !g.flashlight;
        logfile::Line("phone: flashlight %s", g.flashlight ? "on" : "off");
        if (!phone_data::Get().settings.silent) sound::Click();
    }
    ui::Label(cx, cy + size / 2 + 22.0f, on ? "On" : "Off", F(26.0f, on ? 0xFFFFE890 : kWhite, sprite::Align::Centre));
    ui::Flush();
    StatusBar();
    Header("Flashlight");
}

// --- Calendar ---------------------------------------------------------------
//
// The game's own date - San Andreas keeps a day and a month, and a day of
// the week - on a month to a page, with events of your own on any day. The
// game has no year: the phone starts at [Calendar] Year (2007, SP-RP's), and
// turns the year over when the game's December runs into January.

const char* const kMonthNames[12] = {"January", "February", "March",     "April",   "May",      "June",
                                     "July",    "August",   "September", "October", "November", "December"};

bool LeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int MonthDays(int y, int m) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && LeapYear(y) ? 29 : kDays[std::clamp(m, 1, 12) - 1];
}
// Days since a fixed day, for any date (Howard Hinnant's days_from_civil).
int DayNumber(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}

int CalendarYear() {
    auto& store = phone_data::Get().store;
    const int month = std::clamp<int>(*reinterpret_cast<const uint8_t*>(kClockMonth), 1, 12);
    int year = store.count("cal.year") ? atoi(store["cal.year"].c_str()) : config::Get().calendarYear;
    const int last = store.count("cal.month") ? atoi(store["cal.month"].c_str()) : month;
    if (month < last) ++year;
    if (month != last || !store.count("cal.year")) {
        store["cal.year"] = std::to_string(year);
        store["cal.month"] = std::to_string(month);
        Save();
    }
    return year;
}

std::string EventKey(int y, int m, int d) {
    return "cal." + std::to_string(y) + "." + std::to_string(m) + "." + std::to_string(d);
}
std::vector<std::string> EventsOn(int y, int m, int d) {
    std::vector<std::string> out;
    const auto& store = phone_data::Get().store;
    const auto it = store.find(EventKey(y, m, d));
    if (it == store.end()) return out;
    std::stringstream in(it->second);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) out.push_back(line);
    }
    return out;
}
void SaveEvents(int y, int m, int d, const std::vector<std::string>& events) {
    auto& store = phone_data::Get().store;
    std::string joined;
    for (const auto& e : events) joined += (joined.empty() ? "" : "\n") + e;
    if (joined.empty()) store.erase(EventKey(y, m, d));
    else store[EventKey(y, m, d)] = joined;
    Save();
}

void CalendarScreen() {
    const int year = CalendarYear();
    const int month = std::clamp<int>(*reinterpret_cast<const uint8_t*>(kClockMonth), 1, 12);
    const int today = std::clamp<int>(*reinterpret_cast<const uint8_t*>(kClockMonthDay), 1, MonthDays(year, month));
    const int weekday = phone_data::Now().day - 1;  // 0 Sunday
    if (!g.calYear) {
        g.calYear = year;
        g.calMonth = month;
        g.calDay = today;
    }
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, 0xFFE9EAEE);
    // The month, and arrows either side of it.
    const float top = kContentTop;
    ui::Gradient(0, top, ui::kScreenW, 58.0f, 0xFFF6F6F8, 0xFFCDCED4);
    // The month's name, which tapped goes back to today.
    const Rect title{70.0f, top, ui::kScreenW - 140.0f, 34.0f};
    ui::Label(ui::kScreenW / 2, top + 6.0f, std::string(kMonthNames[g.calMonth - 1]) + " " + std::to_string(g.calYear),
              F(20.0f, ui::Pressing(title) ? kBand : 0xFF2A2C33, sprite::Align::Centre, 0));
    if (ui::Tapped(title)) {
        g.calYear = year;
        g.calMonth = month;
        g.calDay = today;
    }
    const Rect prev{0, top, 60.0f, 34.0f}, next{ui::kScreenW - 60.0f, top, 60.0f, 34.0f};
    ui::Label(24.0f, top + 4.0f, "<", F(22.0f, ui::Pressing(prev) ? kBand : 0xFF4A4C55, sprite::Align::Centre, 0));
    ui::Label(ui::kScreenW - 24.0f, top + 4.0f, ">", F(22.0f, ui::Pressing(next) ? kBand : 0xFF4A4C55, sprite::Align::Centre, 0));
    auto moveMonth = [&](int by) {
        g.calMonth += by;
        if (g.calMonth < 1) { g.calMonth = 12; --g.calYear; }
        if (g.calMonth > 12) { g.calMonth = 1; ++g.calYear; }
        g.calDay = std::min(g.calDay, MonthDays(g.calYear, g.calMonth));
    };
    if (ui::Tapped(prev)) moveMonth(-1);
    if (ui::Tapped(next)) moveMonth(1);
    static const char* const kLetters[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const float cw = ui::kScreenW / 7.0f;
    for (int i = 0; i < 7; ++i) {
        ui::Label(cw * (i + 0.5f), top + 38.0f, kLetters[i], F(11.0f, 0xFF4A4C55, sprite::Align::Centre, 0));
    }
    // The grid: the first of the month under its day of the week.
    const int first = ((weekday - (DayNumber(year, month, today) - DayNumber(g.calYear, g.calMonth, 1))) % 7 + 7) % 7;
    const int days = MonthDays(g.calYear, g.calMonth);
    const float gy = top + 58.0f, ch = 44.0f;
    for (int cell = 0; cell < 42; ++cell) {
        const int d = cell - first + 1;
        const float cx = (cell % 7) * cw, cy = gy + (cell / 7) * ch;
        const bool inMonth = d >= 1 && d <= days;
        const bool isToday = inMonth && g.calYear == year && g.calMonth == month && d == today;
        const bool picked = inMonth && d == g.calDay;
        uint32_t fill = inMonth ? 0xFFF8F8FA : 0xFFD6D7DC;
        if (picked) fill = isToday ? 0xFF2F6FD0 : 0xFF6B7890;
        ui::Fill(cx, cy, cw, ch, fill);
        ui::Frame(cx, cy, cw + 1.0f, ch + 1.0f, 1.0f, 0xFFB4B6BE);
        if (isToday && !picked) ui::Frame(cx + 2.0f, cy + 2.0f, cw - 3.0f, ch - 3.0f, 2.0f, 0xFF2F6FD0);
        if (inMonth) {
            ui::Label(cx + cw / 2, cy + 12.0f, std::to_string(d),
                      F(18.0f, picked ? kWhite : 0xFF2A2C33, sprite::Align::Centre, 0));
            if (!EventsOn(g.calYear, g.calMonth, d).empty()) {
                ui::Image("disc", cx + cw / 2 - 3.0f, cy + ch - 9.0f, 6.0f, 6.0f, picked ? kWhite : 0xFF4A4C55);
            }
            if (ui::Tapped(Rect{cx, cy, cw, ch})) g.calDay = d;
        }
    }
    // The day picked: its events.
    const float ly = gy + 6 * ch + 4.0f;
    const auto events = EventsOn(g.calYear, g.calMonth, g.calDay);
    float y = ly;
    if (events.empty()) {
        ui::Label(ui::kScreenW / 2, y + 12.0f, "No Events", F(16.0f, 0xFF8A8C95, sprite::Align::Centre, 0));
    }
    for (size_t i = 0; i < events.size() && y < ui::kScreenH - 30.0f; ++i) {
        const Rect row{0, y, ui::kScreenW, 30.0f};
        if (ui::Pressing(row)) ui::Fill(0, y, ui::kScreenW, 30.0f, 0x302F6FD0);
        ui::Fill(12.0f, y + 9.0f, 4.0f, 12.0f, 0xFF2F6FD0);
        ui::Label(24.0f, y + 6.0f, ui::Fit(events[i], ui::kScreenW - 36.0f, F(15.0f, 0xFF2A2C33, sprite::Align::Left, 0)),
                  F(15.0f, 0xFF2A2C33, sprite::Align::Left, 0));
        if (ui::Tapped(row)) {
            g.calEvent = static_cast<int>(i);
            g.calText = events[i];
            Go(Screen::CalendarEvent);
        }
        y += 30.0f;
    }
    ui::Flush();
    StatusBar();
    if (Header("Calendar", nullptr, "+") == 1) {
        g.calEvent = -1;
        g.calText.clear();
        Go(Screen::CalendarEvent);
    }
}

void CalendarEventScreen() {
    Backdrop();
    const float top = kContentTop;
    char when[64];
    snprintf(when, sizeof when, "%s %d, %d", kMonthNames[g.calMonth - 1], g.calDay, g.calYear);
    ui::Label(ui::kScreenW / 2, top + 14.0f, when, F(16.0f, kMenuBlue, sprite::Align::Centre));
    TextBox(10.0f, top + 44.0f, 300.0f, 40.0f, g.calText, "Title", true, 60, false);
    if (g.calEvent >= 0 &&
        Button(20.0f, ui::kScreenH - 56.0f, 280.0f, 42.0f, kWastedRed, "Delete Event", 17.0f)) {
        auto events = EventsOn(g.calYear, g.calMonth, g.calDay);
        if (g.calEvent < static_cast<int>(events.size())) events.erase(events.begin() + g.calEvent);
        SaveEvents(g.calYear, g.calMonth, g.calDay, events);
        Go(Screen::Calendar);
        return;
    }
    ui::Flush();
    StatusBar();
    const int nav = Header(g.calEvent >= 0 ? "Edit Event" : "New Event", "Cancel", "Done");
    if (nav == -1) {
        Go(Screen::Calendar);
    } else if (nav == 1 || g.enter) {
        auto events = EventsOn(g.calYear, g.calMonth, g.calDay);
        std::string text = g.calText;
        for (char& c : text) {
            if (c == '\n' || c == '\r') c = ' ';
        }
        const bool blank = text.find_first_not_of(' ') == std::string::npos;
        if (g.calEvent >= 0 && g.calEvent < static_cast<int>(events.size())) {
            if (blank) events.erase(events.begin() + g.calEvent);
            else events[g.calEvent] = text;
        } else if (!blank) {
            events.push_back(text);
        }
        SaveEvents(g.calYear, g.calMonth, g.calDay, events);
        Go(Screen::Calendar);
    }
}

// --- Alarm ------------------------------------------------------------------
//
// The Clock's alarm goes off on the game's clock, once a game day, and rings
// with the ringtone picked in Settings until it is stopped.

int AlarmMinutes() {
    const auto& store = phone_data::Get().store;
    const auto it = store.find("alarm");
    return it == store.end() ? 7 * 60 : std::clamp(atoi(it->second.c_str()), 0, 24 * 60 - 1);
}

bool AlarmOn() {
    const auto& store = phone_data::Get().store;
    const auto it = store.find("alarm.on");
    return it != store.end() && it->second == "1";
}

void CheckAlarm() {
    const phone_data::Stamp now = phone_data::Now();
    const int minutes = now.hour * 60 + now.minute;
    if (minutes == g.alarmLastMinute) return;
    g.alarmLastMinute = minutes;
    if (!AlarmOn() || minutes != AlarmMinutes() || g.alarmRinging || !phone_data::Get().settings.poweredOn) return;
    g.alarmRinging = true;
    g.ringingFor = "Alarm";
    sound::Play(AlarmTone().sound, true);
    Subtitle("CJ's phone alarm is ringing.");
    logfile::Line("phone: alarm");
}

void CheckTimer() {
    auto& t = g.timer;
    if (!t.running || GetTickCount64() < t.end) return;
    t.running = false;
    g.alarmRinging = true;
    g.ringingFor = "Timer Done";
    sound::Play(AlarmTone().sound, true);
    Subtitle("CJ's phone: the timer is done.");
}

void StopAlarm() {
    g.alarmRinging = false;
    sound::Stop();
}

void AlarmScreen() {
    Wallpaper(0xFF6E6E6E);
    StatusBar();
    ui::Fill(0, kStatusH, ui::kScreenW, 140.0f, kPanel);
    char time[8];
    const int m = AlarmMinutes();
    snprintf(time, sizeof time, "%02d:%02d", m / 60, m % 60);
    ui::Label(ui::kScreenW / 2, kStatusH + 18.0f, time, Pricedown(56.0f));
    ui::Label(ui::kScreenW / 2, kStatusH + 90.0f, g.ringingFor, F(22.0f, kMenuBlue, sprite::Align::Centre));
    if (Button(20.0f, ui::kScreenH - 110.0f, 280.0f, 60.0f, kWastedRed, "Stop", 24.0f)) StopAlarm();
}

// --- Camera -----------------------------------------------------------------
//
// The Camera app is on the phone's own screen, upright, as the phone the
// iFruit copies has it: the live picture above, the last photo, the shutter
// and the switch to the front camera below. CJ holds the phone up, and the
// game's view goes over his shoulder (or, for a selfie, round in front of
// him) so you see him doing it. The picture on the phone is the world drawn
// a second time from the phone's lens, the way the game draws its mirrors
// (viewfinder.h).
//
// The mouse aims - it turns CJ, and tilts the phone - the wheel zooms, a
// click takes the picture, F turns the phone round, the right button lowers the phone,
// Esc or Backspace goes back. Pictures are the game's own photos: for the
// frame the picture is taken the game's camera is put at the lens, under the
// flash, and the game saves what it sees to the Gallery folder exactly as
// the in-game camera does. With [Photos] Shape=Portrait the saved picture is
// then cut to the viewfinder's upright shape.

constexpr uintptr_t kTakePhoto = 0xC8A7C1;          // CWeapon::ms_bTakePhoto
constexpr uintptr_t kSavePhotos = 0xBA6748 + 0xE8;  // FrontEndMenuManager.m_bSavePhotos
constexpr uintptr_t kSaveDone = 0xBA6748 + 0x5F;    // FrontEndMenuManager.m_bIsSaveDone
constexpr uint16_t kSetPlayerControl = 0x01B4;      // player, on
constexpr uint16_t kFixCamera = 0x015F;             // x, y, z, rotation x, y, z
constexpr uint16_t kPointCamera = 0x0160;           // x, y, z, switch style
constexpr uint16_t kRestoreCamera = 0x02EB;         // restore_camera_jumpcut
constexpr uint16_t kCameraFov = 0x0922;             // from, to, ms, ease
constexpr uint16_t kGetHeading = 0x0172;            // ped -> heading
constexpr uint16_t kSetHeading = 0x0173;            // ped, heading
constexpr uint16_t kOffsetFromChar = 0x04C4;        // ped, x, y, z -> x, y, z
constexpr uint16_t kInWater = 0x04AD;               // ped
constexpr int kJumpCut = 2;
const char* const kShutterSound = "1132";  // SOUND_CAMERA_SHOT
constexpr float kFov = 70.0f;              // the game's own, on foot
constexpr float kMaxZoom = 4.0f, kMaxSelfieZoom = 2.0f;
// The viewfinder: 3:4 under the camera's bar, as on the 2007 phone.
constexpr float kViewH = 427.0f;
constexpr float kViewAspect = ui::kScreenW / kViewH;

struct Vec3 {
    float x, y, z;
};

Vec3 OffsetFromPed(int handle, float x, float y, float z) {
    script::Locals out;
    script::Command(kOffsetFromChar, {handle, x, y, z, script::Arg::Local(0), script::Arg::Local(1),
                                      script::Arg::Local(2)},
                    &out);
    return {out.Float(0), out.Float(1), out.Float(2)};
}

float PedHeading(int handle) {
    script::Locals out;
    script::Command(kGetHeading, {handle, script::Arg::Local(0)}, &out);
    return out.Float(0);
}

void LoadLastPhoto();
void ScanGallery();
std::string GalleryFolder();
std::vector<std::string> GalleryFolders();
void Lower();

// Where the lens is, and what it looks at.
void Lens(int handle, Vec3& eye, Vec3& target) {
    const config::Config& c = config::Get();
    if (g.camera.selfie) {
        const config::Lens& l = c.selfieLens;
        eye = OffsetFromPed(handle, l.right, l.forward, l.up + g.camera.arm);
        target = OffsetFromPed(handle, 0.0f, 0.0f, 0.62f);
        return;
    }
    const config::Lens& l = c.cameraLens;
    eye = OffsetFromPed(handle, l.right, l.forward, l.up);
    const float h = g.camera.heading * 3.14159265f / 180.0f;
    const float p = g.camera.pitch * 3.14159265f / 180.0f;
    target = {eye.x - std::sin(h) * std::cos(p) * 20.0f, eye.y + std::cos(h) * std::cos(p) * 20.0f,
              eye.z + std::sin(p) * 20.0f};
}

// The field of view that sees what the viewfinder shows at this zoom.
float ZoomedFov() {
    const float half = kFov * 3.14159265f / 360.0f;
    return std::atan(std::tan(half) / g.camera.zoom) * 360.0f / 3.14159265f;
}

// The game's own view is left exactly as the player has it while the camera
// is up - only CJ turns. For the frames a picture is taken from, and only
// then, the game's camera goes to the lens (under the flash), and it is put
// straight back after.
void PlaceGameCamera(int handle) {
    if (g.camera.shot == 0 && !false) {
        if (g.camera.gameCameraMoved) {
            script::Command(kCameraFov, {kFov, kFov, 1, false});
            script::Command(kRestoreCamera, {});
            g.camera.gameCameraMoved = false;
        }
        return;
    }
    Vec3 eye{}, target{};
    Lens(handle, eye, target);
    const float fov = ZoomedFov();
    script::Command(kFixCamera, {eye.x, eye.y, eye.z, 0.0f, 0.0f, 0.0f});
    script::Command(kPointCamera, {target.x, target.y, target.z, kJumpCut});
    script::Command(kCameraFov, {fov, fov, 1, false});
    g.camera.gameCameraMoved = true;
}

bool CanStartCamera() {
    game::VehicleState vehicle{};
    if (game::PlayerVehicleState(vehicle)) {
        Notice("Get out of the vehicle to use the camera.");
        return false;
    }
    const uintptr_t ped = PlayerPed();
    if (!ped || !PlayerCanUsePhone()) return false;
    if (script::Command(kInWater, {script::PedHandle(ped)})) {
        Notice("Not in the water.");
        return false;
    }
    return input::Install();
}

bool g_cameraControlsHeld = false;
bool g_restoreCameraPending = false;
void StopPose();

void RestoreCameraControl() {
    if (g.camera.gameCameraMoved) {
        script::Command(kCameraFov, {kFov, kFov, 1, false});
        script::Command(kRestoreCamera, {});
        g.camera.gameCameraMoved = false;
    }
    if (g_cameraControlsHeld) {
        script::Command(kSetPlayerControl, {0, true});
        g_cameraControlsHeld = false;
    }
}

void StartCamera(bool selfie) {
    if (g.camera.on || !CanStartCamera()) return;
    g_restoreCameraPending = false;
    RestoreCameraControl();
    const int handle = script::PedHandle(PlayerPed());
    const picture::Picture thumb = g.camera.thumb;
    const std::string thumbPath = g.camera.thumbPath;
    g.camera = {};
    g.camera.thumb = thumb;
    g.camera.thumbPath = thumbPath;
    g.camera.on = true;
    g.camera.selfie = selfie;
    g.camera.heading = PedHeading(handle);
    g.camera.since = GetTickCount64();
    g.camera.returnTo = g.screen == Screen::CameraRoll ? Screen::CameraRoll : Screen::Home;
    if (!g.out) Open();
    if (!g.focused) Raise();
    StartMove(g.screen, Screen::Camera);
    g.screen = Screen::Camera;
    // The mouse aims from the middle of the screen; the phone's cursor is
    // not put back on its screen until the camera is done.
    g.placeCursor = false;
    const game::Point screen = game::ScreenSize();
    input::SetCursor({screen.x / 2, screen.y / 2});
    script::Command(kSetPlayerControl, {0, false});
    g_cameraControlsHeld = true;
    LoadLastPhoto();
    logfile::Line("phone: camera on (%s)", selfie ? "front" : "back");
}

// A picture half taken is finished off: the game's setting put back and its
// flag cleared, so the next photo the player takes is not affected.
void EndShot() {
    if (g.camera.shot == 0) return;
    if (g.camera.shot >= 2) {
        script::Command(0x0826, {1});  // DISPLAY_HUD
        script::Command(0x0581, {1});  // DISPLAY_RADAR
    }
    if (g.camera.shot >= 3) {
        *reinterpret_cast<bool*>(kTakePhoto) = false;
        *reinterpret_cast<bool*>(kSavePhotos) = g.camera.savedSetting;
        if (g.camera.savedDone) *reinterpret_cast<bool*>(kSaveDone) = true;
    }
    g.camera.shot = 0;
}

// `toPhone`: the player went back, so the phone stays up on the screen the
// camera was opened from. Otherwise the camera was taken away (a car, a
// call, a cutscene) and the rest of the phone is left to whoever did it.
void StopCamera(bool toPhone) {
    const bool wasOn = g.camera.on;
    g.camera.on = false;
    EndShot();
    viewfinder::Update(false);
    // Input can close the camera during HUD drawing. Restore immediately and
    // once at the next simulation boundary, after the last fixed-camera frame.
    g_restoreCameraPending = g_restoreCameraPending || g.camera.gameCameraMoved || g_cameraControlsHeld;
    RestoreCameraControl();
    if (!wasOn) return;
    StopPose();
    if (g.screen == Screen::Camera && !g.cameraPaused) {
        StartMove(Screen::Camera, g.camera.returnTo);
        g.screen = g.camera.returnTo;
    }
    g.placeCursor = true;
    if (toPhone && g.camera.taken > 0) {
        g.roll.scanned = 0;
        g.roll.index = 0;
    }
    logfile::Line("phone: camera off");
}

void TakePicture() {
    if (g.camera.shot != 0 || *reinterpret_cast<const bool*>(kTakePhoto)) return;
    // The click that opened the app is not a picture.
    if (GetTickCount64() - g.camera.since < 400) return;
    g.camera.shot = 1;
    g.camera.shotFrames = 0;
    g.camera.shotAt = GetTickCount64();
    sound::Play(kShutterSound);
}

// The mouse and keys while the camera is up, from the draw hook with the
// rest of the phone's input. What they do is applied before the next frame.
void CameraInput() {
    input::BeginFrame();
    const game::Point screen = game::ScreenSize();
    const input::Point cursor = input::Cursor();
    g.camera.lookX += cursor.x - screen.x / 2;
    g.camera.lookY += cursor.y - screen.y / 2;
    input::SetCursor({screen.x / 2, screen.y / 2});

    // The right button closes the camera (or selfie) and goes back to the
    // phone, as Escape does; everywhere else it lowers the phone.
    const bool lower = input::RightPressed();
    bool shoot = input::LeftPressed(), flip = false, close = lower, roll = false;
    input::Key key{};
    while (input::NextKey(key)) {
        if (key.vk == VK_ESCAPE || key.vk == VK_BACK) close = true;
        else if (key.vk == VK_RETURN || key.ch == ' ') shoot = true;
        // F turns the phone round. Not Tab: valkyrie-atmosphere has it.
        else if (key.ch == 'f' || key.ch == 'F') flip = true;
        else if (key.ch == 'g' || key.ch == 'G') roll = true;
    }
    if (close || roll) {
        StopCamera(true);
        if (lower) Lower();
        if (roll) {
            g.roll.scanned = 0;
            g.roll.index = 0;
            Go(Screen::CameraRoll);
        }
        return;
    }
    if (g.camera.shot != 0) return;
    if (flip) {
        g.camera.selfie = !g.camera.selfie;
        g.camera.pitch = 0.0f;
        g.camera.arm = 0.0f;
        g.camera.zoom = 1.0f;
        g.camera.flippedAt = GetTickCount64();
    }
    if (const int wheel = input::Wheel()) {
        const float most = g.camera.selfie ? kMaxSelfieZoom : kMaxZoom;
        g.camera.zoom = std::clamp(g.camera.zoom * std::pow(1.15f, static_cast<float>(wheel)), 1.0f, most);
    }
    if (shoot) TakePicture();
}

// Before CGame::Process: CJ turned and the game's camera placed for this
// frame, so the view and CJ never disagree by a frame as he turns.
void CameraBeforeFrame() {
    if (g_restoreCameraPending && !g.camera.on) {
        g_restoreCameraPending = false;
        // A mission/cutscene may have acquired control since the HUD exit.
        if (!PlayerCanUsePhone()) return;
        script::Command(kCameraFov, {kFov, kFov, 1, false});
        script::Command(kRestoreCamera, {});
        script::Command(kSetPlayerControl, {0, true});
        logfile::Line("phone: camera and player control restored at simulation boundary");
    }
    if (!g.camera.on) return;
    const uintptr_t ped = PlayerPed();
    if (!ped) return;
    const int handle = script::PedHandle(ped);
    const float sens = 0.12f * config::Get().cameraSensitivity / g.camera.zoom;
    const float dx = g.camera.lookX, dy = g.camera.lookY;
    g.camera.lookX = g.camera.lookY = 0.0f;
    if (g.camera.shot == 0) {
        g.camera.heading += dx * sens * (g.camera.selfie ? 1.0f : -1.0f);
        g.camera.heading = std::fmod(g.camera.heading + 360.0f, 360.0f);
        if (g.camera.selfie) {
            g.camera.arm = std::clamp(g.camera.arm - dy * 0.004f * config::Get().cameraSensitivity, -0.3f, 0.35f);
        } else {
            g.camera.pitch = std::clamp(g.camera.pitch - dy * sens, -60.0f, 60.0f);
        }
    }
    script::Command(kSetHeading, {handle, g.camera.heading});
    PlaceGameCamera(handle);
    // The game camera is at the lens from this frame on: the picture can be
    // asked for after it.
    if (g.camera.shot == 1) {
        g.camera.shot = 2;
        // The game grabs the whole finished frame for the picture: the HUD
        // and the radar are left out of it (and the phone draws nothing).
        script::Command(0x0826, {0});  // DISPLAY_HUD
        script::Command(0x0581, {0});  // DISPLAY_RADAR
    }
}

// After CGame::Process: the picture's progress, and the lens handed to the
// viewfinder for this frame's render.
// The world seen from the phone, for the handset to mirror: while the phone
// is up (and the Camera app is not using the same render), the second
// render looks from where the phone is in CJ's hands back toward the
// camera - what a mirror held there would show - and the lighting shader
// reflects it in the glass and the steel.
bool g_reflecting = false;

void UpdateReflection() {
    g_reflecting = false;
    const uintptr_t ped = PlayerPed();
    game::VehicleState vehicle{};
    if (!ped || !config::Get().reflections || !g.out || g.slide < 0.5f || !PlayerCanUsePhone() ||
        game::PlayerVehicleState(vehicle)) {
        viewfinder::Update(false);
        return;
    }
    // The phone on the screen faces the viewer, so its glass shows what is
    // behind the viewer: the world at the game's camera, looking straight
    // back from it - which turns as the camera does.
    const float* m = reinterpret_cast<const float*>(0xB6F028 + 0x974);  // TheCamera's matrix
    const float* cam = m + 12;                                           // its position
    viewfinder::Aim({cam[0], cam[1], cam[2]}, {cam[0] - m[4], cam[1] - m[5], cam[2] - m[6]});
    // Every frame, as far as the game draws: a mirror switched off and on
    // between frames makes the game's own picture flicker (its trees fade on
    // a per-frame count, and it frees the mirror's picture in between), and
    // the render's far plane is also the one the game's own draw list is
    // cut to.
    viewfinder::Reflection(1, 0.0f);
    viewfinder::Update(true);
    g_reflecting = true;
}

void UpdateCamera() {
    if (!g.camera.on) {
        UpdateReflection();
        return;
    }
    // The mirror render is the viewfinder's now: nothing reflects it.
    g_reflecting = false;
    const uintptr_t ped = PlayerPed();
    game::VehicleState vehicle{};
    if (!ped || !PlayerCanUsePhone() || game::PlayerVehicleState(vehicle) || !g.out || g.call.active ||
        g.alarmRinging || !phone_data::Get().settings.poweredOn) {
        StopCamera(false);
        return;
    }
    const int handle = script::PedHandle(ped);

    if (g.camera.shot == 2) {
        // The game only writes the file while the menu's "save photos" is
        // on; it is switched on for this one picture and put back after.
        g.camera.savedSetting = *reinterpret_cast<const bool*>(kSavePhotos);
        *reinterpret_cast<bool*>(kSavePhotos) = true;
        // Left set after a save, it makes the game drop every photo unsaved
        // (CPostEffects::Render); cleared for this one.
        g.camera.savedDone = *reinterpret_cast<const bool*>(kSaveDone);
        *reinterpret_cast<bool*>(kSaveDone) = false;
        *reinterpret_cast<bool*>(kTakePhoto) = true;
        g.camera.shot = 3;
    } else if (g.camera.shot == 3) {
        ++g.camera.shotFrames;
        const bool done = !*reinterpret_cast<const bool*>(kTakePhoto);
        if (done || g.camera.shotFrames > 60) {
            if (!done) {
                logfile::Line("phone: the game did not take the picture");
                g.camera.lastSaved = false;
            }
            EndShot();
            g.camera.flashAt = GetTickCount64();
            if (done) {
                ++g.camera.taken;
                ScanGallery();
                g.roll.scanned = 0;
                // The newest picture is this one, unless the game could not
                // write it.
                const std::string newest = g.roll.files.empty() ? std::string() : g.roll.files.front();
                WIN32_FILE_ATTRIBUTE_DATA info{};
                ULARGE_INTEGER written{};
                if (!newest.empty() && GetFileAttributesExA(newest.c_str(), GetFileExInfoStandard, &info)) {
                    written.LowPart = info.ftLastWriteTime.dwLowDateTime;
                    written.HighPart = info.ftLastWriteTime.dwHighDateTime;
                }
                FILETIME nowFt;
                GetSystemTimeAsFileTime(&nowFt);
                ULARGE_INTEGER now{};
                now.LowPart = nowFt.dwLowDateTime;
                now.HighPart = nowFt.dwHighDateTime;
                const bool fresh = written.QuadPart && now.QuadPart - written.QuadPart < 10ull * 10000000ull;
                if (fresh && config::Get().portraitPhotos) {
                    const game::Point screen = game::ScreenSize();
                    const float screenAspect = screen.y > 0 ? screen.x / screen.y : 16.0f / 9.0f;
                    picture::CropToAspect(newest, kViewAspect * screenAspect / viewfinder::Aspect());
                }
                g.camera.lastSaved = fresh;
                logfile::Line("phone: picture %s (gallery %s)", fresh ? newest.c_str() : "not written",
                              GalleryFolder().c_str());
                g.camera.thumbPath.clear();  // read it again, cut or not
                g.camera.refreshThumb = true;
            }
        }
    }
    if (g.camera.refreshThumb && picture::CropDone()) {
        g.camera.refreshThumb = false;
        g.camera.thumbPath.clear();
        LoadLastPhoto();
    }

    Vec3 eye{}, target{};
    Lens(handle, eye, target);
    viewfinder::Aim({eye.x, eye.y, eye.z}, {target.x, target.y, target.z});
    viewfinder::Lens();
    viewfinder::Update(!false && g.slide > 0.5f && config::Get().liveView);
}

// Completed-frame capture uses the completed main view, including its custom post effects.
// It never changes the mirror camera or adds a second world render.
IDirect3DTexture9* g_cameraPreview = nullptr;
UINT g_cameraPreviewSize[2] = {};
bool g_cameraPreviewReady = false;
void CaptureCameraPreview();

// The Camera app's screen, inside the phone.
void CameraScreen() {
    if (!g.camera.on) {
        if (!g.cameraPaused) {
            Go(Screen::Home);
            return;
        }
        if (g.focused) {
            // Raised again: the camera carries on, facing as it was.
            const Screen back = g.cameraPausedReturn;
            g.cameraPaused = false;
            StartCamera(g.cameraPausedSelfie);
            if (g.camera.on) g.camera.returnTo = back;
            else Go(back);
            return;
        }
        // Lowered: the viewfinder is dark until it is raised.
        ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, kBlack);
        ui::Label(ui::kScreenW / 2, ui::kScreenH / 2 - 10.0f, "Camera", F(18.0f, kGrey, sprite::Align::Centre));
        return;
    }
    const ULONGLONG now = GetTickCount64();
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, kBlack);

    // The picture, filling the viewfinder at the zoom; the view is wider
    // than it is, so the sides fall outside and are cut off.
    const uintptr_t live = viewfinder::Texture();
    const bool preview = false && g_cameraPreviewReady;
    if (preview) {
        ui::Flush();
        const float x0 = ui::ToPixelX(0), y0 = ui::ToPixelY(0);
        const float x1 = ui::ToPixelX(ui::kScreenW), y1 = ui::ToPixelY(kViewH);
        const float h = y1 - y0;
        const float w = h * g_cameraPreviewSize[0] / g_cameraPreviewSize[1];
        const float cx = (x0 + x1) / 2;
        sprite::BeginScissor(x0, y0, x1, y1);
        DrawDeviceTexture(g_cameraPreview, cx - w / 2, y0, cx + w / 2, y1);
        sprite::EndScissor();
    } else if (live) {
        const float x0 = ui::ToPixelX(0), y0 = ui::ToPixelY(0);
        const float x1 = ui::ToPixelX(ui::kScreenW), y1 = ui::ToPixelY(kViewH);
        const float h = (y1 - y0) * g.camera.zoom, w = h * viewfinder::Aspect();
        const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        sprite::BeginScissor(x0, y0, x1, y1);
        sprite::Draw(live, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        sprite::EndScissor();
    } else {
        ui::Fill(0, 0, ui::kScreenW, kViewH, 0xFF1C1C1E);
    }

    // The shutter's two leaves: shut when the app opens and when a picture
    // is taken or the camera turned round, then drawn apart.
    const ULONGLONG opened = std::max({g.camera.since, g.camera.flashAt, g.camera.flippedAt});
    float shut = 0.0f;
    if (g.camera.shot != 0 || (!live && !preview)) shut = 1.0f;
    else if (now - opened < 350) shut = 1.0f - (now - opened) / 350.0f;
    if (shut > 0.0f) {
        const float half = kViewH / 2 * shut;
        ui::Fill(0, 0, ui::kScreenW, half, 0xFF2A2A2E);
        ui::Fill(0, kViewH - half, ui::kScreenW, half, 0xFF2A2A2E);
        if (shut > 0.95f) ui::Fill(0, kViewH / 2 - 1.0f, ui::kScreenW, 2.0f, 0xFF4A4A4E);
    }

    // What the camera is doing, along the top of the picture.
    if (g.camera.zoom > 1.01f) {
        char zoom[16];
        snprintf(zoom, sizeof zoom, "%.1fx", g.camera.zoom);
        ui::Label(ui::kScreenW / 2, 10.0f, zoom, F(15.0f, 0xFFFFCC33, sprite::Align::Centre));
    }
    if (g.camera.flashAt && now - g.camera.flashAt < 2000) {
        ui::Fill(0, kViewH - 34.0f, ui::kScreenW, 28.0f, kPanel);
        ui::Label(ui::kScreenW / 2, kViewH - 30.0f, g.camera.lastSaved ? "Saved to Camera Roll" : "The photo was not saved",
                  F(14.0f, g.camera.lastSaved ? kWhite : 0xFFFF8080, sprite::Align::Centre));
    }

    // The bar: the last picture, the shutter, the switch.
    const float barTop = kViewH, barH = ui::kScreenH - kViewH, cy = barTop + barH / 2;
    ui::Gradient(0, barTop, ui::kScreenW, barH, 0xFF4A4C52, 0xFF1E1F22);
    ui::Fill(0, barTop, ui::kScreenW, 1.0f, 0xFF6A6C72);
    const float tx = 14.0f, ts = 38.0f;
    if (g.camera.thumb.texture) {
        const float aspect =
            g.camera.thumb.width > 0 ? static_cast<float>(g.camera.thumb.height) / g.camera.thumb.width : 0.75f;
        // Square, from the middle.
        float w = ts, h = ts;
        if (aspect < 1.0f) w = ts / aspect;
        else h = ts * aspect;
        sprite::BeginScissor(ui::ToPixelX(tx), ui::ToPixelY(cy - ts / 2), ui::ToPixelX(tx + ts),
                             ui::ToPixelY(cy + ts / 2));
        sprite::Draw(g.camera.thumb.texture, ui::ToPixelX(tx + ts / 2 - w / 2), ui::ToPixelY(cy - h / 2),
                     ui::ToPixelX(tx + ts / 2 + w / 2), ui::ToPixelY(cy + h / 2));
        sprite::EndScissor();
    } else {
        ui::Fill(tx, cy - ts / 2, ts, ts, 0xFF2A2A2E);
    }
    ui::Frame(tx - 2.0f, cy - ts / 2 - 2.0f, ts + 4.0f, ts + 4.0f, 2.0f, kBlack);
    const bool pressed = input::LeftHeld() || g.camera.shot != 0;
    ui::Image("cam_shutter", ui::kScreenW / 2 - 21.0f, cy - 21.0f, 42.0f, 42.0f, pressed ? 0xFFB0B0B0 : kWhite);
    ui::Image("disc", ui::kScreenW - 54.0f, cy - 20.0f, 40.0f, 40.0f, g.camera.selfie ? 0xFF5A7FB0 : 0xFF3A3A3E);
    ui::Image("g_flip", ui::kScreenW - 47.0f, cy - 13.0f, 26.0f, 26.0f);
    ui::Flush();
}

// Over the whole screen while the picture is taken: the flash, which also
// hides the frames the game's camera spends at the lens.
void DrawFlash() {
    if (!g.camera.on) return;
    const game::Point screen = game::ScreenSize();
    // Only once the picture is taken: during it the frame is the picture.
    uint32_t alpha = 0;
    if (config::Get().photoFlash && g.camera.shot == 0 && g.camera.flashAt) {
        const ULONGLONG since = GetTickCount64() - g.camera.flashAt;
        if (since < 250) alpha = static_cast<uint32_t>(255.0f * (1.0f - since / 250.0f));
    }
    if (!alpha) return;
    ui::SetScreen(0.0f, 0.0f, 1.0f);
    ui::NoClip();
    ui::Fill(0.0f, 0.0f, screen.x, screen.y, (alpha << 24) | 0xFFFFFF);
}

// --- CJ's poses ---------------------------------------------------------------
//
// While the phone is up CJ looks at it; for a picture he holds it up, for a
// selfie he holds it out. The animations are the game's own, named in
// [Animations]. At his ear for a call the game's phone task has him, and
// with the phone lowered he is left alone so he can walk.

constexpr uint16_t kPlayAnim = 0x0605;          // ped, anim, file, blend, loop, lock x, lock y, keep, ms
constexpr uint16_t kClearTasks = 0x0687;
constexpr uint16_t kPlayingAnim = 0x0611;       // ped, anim
constexpr uint16_t kPauseAnim = 0x0612;         // ped, anim, paused
constexpr uint16_t kAnimTime = 0x0613;          // ped, anim -> 0..1
constexpr uint16_t kRequestAnimation = 0x04ED;
constexpr uint16_t kAnimationLoaded = 0x04EE;
constexpr uint16_t kRemoveAnimation = 0x04EF;
constexpr uint16_t kInAir = 0x0818;

enum class Pose { None, Use, Camera, Selfie };

void Lower();

struct PoseState {
    Pose now = Pose::None;
    config::Anim anim;
    bool playing = false;    // the animation is on him (not skipped as broken)
    bool seen = false;       // the game has been seen playing it
    bool held = false;       // stopped on its last frame
    ULONGLONG started = 0, checked = 0;
    int tries = 0;           // times this pose's animation has been asked for without showing
    int failedPoses = 0;     // poses in a row whose animation never showed at all
    std::string loadedFile;  // an .ifp this asked the game to load
    std::string broken;      // an animation that would not play; not tried again
    // Every animation put on him since the phone came up. Once it is put
    // away they are looked for for a while and taken off him, so none of
    // them - a held camera pose under one that never started - can leave
    // him stuck.
    std::vector<std::string> used;
    ULONGLONG sweepUntil = 0, sweptAt = 0, sweepFrom = 0;
    int sweeps = 0;
} g_pose;

bool IsPed(const std::string& file) { return _stricmp(file.c_str(), "ped") == 0; }

void ReleaseAnimFile() {
    if (!g_pose.loadedFile.empty()) {
        script::Command(kRemoveAnimation, {g_pose.loadedFile.c_str()});
        g_pose.loadedFile.clear();
    }
}

// Let go of him - but only out of our own animation: if something else has
// him now (a mission, a fall, a punch), that is left alone.
void StopPose() {
    if (g_pose.now != Pose::None && g_pose.playing) logfile::Line("phone: pose off (%s)", g_pose.anim.name.c_str());
    if (g_pose.now != Pose::None && g_pose.playing) {
        if (const uintptr_t ped = PlayerPed()) {
            const int handle = script::PedHandle(ped);
            if (script::Command(kPlayingAnim, {handle, g_pose.anim.name.c_str()})) {
                // Held on its last frame, it is paused: let go of first, or
                // clearing his tasks can leave it on him.
                if (g_pose.held) script::Command(kPauseAnim, {handle, g_pose.anim.name.c_str(), false});
                script::Command(kClearTasks, {handle});
            }
        }
    }
    g_pose.now = Pose::None;
    g_pose.playing = false;
    ReleaseAnimFile();
    if (!g_pose.used.empty()) {
        g_pose.sweepFrom = GetTickCount64() + 1500;
        g_pose.sweepUntil = g_pose.sweepFrom + 3000;
        g_pose.sweeps = 0;
    }
}

// Once the phone has been away long enough for its animation to have blended
// out, anything of it still on him is taken off - once. The game can go on
// reporting an animation for a while after it has let go of him, so this
// never repeats: clearing his tasks over and over is itself what holds him
// still.
void SweepPoses(int handle, ULONGLONG now) {
    if (g_pose.used.empty()) return;
    // Never during a call: clearing his tasks would take the phone from his
    // ear. What is left of a pose from before it is the ear task's to replace.
    if (g.call.active || g.phoneAtEar) {
        g_pose.used.clear();
        return;
    }
    if (now >= g_pose.sweepUntil || g_pose.sweeps > 0) {
        g_pose.used.clear();
        return;
    }
    if (now < g_pose.sweepFrom) return;
    // Not while he is getting into a car, falling, in a cutscene or the
    // menu: waited for, until the sweep's time runs out.
    if (!PlayerCanUsePhone() || InTransition(PlayerPed())) return;
    for (const std::string& name : g_pose.used) {
        if (!script::Command(kPlayingAnim, {handle, name.c_str()})) continue;
        logfile::Line("phone: %s still on CJ %llu ms after the phone went away - cleared", name.c_str(),
                      now - g_pose.sweepFrom + 1500);
        script::Command(kClearTasks, {handle});
        break;
    }
    g_pose.sweeps = 1;
}

void UpdatePose() {
    const uintptr_t ped = PlayerPed();
    if (!ped) {
        g_pose.now = Pose::None;
        g_pose.playing = false;
        return;
    }
    const int handle = script::PedHandle(ped);
    Pose want = Pose::None;
    game::VehicleState vehicle{};
    const bool free = g.holding && !g.phoneAtEar && !(g.call.active && !g.call.speaker) &&
                      PlayerCanUsePhone() && !game::PlayerVehicleState(vehicle) && !InTransition(ped) &&
                      !script::Command(kInWater, {handle}) && !script::Command(kInAir, {handle});
    if (free) {
        if (g.camera.on) want = g.camera.selfie ? Pose::Selfie : Pose::Camera;
        else if (g.out && g.focused) want = Pose::Use;
    }
    const ULONGLONG now = GetTickCount64();
    if (want == Pose::None && g_pose.now == Pose::None) SweepPoses(handle, now);
    if (want == g_pose.now) {
        if (want == Pose::None || !g_pose.playing) return;
        const bool on = script::Command(kPlayingAnim, {handle, g_pose.anim.name.c_str()});
        if (!g_pose.seen) {
            if (on) {
                g_pose.seen = true;
                g_pose.checked = now;
                g_pose.failedPoses = 0;
                if (g_pose.tries > 1) logfile::Line("phone: %s took on try %d", g_pose.anim.name.c_str(), g_pose.tries);
            } else if (now - g_pose.started > 1000) {
                // Not on him yet: the game turns a task down while he is
                // mid-step, turning or still blending out of the last one.
                // Asked again until it takes - the phone is never up in
                // his hand with him doing nothing. Only an animation that
                // never once shows, pose after pose, is taken for a
                // mistake in [Animations].
                if (g_pose.tries >= 6) {
                    if (++g_pose.failedPoses >= 3) {
                        logfile::Line("phone: animation %s in %s.ifp never plays - check [Animations]",
                                      g_pose.anim.name.c_str(), g_pose.anim.file.c_str());
                        g_pose.broken = g_pose.anim.file + "/" + g_pose.anim.name;
                        g_pose.playing = false;
                        ReleaseAnimFile();
                        return;
                    }
                    g_pose.tries = 0;
                }
                if (g_pose.tries == 1) logfile::Line("phone: %s not on CJ yet - asking again", g_pose.anim.name.c_str());
                const config::Anim& a = g_pose.anim;
                script::Command(kPlayAnim, {handle, a.name.c_str(), a.file.c_str(), 4.0f, a.loop, false, false, !a.loop, -1});
                ++g_pose.tries;
                g_pose.started = now;
            }
            return;
        }
        // One that holds its last frame is stopped there, so it is held for
        // as long as the pose lasts rather than played over again.
        if (!g_pose.anim.loop && on && !g_pose.held) {
            script::Locals t;
            script::Command(kAnimTime, {handle, g_pose.anim.name.c_str(), script::Arg::Local(0)}, &t);
            if (t.Float(0) >= 0.95f) {
                script::Command(kPauseAnim, {handle, g_pose.anim.name.c_str(), true});
                g_pose.held = true;
            }
        }
        // Knocked out of it - shoved, hit, fallen over: the phone comes down
        // and he has his controls back, rather than being put straight back
        // into the pose from wherever he landed.
        if (now - g_pose.checked > 500) {
            g_pose.checked = now;
            if (!on) {
                logfile::Line("phone: CJ was knocked out of the pose - lowering the phone");
                g_pose.now = Pose::None;
                g_pose.playing = false;
                ReleaseAnimFile();
                Lower();
            }
        }
        return;
    }
    if (want == Pose::None) {
        StopPose();
        return;
    }
    const config::Config& c = config::Get();
    const config::Anim& a = want == Pose::Use ? c.useAnim : want == Pose::Camera ? c.cameraAnim : c.selfieAnim;
    if (a.name.empty() || _stricmp(g_pose.broken.c_str(), (a.file + "/" + a.name).c_str()) == 0) {
        // Nothing to play: take him out of the last pose and hold the phone.
        StopPose();
        g_pose.now = want;
        return;
    }
    if (!IsPed(a.file)) {
        // Loaded over a few frames; the pose before it is kept meanwhile.
        script::Command(kRequestAnimation, {a.file.c_str()});
        if (!script::Command(kAnimationLoaded, {a.file.c_str()})) return;
    }
    // Out of the last pose before the next: a held one left under a new one
    // that fails to start would otherwise stay on him.
    if (g_pose.now != Pose::None && g_pose.playing &&
        script::Command(kPlayingAnim, {handle, g_pose.anim.name.c_str()})) {
        if (g_pose.held) script::Command(kPauseAnim, {handle, g_pose.anim.name.c_str(), false});
        script::Command(kClearTasks, {handle});
    }
    const std::string previousFile = g_pose.loadedFile;
    if (std::find(g_pose.used.begin(), g_pose.used.end(), a.name) == g_pose.used.end()) g_pose.used.push_back(a.name);
    g_pose.sweepUntil = 0;
    script::Command(kPlayAnim, {handle, a.name.c_str(), a.file.c_str(), 4.0f, a.loop, false, false, !a.loop, -1});
    logfile::Line("phone: pose %s from %s (%s)", a.name.c_str(), a.file.c_str(), a.loop ? "looping" : "held");
    g_pose.now = want;
    g_pose.anim = a;
    g_pose.playing = true;
    g_pose.seen = false;
    g_pose.held = false;
    g_pose.tries = 1;
    g_pose.started = g_pose.checked = now;
    g_pose.loadedFile = IsPed(a.file) ? std::string() : a.file;
    if (!previousFile.empty() && _stricmp(previousFile.c_str(), a.file.c_str()) != 0) {
        script::Command(kRemoveAnimation, {previousFile.c_str()});
    }
}

// --- Camera Roll ------------------------------------------------------------
//
// The pictures the game's camera has taken, from the Gallery folder, newest
// first. Pictures are read a couple a frame so the list opens straight away.

std::string GalleryFolder() {
    const char* user = reinterpret_cast<const char*(__cdecl*)()>(kUserFolder)();
    std::string folder = user ? user : g_gameDir;
    if (!folder.empty() && folder.back() != '\\') folder += '\\';
    return folder + "Gallery\\";
}

void ScanGallery() {
    struct Found {
        std::string path;
        FILETIME at;
    };
    std::vector<Found> found;
    WIN32_FIND_DATAA fd;
    for (const std::string& folder : GalleryFolders()) {
        HANDLE h = FindFirstFileA((folder + "*.jpg").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            found.push_back({folder + fd.cFileName, fd.ftLastWriteTime});
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(),
              [](const Found& a, const Found& b) { return CompareFileTime(&a.at, &b.at) > 0; });
    g.roll.files.clear();
    for (const auto& f : found) g.roll.files.push_back(f.path);
}

void LoadLastPhoto() {
    ScanGallery();
    const std::string newest = g.roll.files.empty() ? std::string() : g.roll.files.front();
    if (newest == g.camera.thumbPath) return;
    picture::Free(g.camera.thumb);
    g.camera.thumbPath = newest;
    if (!newest.empty()) g.camera.thumb = picture::Load(newest, 256);
}

void CameraRollScreen() {
    Backdrop();
    if (GetTickCount64() - g.roll.scanned > 3000) {
        ScanGallery();
        g.roll.scanned = GetTickCount64();
    }
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom) + 6.0f;
    const float y0 = y;
    // Four across in squares, as the phone's own Camera Roll has them,
    // each cut from the middle of its picture whatever its shape.
    const float tw = 75.0f, th = 75.0f, gap = 4.0f;
    int loads = 2;
    for (size_t i = 0; i < g.roll.files.size(); ++i) {
        const float x = gap + (i % 4) * (tw + gap);
        const float ty = y + (i / 4) * (th + gap);
        if (ty + th < top || ty > bottom) continue;
        const std::string& path = g.roll.files[i];
        auto it = g.roll.thumbs.find(path);
        if (it == g.roll.thumbs.end() && loads-- > 0) {
            it = g.roll.thumbs.emplace(path, picture::Load(path, 128)).first;
        }
        const Rect r{x, ty, tw, th};
        if (it != g.roll.thumbs.end() && it->second.texture) {
            const picture::Picture& pic = it->second;
            const float aspect = pic.width > 0 ? static_cast<float>(pic.height) / pic.width : 0.75f;
            float dw = tw, dh = th;
            if (aspect < 1.0f) dw = tw / aspect;
            else dh = th * aspect;
            // Cut to the tile, and to the list like everything else in it.
            const float clipTop = std::max(ty, top);
            if (clipTop < ty + th) {
                sprite::BeginScissor(ui::ToPixelX(x), ui::ToPixelY(clipTop), ui::ToPixelX(x + tw),
                                     ui::ToPixelY(ty + th));
                sprite::Draw(pic.texture, ui::ToPixelX(x + tw / 2 - dw / 2), ui::ToPixelY(ty + th / 2 - dh / 2),
                             ui::ToPixelX(x + tw / 2 + dw / 2), ui::ToPixelY(ty + th / 2 + dh / 2),
                             ui::Pressing(r) ? 0xFF909090 : kWhite);
                sprite::EndScissor();
            }
        } else {
            ui::Fill(x, ty, tw, th, kPanelDark);
        }
        ui::Frame(x, ty, tw, th, 2.0f, kBlack);
        if (ui::Tapped(r)) {
            g.roll.index = static_cast<int>(i);
            Go(Screen::PhotoOpen);
        }
    }
    if (g.roll.files.empty()) Empty("No Photos");
    EndList(top, bottom, ((g.roll.files.size() + 3) / 4) * (th + gap) + 12.0f + (y - y0));
    ui::Flush();
    StatusBar();
    const int bar = Header("Camera Roll", "Photos", "g_camera");
    if (bar == -1) Go(Screen::Photos);
    else if (bar == 1) StartCamera(false);
}

void PhotoOpenScreen() {
    if (g.roll.index < 0 || g.roll.index >= static_cast<int>(g.roll.files.size())) {
        Go(Screen::CameraRoll);
        return;
    }
    const std::string& path = g.roll.files[g.roll.index];
    if (g.roll.viewPath != path) {
        picture::Free(g.roll.view);
        g.roll.view = picture::Load(path, 1024);
        g.roll.viewPath = path;
    }
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, kBlack);
    if (g.roll.view.texture && g.roll.view.width > 0) {
        // The whole picture, as large as the screen takes it: a wide one
        // across, an upright one top to bottom.
        const float aspect = static_cast<float>(g.roll.view.height) / g.roll.view.width;
        float w = ui::kScreenW, h = w * aspect;
        if (h > ui::kScreenH) {
            h = ui::kScreenH;
            w = h / aspect;
        }
        const float x = (ui::kScreenW - w) / 2, y = (ui::kScreenH - h) / 2;
        sprite::Draw(g.roll.view.texture, ui::ToPixelX(x), ui::ToPixelY(y), ui::ToPixelX(x + w),
                     ui::ToPixelY(y + h));
    }
    StatusBar();
    const float barTop = ui::kScreenH - 56.0f;
    ui::Fill(0, barTop, ui::kScreenW, 56.0f, kPanel);
    const size_t n = g.roll.files.size();
    if (Button(12.0f, barTop + 8.0f, 80.0f, 40.0f, 0xFF3A3A3E, "<", 20.0f)) g.roll.index = (g.roll.index + 1) % n;
    if (Button(228.0f, barTop + 8.0f, 80.0f, 40.0f, 0xFF3A3A3E, ">", 20.0f)) g.roll.index = (g.roll.index + n - 1) % n;
    ui::Label(ui::kScreenW / 2, barTop + 18.0f, std::to_string(n - g.roll.index) + " of " + std::to_string(n),
              F(15.0f, kWhite, sprite::Align::Centre));
    if (g.confirmDelete) {
        float by = 0.0f;
        Alert("Delete Photo", "The picture goes to the Windows Recycle Bin.", by);
        if (Button(40.0f, by, 112.0f, 42.0f, 0xFF3A3A3E, "Cancel") || TakeBack()) g.confirmDelete = false;
        else if (Button(168.0f, by, 112.0f, 42.0f, kWastedRed, "Delete")) {
            g.confirmDelete = false;
            // Into the Recycle Bin rather than gone, so it can be had back.
            std::string from = path;
            from.push_back('\0');
            SHFILEOPSTRUCTA op{};
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            const int result = SHFileOperationA(&op);
            logfile::Line("phone: photo %s to the recycle bin (%d)", path.c_str(), result);
            picture::Free(g.roll.view);
            g.roll.viewPath.clear();
            auto thumb = g.roll.thumbs.find(path);
            if (thumb != g.roll.thumbs.end()) {
                picture::Free(thumb->second);
                g.roll.thumbs.erase(thumb);
            }
            ScanGallery();
            g.roll.scanned = GetTickCount64();
            if (g.roll.files.empty()) {
                Go(Screen::CameraRoll);
                return;
            }
            g.roll.index = std::min(g.roll.index, static_cast<int>(g.roll.files.size()) - 1);
        }
        StatusBar();
        return;
    }
    const int bar = Header("Photo", "Camera Roll", "g_trash");
    if (bar == -1) Go(Screen::CameraRoll);
    else if (bar == 1) g.confirmDelete = true;
}

void ReleaseRoll() {
    for (auto& kv : g.roll.thumbs) picture::Free(kv.second);
    g.roll.thumbs.clear();
    picture::Free(g.roll.view);
    g.roll.viewPath.clear();
}

// --- Settings ---------------------------------------------------------------

// A row of a grouped list: a rounded-off panel, a picture, the name, and
// what it is set to.
bool SettingRow(float y, const char* icon, const std::string& title, const std::string& detail, bool first,
                bool last) {
    const Rect r{10.0f, y, ui::kScreenW - 20.0f, 44.0f};
    ui::Fill(r.x, r.y, r.w, r.h, ui::Pressing(r) ? kMenuBlueBar : 0xC8101012);
    if (first) ui::Fill(r.x, r.y, r.w, 2.0f, kBlack);
    if (last) ui::Fill(r.x, r.y + r.h - 2.0f, r.w, 2.0f, kBlack);
    else ui::Fill(r.x + 44.0f, r.y + r.h - 1.0f, r.w - 44.0f, 1.0f, kSeparator);
    ui::Fill(r.x, r.y, 2.0f, r.h, kBlack);
    ui::Fill(r.x + r.w - 2.0f, r.y, 2.0f, r.h, kBlack);
    ui::Image(icon, r.x + 8.0f, y + 8.0f, 28.0f, 28.0f);
    const Font df = F(14.0f, kMenuBlue, sprite::Align::Right);
    const float dw = detail.empty() ? 0.0f : ui::TextWidth(detail, df) + 10.0f;
    ui::Label(r.x + 44.0f, y + 13.0f, ui::Fit(title, r.w - 60.0f - dw, F(16.0f)), F(16.0f));
    if (!detail.empty()) ui::Label(r.x + r.w - 12.0f, y + 15.0f, detail, df);
    return ui::Tapped(r);
}

void SettingsScreen() {
    auto& s = phone_data::Get().settings;
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom) + 14.0f;
    const float y0 = y;
    if (!KeypadSkin()) {
        if (SettingRow(y, "g_wallpaper", "Wallpaper", kWallpaperNames[s.wallpaper], true, true)) {
            Go(Screen::Photos);
            return;
        }
        y += 58.0f;
    }
    // Sounds: each tone opens the list to pick from; then the phone's own
    // ticks, as the phone's Sounds settings have them.
    struct ToneRow {
        const char* title;
        const std::vector<config::Tone>* tones;
        const char* key;
    };
    const ToneRow tones[] = {{"Ringtone", &config::Get().ringtones, "ringtone"},
                             {"Text Tone", &config::Get().textTones, "texttone"},
                             {"Alarm Tone", &config::Get().ringtones, "alarmtone"}};
    for (int i = 0; i < 3; ++i) {
        const ToneRow& t = tones[i];
        const config::Tone& picked = PickedTone(*t.tones, t.key);
        if (SettingRow(y, "g_sounds", t.title, picked.name, i == 0, false)) {
            g.toneKind = i;
            g.toneBack = Screen::Settings;
            Go(Screen::Tones);
            return;
        }
        y += 44.0f;
    }
    const bool lockSounds = Flag("locksounds", true), keyClicks = Flag("keyclicks", true);
    if (SettingRow(y, "g_lock", "Lock Sounds", lockSounds ? "On" : "Off", false, false)) {
        SetFlag("locksounds", !lockSounds);
    }
    y += 44.0f;
    if (SettingRow(y, "g_keypad", "Keyboard Clicks", keyClicks ? "On" : "Off", false, true)) {
        SetFlag("keyclicks", !keyClicks);
        if (!keyClicks && !s.silent) sound::Click(sound::Tone::Key);
    }
    y += 58.0f;
    // What the side buttons set, here too: the ring switch and the volume.
    if (SettingRow(y, "g_alarm", "Silent", s.silent ? "On" : "Off", true, false)) ToggleSilent();
    y += 44.0f;
    const std::string volume = s.volume == 0 ? "Off" : std::to_string(s.volume) + " / 10";
    if (SettingRow(y, "g_speaker", "Volume", volume, false, true)) ChangeVolume(s.volume >= 10 ? -10 : 1);
    y += 58.0f;
    const bool autoBright = Flag("autobright", true);
    if (SettingRow(y, "g_world", "Auto-Brightness", autoBright ? "On" : "Off", true, true)) {
        SetFlag("autobright", !autoBright);
    }
    y += 58.0f;
    // Auto-Lock: each tap steps to the next choice.
    const int minutes = AutoLockMinutes();
    const std::string lockAfter = minutes == 0 ? "Never" : std::to_string(minutes) + (minutes == 1 ? " Minute" : " Minutes");
    if (SettingRow(y, "g_timer", "Auto-Lock", lockAfter, true, false)) {
        const int count = static_cast<int>(sizeof kAutoLockChoices / sizeof kAutoLockChoices[0]);
        int next = 0;
        for (int i = 0; i < count; ++i) {
            if (kAutoLockChoices[i] == minutes) next = (i + 1) % count;
        }
        phone_data::Get().store["autolock"] = std::to_string(kAutoLockChoices[next]);
        Save();
    }
    y += 44.0f;
    if (SettingRow(y, "g_lock", "Slide to Unlock", s.slideToUnlock ? "On" : "Off", false, true)) {
        s.slideToUnlock = !s.slideToUnlock;
        Save();
    }
    y += 58.0f;
    if (SettingRow(y, "g_about", "About", "", true, true)) {
        Go(Screen::About);
        return;
    }
    y += 64.0f;
    ui::Label(ui::kScreenW / 2, y, "Hold the button on the right side", F(14.0f, kGrey, sprite::Align::Centre, 0));
    ui::Label(ui::kScreenW / 2, y + 18.0f, "to turn the phone off.", F(14.0f, kGrey, sprite::Align::Centre, 0));
    y += 50.0f;
    EndList(top, bottom, y - y0 + 14.0f);
    ui::Flush();
    StatusBar();
    Header("Settings");
}

// --- Saving ---------------------------------------------------------------
//
// The save number asks first, then puts the phone down and opens the game's
// own save menu, as a safehouse's save point does - so the game is saved
// where CJ is standing.

constexpr uintptr_t kOnMissionFlag = 0xA476AC;  // CTheScripts::OnAMissionFlag
constexpr uintptr_t kScriptSpace = 0xA49960;    // CTheScripts::ScriptSpace
constexpr uint16_t kActivateSaveMenu = 0x03D8;

bool OnMission() {
    // The offset of main.scm's own "on a mission" variable, which is 1 while
    // one runs.
    const uint32_t flag = *reinterpret_cast<const uint32_t*>(kOnMissionFlag);
    if (flag == 0 || flag > 0x40000) return false;
    return *reinterpret_cast<const int32_t*>(kScriptSpace + flag) == 1;
}

// A box in the middle of the screen, as the phone asks its questions.
void Alert(const char* title, const std::string& text, float& buttonsY) {
    const float x = 26.0f, w = ui::kScreenW - 52.0f;
    const Font body = F(15.0f, kWhite, sprite::Align::Centre, 0);
    std::vector<std::string> lines = sprite::Wrap(text.c_str(), (w - 28.0f) * ui::PixelsPerPoint(),
                                                   [&] {
                                                       sprite::TextStyle st;
                                                       st.height = body.size * ui::PixelsPerPoint();
                                                       st.face = body.face;
                                                       return st;
                                                   }());
    const float h = 60.0f + lines.size() * 19.0f + 64.0f;
    // Rises into place as the screen behind it dims.
    static std::string shown;
    static unsigned shownFrame = 0;
    static ULONGLONG shownAt = 0;
    if (shown != title || g.frame - shownFrame > 2) {
        shown = title;
        shownAt = GetTickCount64();
    }
    shownFrame = g.frame;
    const float e = Ease(Progress(shownAt, 240.0f));
    const float y = (ui::kScreenH - h) / 2 + (1.0f - e) * 36.0f;
    ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, static_cast<uint32_t>(0x80 * e) << 24);
    ui::Fill(x, y, w, h, kBlack);
    ui::Gradient(x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f, 0xF02A3A5A, 0xF0101828);
    ui::Label(ui::kScreenW / 2, y + 16.0f, title, F(19.0f, kWhite, sprite::Align::Centre));
    float ly = y + 46.0f;
    for (const auto& line : lines) {
        ui::Label(ui::kScreenW / 2, ly, line, body);
        ly += 19.0f;
    }
    buttonsY = y + h - 56.0f;
}

void SavePromptScreen() {
    Backdrop();
    StatusBar();
    Header("Save Game");
    game::VehicleState vehicle{};
    std::string problem;
    if (game::PlayerVehicleState(vehicle)) problem = "Get out of the vehicle to save.";
    else if (OnMission()) problem = "You can't save during a mission.";
    float by = 0.0f;
    if (!problem.empty()) {
        Alert("Can't Save", problem, by);
        if (Button(40.0f, by, ui::kScreenW - 80.0f, 42.0f, 0xFF3A3A3E, "OK") || g.enter || TakeBack()) {
            Go(g.phoneTab);
        }
        return;
    }
    Alert("Save Game", "Save the game here, where you are standing? Loading it puts you back on this spot.", by);
    if (Button(40.0f, by, 112.0f, 42.0f, 0xFF3A3A3E, "Cancel") || TakeBack()) {
        Go(g.phoneTab);
        return;
    }
    if (Button(168.0f, by, 112.0f, 42.0f, kMoneyGreen, "Save") || g.enter) {
        g.pendingSave = true;
        g.dial.clear();
        Go(g.phoneTab);
        PutAway();
        logfile::Line("phone: saving where CJ stands");
    }
}

// --- Settings: tones ------------------------------------------------------

void TonesScreen() {
    const bool text = g.toneKind == 1;
    const auto& tones = text ? config::Get().textTones : config::Get().ringtones;
    const char* key = g.toneKind == 2 ? "alarmtone" : text ? "texttone" : "ringtone";
    const std::string picked = PickedTone(tones, key).name;
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom) + 14.0f;
    const float y0 = y;
    for (size_t i = 0; i < tones.size(); ++i) {
        const bool first = i == 0, last = i + 1 == tones.size();
        const bool on = tones[i].name == picked;
        const Rect r{10.0f, y, ui::kScreenW - 20.0f, 44.0f};
        ui::Fill(r.x, r.y, r.w, r.h, ui::Pressing(r) ? 0xFF3B5B8A : 0xE0202024);
        if (first) ui::Fill(r.x, r.y, r.w, 1.5f, kBlack);
        if (last) ui::Fill(r.x, r.y + r.h - 1.5f, r.w, 1.5f, kBlack);
        else ui::Fill(r.x + 12.0f, r.y + r.h - 1.0f, r.w - 12.0f, 1.0f, kSeparator);
        ui::Fill(r.x, r.y, 1.5f, r.h, kBlack);
        ui::Fill(r.x + r.w - 1.5f, r.y, 1.5f, r.h, kBlack);
        ui::Label(r.x + 14.0f, r.y + 12.0f, tones[i].name, F(17.0f, on ? kMenuBlue : kWhite));
        // The tick on the one in use.
        if (on) ui::Label(r.x + r.w - 26.0f, r.y + 10.0f, "v", F(20.0f, kMenuBlue, sprite::Align::Centre));
        if (ui::Tapped(r)) {
            phone_data::Get().store[key] = tones[i].name;
            Save();
            sound::Play(tones[i].sound);
        }
        y += 44.0f;
    }
    EndList(top, bottom, y - y0 + 28.0f);
    ui::Flush();
    StatusBar();
    const char* title = g.toneKind == 2 ? "Alarm Tone" : text ? "Text Tone" : "Ringtone";
    const char* back = g.toneBack == Screen::Alarm ? "Alarm" : g.toneBack == Screen::Timer ? "Timer" : "Settings";
    if (Header(title, back) == -1) {
        sound::Stop();
        Go(g.toneBack);
    }
}

// About: the phone's own page, laid out as the original iPhone's Settings >
// General > About is - the system's name and version over a grouped list of
// what is on the phone - with the same words and links the trainer's About
// page carries.
struct Link {
    const char* icon;
    const char* name;
    const char* url;
    const char* detail = "";
};
const Link kSupport{"brand_valkyrie", "support on ko-fi", "https://ko-fi.com/valkyriesamp"};
const Link kElsewhere[] = {
    {"brand_valkyrie", "valkyrie", "https://ko-fi.com/valkyriesamp"},
    {"brand_sprp", "SP-RP", "https://sp-rp.com/"},
    {"brand_ssmp", "Stars & Stripes Multiplayer", ""},
};

// A row of a grouped list with no picture, or with a logo: the name, and on
// the right what it is.
bool GroupRow(float y, const char* icon, const std::string& title, const std::string& detail, bool first,
              bool last, bool tappable) {
    const Rect r{10.0f, y, ui::kScreenW - 20.0f, 44.0f};
    ui::Fill(r.x, r.y, r.w, r.h, tappable && ui::Pressing(r) ? kMenuBlueBar : 0xC8101012);
    if (first) ui::Fill(r.x, r.y, r.w, 2.0f, kBlack);
    if (last) ui::Fill(r.x, r.y + r.h - 2.0f, r.w, 2.0f, kBlack);
    else ui::Fill(r.x + 12.0f, r.y + r.h - 1.0f, r.w - 12.0f, 1.0f, kSeparator);
    ui::Fill(r.x, r.y, 2.0f, r.h, kBlack);
    ui::Fill(r.x + r.w - 2.0f, r.y, 2.0f, r.h, kBlack);
    float left = r.x + 12.0f;
    if (icon) {
        ui::Image(icon, r.x + 8.0f, y + 6.0f, 32.0f, 32.0f);
        left = r.x + 48.0f;
    }
    const Font df = F(14.0f, kMenuBlue, sprite::Align::Right);
    const float dw = detail.empty() ? 0.0f : ui::TextWidth(detail, df) + 10.0f;
    ui::Label(left, y + 13.0f, ui::Fit(title, r.x + r.w - left - 12.0f - dw, F(16.0f)), F(16.0f));
    if (!detail.empty()) ui::Label(r.x + r.w - 12.0f, y + 15.0f, detail, df);
    return tappable && ui::Tapped(r);
}

// A paragraph, wrapped to the list's width. Returns its height.
float Paragraph(float y, const char* text, uint32_t argb = kGrey) {
    const Font f = F(14.0f, argb, sprite::Align::Left, 0);
    const auto lines = sprite::Wrap(text, 284.0f * ui::PixelsPerPoint(),
                                    sprite::TextStyle{sprite::Face::Subtitles, 14.0f * ui::PixelsPerPoint()});
    float ly = y;
    for (const auto& l : lines) {
        ui::Label(18.0f, ly, l, f);
        ly += 18.0f;
    }
    return ly - y;
}

void OpenLink(const char* url) {
    if (!url || !*url) return;
    logfile::Line("phone: opening %s", url);
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Trainer -------------------------------------------------------------------
//
// The Valkyrie trainer, inside the phone: the actions its menu registers
// (valkyrie-trainer's ActionBindings), by group, and the vehicles, weapons
// and characters it picks from. It is valkyrie-trainer.asi's when that is
// installed too, else the copy the phone carries. Alt+Z still opens the
// trainer's own full menu.
}  // namespace
}  // namespace phone
extern "C" int __cdecl ValkyrieTrainerActionCount();
extern "C" const char* __cdecl ValkyrieTrainerActionLabel(int);
extern "C" const char* __cdecl ValkyrieTrainerActionGroup(int);
extern "C" int __cdecl ValkyrieTrainerActionState(int);
extern "C" void __cdecl ValkyrieTrainerActionRun(int);
extern "C" int __cdecl ValkyrieTrainerListCount(int);
extern "C" const char* __cdecl ValkyrieTrainerListName(int, int);
extern "C" void __cdecl ValkyrieTrainerListPick(int, int);
namespace phone {
namespace {

struct TrainerApi {
    int(__cdecl* count)() = &ValkyrieTrainerActionCount;
    const char*(__cdecl* label)(int) = &ValkyrieTrainerActionLabel;
    const char*(__cdecl* group)(int) = &ValkyrieTrainerActionGroup;
    int(__cdecl* state)(int) = &ValkyrieTrainerActionState;
    void(__cdecl* run)(int) = &ValkyrieTrainerActionRun;
    int(__cdecl* listCount)(int) = &ValkyrieTrainerListCount;
    const char*(__cdecl* listName)(int, int) = &ValkyrieTrainerListName;
    void(__cdecl* listPick)(int, int) = &ValkyrieTrainerListPick;
};

const TrainerApi& Trainer() {
    static TrainerApi api;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE m = GetModuleHandleA("valkyrie-trainer.asi")) {
            // Its own, when it is installed: the phone's copy has stood down.
            auto get = [&](auto& fn, const char* name) {
                FARPROC p = GetProcAddress(m, name);
                fn = p ? reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p) : nullptr;
            };
            get(api.count, "ValkyrieTrainerActionCount");
            get(api.label, "ValkyrieTrainerActionLabel");
            get(api.group, "ValkyrieTrainerActionGroup");
            get(api.state, "ValkyrieTrainerActionState");
            get(api.run, "ValkyrieTrainerActionRun");
            get(api.listCount, "ValkyrieTrainerListCount");
            get(api.listName, "ValkyrieTrainerListName");
            get(api.listPick, "ValkyrieTrainerListPick");
        }
    }
    return api;
}

// The actions whose desktop menu fields the phone has no place for; the
// three lists stand in for what they are for.
bool TrainerHidden(const std::string& label) {
    static const char* const kHidden[] = {"Spawn Vehicle",  "Set Skin",      "Give Weapon",     "Teleport",
                                          "Play Animation", "Play Sequence", "Add To Sequence", "Clear Sequence"};
    for (const char* h : kHidden) {
        if (label == h) return true;
    }
    return false;
}

const char* const kTrainerLists[3] = {"Spawn a Vehicle", "Get a Weapon", "Change Character"};

void TrainerScreen() {
    const TrainerApi& t = Trainer();
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    const bool whole = t.count && t.label && t.group && t.state && t.run && t.listCount && t.listName && t.listPick;
    const int count = whole ? t.count() : 0;
    float y = BeginList(top, bottom) + 16.0f;
    const float y0 = y;
    std::string title = "Trainer";
    const char* back = nullptr;

    if (count == 0) {
        ui::Label(ui::kScreenW / 2, y + 40.0f, "The trainer is not running", F(18.0f, kWhite, sprite::Align::Centre));
        y += 70.0f;
        y += Paragraph(y, "It starts once you are in the game. If it still does not, valkyrie-phone.log says why.");
    } else if (g.trainerList >= 0) {
        // A vehicle, a weapon or a character, picked from its list.
        title = kTrainerLists[g.trainerList];
        back = "Trainer";
        const int n = t.listCount(g.trainerList);
        for (int i = 0; i < n; ++i) {
            if (y > -44.0f && y < ui::kScreenH) {
                const std::string name = t.listName(g.trainerList, i);
                if (GroupRow(y, nullptr, name, "", i == 0, i + 1 == n, true)) {
                    t.listPick(g.trainerList, i);
                    logfile::Line("phone: trainer - %s: %s", kTrainerLists[g.trainerList], name.c_str());
                    Notice(name + (g.trainerList == 0 ? " is here." : "."));
                }
            }
            y += 44.0f;
        }
    } else if (!g.trainerGroup.empty()) {
        // One group's actions; a toggle shows whether it is on.
        title = g.trainerGroup;
        back = "Trainer";
        std::vector<int> rows;
        for (int i = 0; i < count; ++i) {
            if (g.trainerGroup == t.group(i) && !TrainerHidden(t.label(i))) rows.push_back(i);
        }
        for (size_t r = 0; r < rows.size(); ++r) {
            const int i = rows[r];
            const int state = t.state(i);
            const std::string detail = state < 0 ? "" : state ? "On" : "Off";
            if (y > -44.0f && y < ui::kScreenH) {
                const std::string label = t.label(i);
                if (GroupRow(y, nullptr, label, detail, r == 0, r + 1 == rows.size(), true)) {
                    t.run(i);
                    logfile::Line("phone: trainer - %s", label.c_str());
                    if (state < 0) Notice(label + ".");
                }
            }
            y += 44.0f;
        }
    } else {
        // The first page: the trainer's mark, the three lists, then its groups
        // in the order its menu has them.
        ui::Image("brand_valkyrie", ui::kScreenW / 2 - 28.0f, y, 56.0f, 56.0f);
        y += 62.0f;
        ui::Label(ui::kScreenW / 2, y, "Valkyrie Trainer", F(22.0f, kWhite, sprite::Align::Centre));
        y += 36.0f;
        for (int i = 0; i < 3; ++i) {
            if (GroupRow(y, nullptr, kTrainerLists[i], "", i == 0, i == 2, true)) {
                g.trainerList = i;
                g.scroll[Screen::Trainer] = 0.0f;
            }
            y += 44.0f;
        }
        y += 22.0f;
        std::vector<std::string> groups;
        for (int i = 0; i < count; ++i) {
            const std::string grp = t.group(i);
            if (!grp.empty() && std::find(groups.begin(), groups.end(), grp) == groups.end()) groups.push_back(grp);
        }
        for (size_t i = 0; i < groups.size(); ++i) {
            if (GroupRow(y, nullptr, groups[i], "", i == 0, i + 1 == groups.size(), true)) {
                g.trainerGroup = groups[i];
                g.scroll[Screen::Trainer] = 0.0f;
            }
            y += 44.0f;
        }
    }
    y += 20.0f;
    EndList(top, bottom, y - y0 + 16.0f);
    if (!g.notice.empty() && GetTickCount64() - g.noticeAt < 2500) {
        ui::Fill(14.0f, ui::kScreenH - 60.0f, ui::kScreenW - 28.0f, 40.0f, kPanelDark);
        ui::Label(ui::kScreenW / 2, ui::kScreenH - 50.0f, ui::Fit(g.notice, 270.0f, F(15.0f)),
                  F(15.0f, kWhite, sprite::Align::Centre));
    }
    ui::Flush();
    StatusBar();
    if (Header(title, back) == -1) {
        g.trainerGroup.clear();
        g.trainerList = -1;
        g.scroll[Screen::Trainer] = 0.0f;
    }
}

void AboutScreen() {
    const auto& d = phone_data::Get();
    Backdrop();
    const float top = kContentTop, bottom = ui::kScreenH;
    float y = BeginList(top, bottom) + 16.0f;
    const float y0 = y;

    // The system, as the phone names it - the one place its mark is.
    ui::Image("brand_valkyrie", ui::kScreenW / 2 - 32.0f, y, 64.0f, 64.0f);
    y += 70.0f;
    ui::Label(ui::kScreenW / 2, y, "Valkyrie OS", F(24.0f, kWhite, sprite::Align::Centre));
    y += 28.0f;
    ui::Label(ui::kScreenW / 2, y, std::string("version ") + VALKYRIE_PHONE_VERSION,
              F(13.0f, kGrey, sprite::Align::Centre, 0));
    y += 30.0f;

    // What it is.
    const std::pair<const char*, std::string> info[] = {
        {"Name", "Carl's iFruit"},
        {"My Number", phone_data::FormatNumber(d.settings.ownNumber)},
        {"Model", KeypadSkin() ? "Keypad" : "iFruit"},
        {"Version", std::string("Valkyrie OS ") + VALKYRIE_PHONE_VERSION},
    };
    const int count = static_cast<int>(sizeof info / sizeof info[0]);
    for (int i = 0; i < count; ++i) {
        GroupRow(y, nullptr, info[i].first, info[i].second, i == 0, i + 1 == count, false);
        y += 44.0f;
    }
    y += 22.0f;

    // Who made it, and where to support it.
    y += Paragraph(y, "made by valkyrie. if the phone is useful to you, you can support the work on ko-fi.") + 10.0f;
    if (GroupRow(y, nullptr, "Support on Ko-fi", "ko-fi.com", true, true, true)) OpenLink(kSupport.url);
    y += 44.0f + 22.0f;

    // With thanks, and the projects it is part of.
    y += Paragraph(y, "Grand Theft Auto: San Andreas and its artwork are Rockstar Games'. Built with plugin-sdk and "
                      "the gta-reversed project. Thanks to everyone testing it and reporting bugs.") + 16.0f;
    const int links = static_cast<int>(sizeof kElsewhere / sizeof kElsewhere[0]);
    int shown = 0;
    for (int i = 0; i < links; ++i) shown += std::strcmp(kElsewhere[i].icon, "brand_valkyrie") != 0;
    int row = 0;
    for (int i = 0; i < links; ++i) {
        const Link& l = kElsewhere[i];
        if (std::strcmp(l.icon, "brand_valkyrie") == 0) continue;  // the Ko-fi row above is its link
        if (GroupRow(y, l.icon, l.name, l.detail, row == 0, row + 1 == shown, *l.url != 0)) OpenLink(l.url);
        ++row;
        y += 44.0f;
    }
    y += 20.0f;

    EndList(top, bottom, y - y0 + 16.0f);
    ui::Flush();
    StatusBar();
    if (Header("About", "Settings") == -1) Go(Screen::Settings);
}

// ---------------------------------------------------------------------------
// The handset
// ---------------------------------------------------------------------------

// Starting up. The backlight comes on over the black of a lit display, a
// touch lighter at its edges, with one flicker; the iFruit mark fades in and
// is held a while, a light passes over it once, and a thin bar under it
// fills in uneven steps with the stalls a real start-up has. Then it all
// goes, and the lock screen comes up out of black.
void BootScreen() {
    const float W = ui::kScreenW, H = ui::kScreenH;
    // Its steps are laid out over fourteen seconds, and stretched or squeezed
    // to the length [Features] gives it.
    const float t = static_cast<float>(GetTickCount64() - g.bootAt) * 14000.0f / std::max(1.0f, BootMs());
    auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    const float out = t > 13000.0f ? 1.0f - Ease(clamp01((t - 13000.0f) / 600.0f)) : 1.0f;
    auto faded = [&](float a, uint32_t rgb) { return (static_cast<uint32_t>(clamp01(a) * 255.0f) << 24) | rgb; };
    ui::Fill(0, 0, W, H, kBlack);

    // The backlight.
    float back = Ease(clamp01(t / 350.0f));
    if (t > 110.0f && t < 170.0f) back *= 0.35f;
    back *= out;
    ui::Fill(0, 0, W, H, faded(back, 0x0B0C0F));
    ui::Gradient(0, 0, W, 46.0f, faded(back * 0.55f, 0x1A1F28), faded(0.0f, 0x1A1F28));
    ui::Gradient(0, H - 46.0f, W, 46.0f, faded(0.0f, 0x1A1F28), faded(back * 0.55f, 0x1A1F28));

    // The mark, and its soft light.
    const float mark = Ease(clamp01((t - 600.0f) / 800.0f)) * out;
    const float size = 96.0f, mx = W / 2 - size / 2, my = H * 0.40f - size / 2;
    // The keypad phone's mark is the pad's ring of teal light.
    const bool sm = KeypadSkin();
    const char* const markArt = sm ? "sm_boot" : "boot_mark";
    ui::Image("glow", mx - 34.0f, my - 34.0f, size + 68.0f, size + 68.0f,
              faded(mark * (sm ? 0.4f : 0.16f), sm ? 0x56D6EC : 0xFFFFFF));
    ui::Image(markArt, mx, my, size, size, faded(mark * 0.94f, sm ? 0x7FE2F2 : 0xF2F4F8));
    // A light passing over it, once, left to right.
    const float sweep = (t - 1900.0f) / 900.0f;
    if (sweep > 0.0f && sweep < 1.0f) {
        const float band = 26.0f, bx = mx - band + (size + band) * Ease(sweep);
        ui::Flush();
        sprite::BeginScissor(ui::ToPixelX(std::max(bx, mx)), ui::ToPixelY(my), ui::ToPixelX(std::min(bx + band, mx + size)),
                             ui::ToPixelY(my + size));
        ui::Image(markArt, mx, my, size, size, faded(mark * 0.8f * std::sin(sweep * 3.14159265f), 0xFFFFFF));
        ui::Flush();
        sprite::EndScissor();
    }

    // The bar, filling in uneven steps.
    static const float kSteps[][2] = {{3000, 0.00f}, {3500, 0.06f}, {4300, 0.11f}, {5100, 0.29f},
                                      {6700, 0.33f}, {7300, 0.50f}, {8100, 0.54f}, {9000, 0.69f},
                                      {10500, 0.73f}, {11200, 0.88f}, {12300, 0.96f}, {12900, 1.00f}};
    float done = t >= kSteps[11][0] ? 1.0f : 0.0f;
    for (int i = 0; i + 1 < 12; ++i) {
        if (t >= kSteps[i][0] && t < kSteps[i + 1][0]) {
            const float k = (t - kSteps[i][0]) / (kSteps[i + 1][0] - kSteps[i][0]);
            done = kSteps[i][1] + (kSteps[i + 1][1] - kSteps[i][1]) * (k * k * (3.0f - 2.0f * k));
        }
    }
    const float bar = Ease(clamp01((t - 3000.0f) / 400.0f)) * out;
    if (bar > 0.0f) {
        const float bw = 118.0f, bh = 5.0f, bx = W / 2 - bw / 2, by = my + size + 40.0f;
        // The track, set into the glass: a lighter rim, a dark bed, a
        // shadow along its top.
        ui::Fill(bx - 1.0f, by - 1.0f, bw + 2.0f, bh + 2.0f, faded(bar, 0x2A2C31));
        ui::Fill(bx, by, bw, bh, faded(bar, 0x0E0F12));
        ui::Fill(bx, by, bw, 1.0f, faded(bar, 0x000000));
        // The fill: pale, catching light along its top, a glow at its end.
        const float fw = bw * done;
        if (fw > 0.5f) {
            ui::Gradient(bx, by, fw, bh, faded(bar, sm ? 0x9CEAF6 : 0xF4F6FA), faded(bar, sm ? 0x38A8C8 : 0xB4B8C2));
            ui::Fill(bx, by, fw, 1.0f, faded(bar * 0.6f, 0xFFFFFF));
            ui::Image("glow", bx + fw - 7.0f, by - 6.0f, 14.0f, bh + 12.0f, faded(bar * 0.35f, 0xFFFFFF));
        }
    }
}

void DrawScreen() {
    if (Booting()) {
        BootScreen();
        return;
    }
    if (!phone_data::Get().settings.poweredOn) {
        ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, kBlack);
        // Off like a picture tube: the screen folds to a bright line, then
        // to a point, and goes out.
        const float t = Progress(g.powerAt, 420.0f);
        if (g.powerAt && t < 1.0f) {
            const float fold = Ease(std::min(1.0f, t / 0.55f));
            const float shrink = t > 0.55f ? Ease((t - 0.55f) / 0.45f) : 0.0f;
            const float h = std::max(2.0f, (1.0f - fold) * ui::kScreenH);
            const float w = std::max(4.0f, (1.0f - shrink) * ui::kScreenW);
            const uint32_t a = static_cast<uint32_t>(255.0f * (1.0f - shrink * 0.8f));
            ui::Fill((ui::kScreenW - w) / 2, (ui::kScreenH - h) / 2, w, h, (a << 24) | 0xD8E8FF);
        }
        return;
    }
    if (g.call.active) {
        g.game.reset();
        CallScreen();
        return;
    }
    if (g.alarmRinging) {
        AlarmScreen();
        return;
    }
    if (g.game) {
        PlayScreen();
        return;
    }
    switch (g.screen) {
        case Screen::Off:
        case Screen::Lock: LockScreen(); break;
        case Screen::Home: HomeScreen(); break;
        case Screen::Keypad: KeypadScreen(); break;
        case Screen::Recents: RecentsScreen(); break;
        case Screen::Contacts: ContactsScreen(); break;
        case Screen::ContactView: ContactViewScreen(); break;
        case Screen::ContactEdit: ContactEditScreen(); break;
        case Screen::ContactIcon: ContactIconScreen(); break;
        case Screen::Messages: MessagesScreen(); break;
        case Screen::Thread: ThreadScreen(); break;
        case Screen::Compose: ComposeScreen(); break;
        case Screen::Photos: PhotosScreen(); break;
        case Screen::PhotoView: PhotoViewScreen(); break;
        case Screen::Clock: ClockScreen(); break;
        case Screen::Alarm: AlarmScreenSet(); break;
        case Screen::Stopwatch: StopwatchScreen(); break;
        case Screen::Timer: TimerScreen(); break;
        case Screen::Games: GamesScreen(); break;
        case Screen::Internet: InternetScreen(); break;
        case Screen::Sites: SitesScreen(); break;
        case Screen::Calculator: CalculatorScreen(); break;
        case Screen::Notes: NotesScreen(); break;
        case Screen::NoteEdit: NoteEditScreen(); break;
        case Screen::Weather: WeatherScreen(); break;
        case Screen::Stocks: StocksScreen(); break;
        case Screen::Radio: RadioScreen(); break;
        case Screen::Flashlight: FlashlightScreen(); break;
        case Screen::Calendar: CalendarScreen(); break;
        case Screen::CalendarEvent: CalendarEventScreen(); break;
        case Screen::CameraRoll: CameraRollScreen(); break;
        case Screen::PhotoOpen: PhotoOpenScreen(); break;
        case Screen::Camera: CameraScreen(); break;
        case Screen::Maps: MapsScreen(); break;
        case Screen::Settings: SettingsScreen(); break;
        case Screen::About: AboutScreen(); break;
        case Screen::Trainer: TrainerScreen(); break;
        case Screen::Tones: TonesScreen(); break;
        case Screen::SavePrompt: SavePromptScreen(); break;
    }
}

// Where the lit screen is on the display this frame, in pixels, and pixels
// to a point.
float g_screenLeft = 0.0f, g_screenTop = 0.0f, g_ppp = 1.0f;

// Draw `s` moved by (dx, dy) points and scaled by k about the point (cx, cy),
// cut to the lit screen. `from`: the screen being left, drawn as a picture.
void DrawScreenAs(Screen s, bool from, float dx, float dy, float k = 1.0f, float cx = ui::kScreenW / 2,
                  float cy = ui::kScreenH / 2, float dim = 0.0f) {
    const Screen keep = g.screen;
    const Field keepField = g.field;
    g.screen = s;
    g_drawingFrom = from;
    const float left = g_screenLeft + (cx + dx - cx * k) * g_ppp;
    const float top = g_screenTop + (cy + dy - cy * k) * g_ppp;
    ui::SetScreen(left, top, g_ppp * k);
    // Cut to the smaller of the lit screen and this screen's own rectangle.
    sprite::BeginScissor(left, top, left + ui::kScreenW * g_ppp * k, top + ui::kScreenH * g_ppp * k);
    DrawScreen();
    ui::NoClip();
    if (dim > 0.0f) ui::Fill(0, 0, ui::kScreenW, ui::kScreenH, static_cast<uint32_t>(255.0f * dim) << 24);
    ui::Flush();
    sprite::EndScissor();
    g_drawingFrom = false;
    // A screen drawn as `from` may not change which one is shown, nor take
    // the typing.
    g.screen = keep;
    if (from) g.field = keepField;
    ui::SetScreen(g_screenLeft, g_screenTop, g_ppp);
}

// "slide to power off", over whatever is on the screen, as the sleep button
// held brings it up: a red knob to slide across, and Cancel under it. Its
// touches are taken before the screen under it is drawn, so nothing under
// it is touched as well.
void PowerMenuInput() {
    const Rect track{18.0f, 40.0f, 284.0f, 52.0f};
    const float knobW = 70.0f, travel = track.w - knobW - 8.0f;
    const Rect knob{track.x + 4.0f + g.powerKnob * travel, track.y + 4.0f, knobW, track.h - 8.0f};
    if (!g.powerSliding && ui::Pressing(knob)) g.powerSliding = true;
    if (g.powerSliding) {
        if (ui::Dragging() || ui::Pressing(knob)) {
            g.powerKnob = std::clamp((ui::CursorX() - track.x - 4.0f - knobW / 2) / travel, 0.0f, 1.0f);
        }
        if (!input::LeftHeld()) {
            g.powerSliding = false;
            if (g.powerKnob > 0.92f) {
                g.powerMenu = false;
                TogglePower();
            }
        }
    } else {
        g.powerKnob = g.powerKnob < 0.002f ? 0.0f : g.powerKnob * std::exp(-g.dt * 14.0f);
    }
    if (ui::Tapped(Rect{40.0f, ui::kScreenH - 110.0f, 240.0f, 52.0f})) g.powerMenu = false;
    // Nothing under the menu is touched - but the press is kept, or the
    // knob could never be dragged.
    ui::BlockTouch();
}

void PowerMenuDraw() {
    const float W = ui::kScreenW, H = ui::kScreenH;
    ui::Fill(0, 0, W, H, 0xB0000000);
    const Rect track{18.0f, 40.0f, 284.0f, 52.0f};
    const float knobW = 70.0f, travel = track.w - knobW - 8.0f;
    ui::Fill(track.x, track.y, track.w, track.h, kPanelDark);
    ui::Frame(track.x, track.y, track.w, track.h, 2.0f, kBlack);
    Shimmer(track.x + track.w / 2 + 30.0f, track.y + 15.0f, "slide to power off", 18.0f, 0x00FFFFFF,
            std::clamp(1.0f - g.powerKnob * 2.5f, 0.0f, 1.0f));
    const Rect knob{track.x + 4.0f + g.powerKnob * travel, track.y + 4.0f, knobW, track.h - 8.0f};
    ui::Fill(knob.x, knob.y, knob.w, knob.h, kWastedRed);
    ui::Frame(knob.x, knob.y, knob.w, knob.h, 2.0f, kBlack);
    ui::Image("g_power", knob.x + knob.w / 2 - 14.0f, knob.y + 8.0f, 28.0f, 28.0f);
    const Rect cancel{40.0f, H - 110.0f, 240.0f, 52.0f};
    ui::Fill(cancel.x, cancel.y, cancel.w, cancel.h, 0xFF3A3A3E);
    ui::Frame(cancel.x, cancel.y, cancel.w, cancel.h, 2.0f, kBlack);
    ui::Label(W / 2, cancel.y + 15.0f, "Cancel", F(19.0f, kWhite, sprite::Align::Centre));
}

// The side buttons' picture in the middle of the screen: the volume as a
// row of bars, or the bell struck through for silent.
void OverlayDraw() {
    const ULONGLONG age = GetTickCount64() - g.overlayAt;
    if (!g.overlayAt || age > 1600) return;
    const float fade = age > 1300 ? 1.0f - (age - 1300) / 300.0f : Ease(age / 120.0f);
    const uint32_t a = static_cast<uint32_t>(0xC8 * fade) << 24;
    const float bx = 85.0f, by = 150.0f, bs = 150.0f;
    ui::Fill(bx, by, bs, bs, a);
    const uint32_t white = (static_cast<uint32_t>(255 * fade) << 24) | 0xFFFFFF;
    const auto& st = phone_data::Get().settings;
    if (g.overlayKind == 0) {
        ui::Image("g_speaker", bx + bs / 2 - 32.0f, by + 20.0f, 64.0f, 64.0f, white);
        for (int i = 0; i < 10; ++i) {
            ui::Fill(bx + 16.0f + i * 12.0f, by + 110.0f, 9.0f, 12.0f, i < st.volume ? white : (a | 0x404040));
        }
    } else {
        ui::Image("g_bell", bx + bs / 2 - 32.0f, by + 24.0f, 64.0f, 64.0f, white);
        if (g.overlayKind == 2) {
            ui::Needle(bx + bs / 2, by + 56.0f, 0.785f, 46.0f, 46.0f, 6.0f, (white & 0xFF000000) | 0xE03030);
        }
        ui::Label(bx + bs / 2, by + 106.0f, g.overlayKind == 2 ? "Silent" : "Ring",
                  F(18.0f, white, sprite::Align::Centre));
    }
}

// The screen, or the two screens of a move between them.
void DrawAnimated() {
    const float W = ui::kScreenW, H = ui::kScreenH;
    sprite::BeginScissor(g_screenLeft, g_screenTop, g_screenLeft + W * g_ppp, g_screenTop + H * g_ppp);
    // Asleep: nothing on the screen is touched. Off or starting up, the
    // screen has nothing to touch - and the press is left alone, because the
    // sleep button, read after the screen is drawn, needs it to switch the
    // phone on.
    if (g.asleep) ui::CancelTouch();
    if (g.powerMenu) PowerMenuInput();
    const bool plain = g.call.active || g.alarmRinging || g.game || Dead();
    if (!plain && Moving()) {
        const float e = Ease(Progress(g_move.at, kMoveMs));
        const Screen to = g.screen;
        switch (g_move.motion) {
            case Motion::Push:
                DrawScreenAs(g_move.from, true, -e * W * 0.5f, 0.0f, 1.0f, W / 2, H / 2, 0.45f * e);
                DrawScreenAs(to, false, (1.0f - e) * W, 0.0f);
                break;
            case Motion::Pop:
                DrawScreenAs(to, false, -(1.0f - e) * W * 0.5f, 0.0f, 1.0f, W / 2, H / 2, 0.45f * (1.0f - e));
                DrawScreenAs(g_move.from, true, e * W, 0.0f);
                break;
            case Motion::ZoomIn: {
                DrawScreenAs(g_move.from, true, 0.0f, 0.0f, 1.0f + 0.12f * e, W / 2, H / 2, 0.6f * e);
                const float k = 0.06f + 0.94f * e;
                // The app's middle travels from the icon to the screen's middle.
                const float mx = g_move.ox + (W / 2 - g_move.ox) * e, my = g_move.oy + (H / 2 - g_move.oy) * e;
                DrawScreenAs(to, false, mx - W / 2, my - H / 2, k);
                break;
            }
            case Motion::ZoomOut: {
                DrawScreenAs(to, false, 0.0f, 0.0f, 1.12f - 0.12f * e, W / 2, H / 2, 0.6f * (1.0f - e));
                DrawScreenAs(g_move.from, true, 0.0f, 0.0f, 1.0f - 0.94f * e);
                break;
            }
            case Motion::Unlock:
                DrawScreenAs(to, false, 0.0f, 0.0f);
                DrawScreenAs(g_move.from, true, 0.0f, -e * H);
                break;
            case Motion::None:
                break;
        }
    } else if (g.call.active && Progress(g.call.started, 300.0f) < 1.0f && g.screen != Screen::Lock) {
        // A call rises over the screen it was made from.
        const float e = Ease(Progress(g.call.started, 300.0f));
        // Nothing is touched while it rises: the screen under it must not
        // take a tap meant for the call.
        ui::CancelTouch();
        g.call.active = false;
        DrawScreenAs(g.screen, true, 0.0f, 0.0f, 1.0f, W / 2, H / 2, 0.5f * e);
        g.call.active = true;
        DrawScreenAs(g.screen, false, 0.0f, (1.0f - e) * H);
    } else {
        DrawScreen();
    }
    if (g.powerMenu) {
        ui::NoClip();
        PowerMenuDraw();
    }
    ui::NoClip();
    OverlayDraw();
    if (!Dead() && !g.asleep) NoticeDraw();
    ui::NoClip();
    // Auto-Brightness: the display turns itself down a little in the dark,
    // as the phone's light sensor has it do.
    if (Flag("autobright", true)) {
        const float level = std::clamp((std::max({g.lightR, g.lightG, g.lightB}) - 0.2f) / 0.6f, 0.0f, 1.0f);
        const float down = 0.22f * (1.0f - level);
        if (down > 0.01f) ui::Fill(0, 0, W, H, static_cast<uint32_t>(255.0f * down) << 24);
    }
    // Left alone, the screen dims before it locks.
    if (g.dim > 0.0f) ui::Fill(0, 0, W, H, static_cast<uint32_t>(150.0f * g.dim) << 24);
    // Put to sleep: the picture dims to black; woken, it comes back.
    if (g.asleep) {
        ui::Fill(0, 0, W, H, static_cast<uint32_t>(255.0f * Ease(Progress(g.asleepAt, 220.0f))) << 24);
    } else if (g.wokeAt && Progress(g.wokeAt, 300.0f) < 1.0f) {
        ui::Fill(0, 0, W, H, static_cast<uint32_t>(255.0f * (1.0f - Ease(Progress(g.wokeAt, 300.0f)))) << 24);
    }
    // Switched on and started: the picture comes up out of black.
    const ULONGLONG shown = g.bootAt ? g.bootAt + static_cast<ULONGLONG>(BootMs()) : g.powerAt;
    const float on = Progress(shown, 450.0f);
    if (shown && GetTickCount64() >= shown && on < 1.0f && phone_data::Get().settings.poweredOn) {
        ui::NoClip();
        ui::Fill(0, 0, W, H, static_cast<uint32_t>(255.0f * (1.0f - Ease(on))) << 24);
    }
    ui::NoClip();
    ui::Flush();
    sprite::EndScissor();
}

volatile LONG g_openAsked = 0;  // RequestOpen, from another mod
ULONGLONG g_openAskedAt = 0;

void Open() {
    if (!g.loaded || g.dictionary < 0 || !input::Install() || !PlayerCanUsePhone()) return;
    // Not while swimming: it would go straight back in the pocket.
    if (PlayerSwimming()) return;
    Wake();
    // Taken out, its screen lights up out of black.
    g.wokeAt = GetTickCount64();
    g.activeAt = g.wokeAt;
    g.out = true;
    g.focused = true;
    g.placeCursor = true;
    // The menu that asked for it may still be up a frame or two, and the
    // phone is only drawn once it has gone: that is not the phone being
    // hidden, which would lower it.
    g.drawGrace = g.frame + 180;
    g.drawnSinceUp = false;
    // Held a little off square, differently each time: up to about 2.5
    // degrees turned and tipped, 1.5 spun.
    {
        static std::mt19937 rng(GetTickCount());
        std::uniform_real_distribution<float> d(-1.0f, 1.0f);
        g.restTilt[0] = d(rng) * 0.044f;
        g.restTilt[1] = d(rng) * 0.044f;
        g.restTilt[2] = d(rng) * 0.026f;
    }
    input::Capture(true);
    if (phone_data::Get().settings.slideToUnlock && !g.call.active) {
        if (g.screen != Screen::Lock) g.resume = g.screen;
        g.screen = Screen::Lock;
        g.knob = 0.0f;
    }
}

void Lower() {
    // The camera needs the mouse, so lowering the phone stops it - but the
    // Camera app stays on the phone's screen, and raising it again carries
    // on where it was.
    if (g.camera.on) {
        g.cameraPaused = true;
        g.cameraPausedSelfie = g.camera.selfie;
        g.cameraPausedReturn = g.camera.returnTo;
        StopCamera(true);
    }
    g.focused = false;
    input::Capture(false);
    StopPose();
}

void Raise() {
    g.drawGrace = g.frame + 180;
    g.drawnSinceUp = false;
    g.activeAt = GetTickCount64();
    g.focused = true;
    g.placeCursor = true;
    input::Capture(true);
}

// The keypad handset's A: wakes the screen, unlocks, opens the
// app lit on the menu. Apps keep their own on-screen actions.
void KeyA() {
    if (g.asleep) {
        Wake();
        return;
    }
    if (!phone_data::Get().settings.poweredOn || g.call.active || g.alarmRinging || g.game || g.powerMenu) return;
    if (g.screen == Screen::Lock) {
        Unlock();
    } else if (g.screen == Screen::Home) {
        if (!g.menuSel.empty()) OpenApp(g.menuSel);
    }
}

// Its minus: back - out of a game, off a call, to the first page of the
// menu, or back a screen where the screen has a way back.
void KeyMinus() {
    if (g.asleep) {
        Wake();
        return;
    }
    if (g.powerMenu) {
        g.powerMenu = false;
    } else if (g.alarmRinging) {
        return;
    } else if (g.camera.on) {
        StopCamera(true);
    } else if (g.game) {
        g.game.reset();
    } else if (g.call.active) {
        EndCall();
    } else if (g.screen == Screen::Home) {
        g.menuPage = 0;
    } else {
        g.backPending = true;
    }
}

void PutAway() {
    // The light goes out with the phone.
    g.flashlight = false;
    g.cameraPaused = false;
    StopCamera(false);
    g.powerMenu = false;
    g.game.reset();
    g.out = false;
    g.focused = false;
    input::Capture(false);
}

void TogglePower() {
    auto& s = phone_data::Get().settings;
    s.poweredOn = !s.poweredOn;
    g.powerAt = GetTickCount64();
    if (!s.poweredOn) {
        EndCall();
    } else {
        g.screen = s.slideToUnlock ? Screen::Lock : Screen::Home;
        g.resume = Screen::Home;
        g.knob = 0.0f;
        g.bootAt = g.powerAt;
    }
    Save();
}

// The sleep button: the screen goes off, and back on at the lock.
// The sleep button pressed: the phone locks - straight to "slide to unlock",
// the screen staying lit.
void Sleep() {
    if (!phone_data::Get().settings.poweredOn) return;
    g.powerMenu = false;
    if (g.call.active || g.screen == Screen::Lock) return;
    StopCamera(false);
    g.game.reset();
    g.resume = g.screen;
    g.screen = Screen::Lock;
    g.knob = 0.0f;
    ui::CancelTouch();
    if (LockSounds()) sound::Click(sound::Tone::Lock);
}

void Wake() {
    if (!g.asleep) return;
    g.asleep = false;
    g.wokeAt = GetTickCount64();
}

void ShowOverlay(int kind) {
    g.overlayKind = kind;
    g.overlayAt = GetTickCount64();
}

void Vibrate(ULONGLONG ms) {
    if (config::Get().features.vibrate) g.shakeUntil = GetTickCount64() + ms;
}

void ChangeVolume(int by) {
    auto& s = phone_data::Get().settings;
    s.volume = std::clamp(s.volume + by, 0, 10);
    sound::SetVolume(s.volume / 10.0f);
    ShowOverlay(0);
    if (!s.silent) sound::Click();
    Save();
}

void ToggleSilent() {
    auto& s = phone_data::Get().settings;
    s.silent = !s.silent;
    ShowOverlay(s.silent ? 2 : 1);
    if (s.silent) Vibrate(450);
    Save();
}

// A tone the ring switch holds back: key clicks, a text going, a text tone.
// Silenced, the phone buzzes instead.
void PlayAlert(const std::string& tone, ULONGLONG buzz) {
    if (phone_data::Get().settings.silent) {
        if (buzz) Vibrate(buzz);
        return;
    }
    sound::Play(tone);
}

// The phone in CJ's left hand while it is out. It is an item of its own, not
// a weapon: his fists are out while he holds it, the weapon he was holding is
// put away for it and given back when the phone goes away, and no weapon can
// be drawn meanwhile. The game only draws held models in the right hand, so
// the phone draws itself on the left hand's bone (phone_model::
// RenderInLeftHand, from Draw). While it is at his ear the mission phone
// task has his right hand and its own copy of the model.
// --- The phone as a weapon ---------------------------------------------------
//
// With the Valkyrie Phone modloader folder in, and its line in fastman92's
// weapon type config, the phone is a weapon of the game's own (VALKYRIEPHONE,
// model 23900, the detonator's slot): the game scrolls to it and away,
// draws it in CJ's left hand. Selected, the phone comes
// out; another weapon selected, it goes away; P selects it. Without them the
// phone holds itself, as HoldPhone does below.
constexpr int kPhoneWeaponModel = 23900;
constexpr int kPhoneWeaponSlot = 12;
constexpr uintptr_t kGetWeaponInfo = 0x743C60;  // CWeaponInfo::GetWeaponInfo(type, skill)
constexpr uintptr_t kGiveWeapon = 0x5E6080;     // CPed::GiveWeapon(type, ammo, bool)
constexpr size_t kPedWeapons = 0x5A0, kWeaponSize = 0x1C;  // CPed::m_aWeapons[13]

// The weapon type the game gave the phone, found by its model once the
// weapon data is loaded; -1 when there is none.
// A model the phone needs, loaded now only if the game does not have it
// already: loading everything requested at once makes the game drop the
// distant scenery (LODs) for a moment to make room.
void EnsureModel(int id) {
    if (phone_model::InMemory(id)) return;
    reinterpret_cast<void(__cdecl*)(int, int)>(kRequestModel)(id, 0x8);
    reinterpret_cast<void(__cdecl*)(bool)>(kLoadAllRequestedModels)(false);
    logfile::Line("phone: model %d was not in memory - loaded it", id);
}

int PhoneWeaponType() {
    static int type = -2;
    if (type != -2) return type;
    // Not until the game has read its weapon data.
    if (*reinterpret_cast<const bool*>(kGameNotLoaded)) return -1;
    type = -1;
    auto info = reinterpret_cast<const uint8_t*(__cdecl*)(int, int)>(kGetWeaponInfo);
    for (int t = 1; t < 512; ++t) {
        const uint8_t* w = info(t, 1);
        if (w && *reinterpret_cast<const int*>(w + 0xC) == kPhoneWeaponModel) {
            type = t;
            break;
        }
    }
    logfile::Line(type > 0 ? "phone: the phone is a weapon (type %d)" : "phone: no phone weapon - it holds itself",
                  type);
    return type;
}

int WeaponInSlot(uintptr_t ped, int slot) {
    return *reinterpret_cast<const int*>(ped + kPedWeapons + slot * kWeaponSize);
}

int CurrentSlot(uintptr_t ped) { return *reinterpret_cast<const uint8_t*>(ped + kPedWeaponSlot); }

// The phone as the game's weapon: in CJ's pockets whenever its slot is free,
// out when it is the weapon in his hand.
void WieldPhone(uintptr_t ped, int type) {
    // His phone is always on him: given back whenever its slot is empty (a
    // detonator, arrest or death having taken it).
    if (WeaponInSlot(ped, kPhoneWeaponSlot) == 0) {
        EnsureModel(kPhoneWeaponModel);
        reinterpret_cast<void(__thiscall*)(uintptr_t, int, unsigned, bool)>(kGiveWeapon)(ped, type, 1, false);
    }
    if (WeaponInSlot(ped, kPhoneWeaponSlot) != type) {
        g.holding = false;
        g.wieldedLastFrame = false;
        return;
    }
    game::VehicleState vehicle{};
    // In a vehicle, or getting in or out of one (the game takes the weapon
    // out of his hand for that), the phone stays out on the screen and the
    // weapon slot is left alone: losing the slot then is not another weapon
    // chosen. Back on foot with the phone still out, it is in his hand again.
    if (game::PlayerVehicleState(vehicle) || InTransition(ped)) {
        g.wieldedLastFrame = false;
        g.holding = false;
        return;
    }
    const bool onFoot = true;
    const bool wielded = CurrentSlot(ped) == kPhoneWeaponSlot;
    auto select = [&](int slot) { reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(kSetCurrentWeapon)(ped, slot); };
    const bool want = (g.out || g.camera.on) && onFoot && PlayerAble();
    if (wielded && !g.wieldedLastFrame && !g.out && PlayerCanUsePhone()) {
        // Scrolled onto: out it comes, lowered at his side - the mouse and its
        // wheel stay the game's, to scroll on past it. The right button raises it.
        g.holsteredSlot = 0;
        Open();
        if (g.out) Lower();
    } else if (!wielded && g.wieldedLastFrame && g.out) {
        // Another weapon chosen: away it goes, and that weapon stays.
        g.holsteredSlot = -1;
        PutAway();
    } else if (want && !wielded && !g.wieldedLastFrame) {
        // Taken out some other way (P, another mod): it becomes the weapon in
        // his hand, and the one there is given back when it goes away.
        g.holsteredSlot = CurrentSlot(ped);
        select(kPhoneWeaponSlot);
    } else if (!want && wielded && g.wieldedLastFrame) {
        // Put away some other way: the weapon from before is back in his hand.
        select(g.holsteredSlot > 0 ? g.holsteredSlot : 0);
    }
    g.wieldedLastFrame = CurrentSlot(ped) == kPhoneWeaponSlot && onFoot;
    g.holding = g.wieldedLastFrame;
    // Drawn in his left hand from model 330, as when it holds itself.
    if (g.holding) phone_model::Use(true);
}

void HoldPhone() {
    const uintptr_t ped = PlayerPed();
    if (!ped) {
        g.holding = false;
        return;
    }
    if (const int type = PhoneWeaponType(); type > 0) {
        WieldPhone(ped, type);
        return;
    }
    game::VehicleState vehicle{};
    const bool onFoot = !game::PlayerVehicleState(vehicle);
    const bool want = (g.out || g.camera.on) && onFoot && PlayerAble();
    auto slot = [&] { return static_cast<int>(*reinterpret_cast<const uint8_t*>(ped + kPedWeaponSlot)); };
    auto fists = [&] { reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(kSetCurrentWeapon)(ped, 0); };

    if (want && !g.holding) {
        EnsureModel(kCellphoneModel);
        // This phone's own model, not the game's (phone_model.h).
        phone_model::Use(true);
        g.holsteredSlot = slot();
        if (g.holsteredSlot != 0) fists();
        g.holding = true;
    } else if (want && g.holding) {
        // No weapon while the phone is in his hand.
        if (slot() != 0) fists();
        if (!g.phoneAtEar) phone_model::Use(true);
    } else if (!want && g.holding) {
        phone_model::ReleaseHand();
        if (g.holsteredSlot != 0 && slot() == 0) {
            reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(kSetCurrentWeapon)(ped, g.holsteredSlot);
        }
        // The game's phone back, before the model may be unloaded - unless
        // this phone is at CJ's ear for a call.
        if (!g.phoneAtEar) phone_model::Use(false);
        g.holsteredSlot = 0;
        g.holding = false;
    }
}


// Typing goes to whichever field the screen marked as focused last frame.
void TakeKeys() {
    g.enter = false;
    g.tab = false;
    input::Key key{};
    while (input::NextKey(key)) {
        g.activeAt = GetTickCount64();
        if (key.vk == VK_ESCAPE) {
            // Esc puts the icons down before anything else.
            if (g.arranging) {
                EndArranging();
                return;
            }
            // Esc backs out of the power menu before anything else.
            if (g.powerMenu) {
                g.powerMenu = false;
                return;
            }
            if (g.game) g.game.reset();
            else PutAway();
            return;
        }
        if (key.vk == VK_RETURN) { g.enter = true; continue; }
        // Up and Down move between fields. Not Tab: valkyrie-atmosphere has it.
        if (key.vk == VK_UP || key.vk == VK_DOWN) { g.tab = true; continue; }
        Field& f = g.field;
        if (!f.value) continue;
        if (key.vk == VK_BACK) {
            if (!f.value->empty()) {
                f.value->pop_back();
                if (KeyClicks()) sound::Click(sound::Tone::Key);
            }
        } else if (key.ch) {
            if (f.digits && !isdigit(static_cast<unsigned char>(key.ch)) && key.ch != '*' && key.ch != '#') continue;
            if (f.value->size() < f.max) {
                f.value->push_back(key.ch);
                if (KeyClicks()) sound::Click(sound::Tone::Key);
            }
        }
    }
}

// A Direct3D copy of one of the phone's own textures, read once from its
// RenderWare raster, for drawing the game's device can do more with.
IDirect3DTexture9* DeviceCopyOf(uintptr_t tex, const std::string& name) {
    static std::map<std::string, IDirect3DTexture9*> made;
    auto it = made.find(name);
    if (it != made.end()) return it->second;
    IDirect3DTexture9*& out = made[name];
    out = nullptr;
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    const uintptr_t raster = tex ? *reinterpret_cast<const uintptr_t*>(tex) : 0;
    if (!device || !raster) return nullptr;
    const int w = *reinterpret_cast<const int*>(raster + 0xC), h = *reinterpret_cast<const int*>(raster + 0x10);
    if (w <= 0 || h <= 0) return nullptr;
    auto* pixels = reinterpret_cast<const uint8_t*(__cdecl*)(uintptr_t, unsigned char, int)>(0x7FB2D0)(raster, 0, 2);
    if (!pixels) return nullptr;
    const int stride = *reinterpret_cast<const int*>(raster + 0x18);
    // With every mip level, each a 2x2 average of the one above, so the
    // handset stays smooth drawn smaller than its texture.
    if (SUCCEEDED(device->CreateTexture(w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &out, nullptr))) {
        std::vector<uint8_t> level(static_cast<size_t>(w) * h * 4);
        for (int row = 0; row < h; ++row) memcpy(level.data() + static_cast<size_t>(row) * w * 4, pixels + row * stride, w * 4);
        int lw = w, lh = h;
        for (DWORD i = 0; i < out->GetLevelCount(); ++i) {
            D3DLOCKED_RECT r{};
            if (FAILED(out->LockRect(i, &r, nullptr, 0))) {
                out->Release();
                out = nullptr;
                break;
            }
            for (int row = 0; row < lh; ++row) {
                memcpy(static_cast<uint8_t*>(r.pBits) + row * r.Pitch, level.data() + static_cast<size_t>(row) * lw * 4, lw * 4);
            }
            out->UnlockRect(i);
            const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
            std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
            for (int y2 = 0; y2 < nh; ++y2) {
                for (int x2 = 0; x2 < nw; ++x2) {
                    const int x0 = std::min(x2 * 2, lw - 1), x1 = std::min(x2 * 2 + 1, lw - 1);
                    const int y0 = std::min(y2 * 2, lh - 1), y1 = std::min(y2 * 2 + 1, lh - 1);
                    for (int c = 0; c < 4; ++c) {
                        auto at = [&](int px, int py) { return level[(static_cast<size_t>(py) * lw + px) * 4 + c]; };
                        next[(static_cast<size_t>(y2) * nw + x2) * 4 + c] =
                            static_cast<uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                    }
                }
            }
            level.swap(next);
            lw = nw;
            lh = nh;
        }
    }
    reinterpret_cast<void(__cdecl*)(uintptr_t)>(0x7FAEC0)(raster);
    logfile::Line("phone: %s %s for the light", name.c_str(), out ? "ready" : "could not be copied");
    return out;
}

IDirect3DTexture9* DeviceCopy(const char* name) { return DeviceCopyOf(ui::Tex(name), name); }

// The live reflection, when the world is being drawn for it: its Direct3D
// texture is had by letting RenderWare bind it to a spare stage
// (RwD3D9SetTexture) and reading that back, then unbinding it the same way
// so RenderWare's own record of the stage stays true. Held; the caller
// releases it.
IDirect3DTexture9* LiveMirror(IDirect3DDevice9* device) {
    if (false && !g.camera.on && config::Get().reflections && g_cameraPreviewReady && g_cameraPreview) {
        g_cameraPreview->AddRef();
        return g_cameraPreview;
    }
    if (!g_reflecting || !device) return nullptr;
    const uintptr_t mirror = viewfinder::Texture();
    if (!mirror) return nullptr;
    auto bind = reinterpret_cast<int(__cdecl*)(uintptr_t, unsigned)>(0x7FDE70);
    bind(mirror, 7);
    IDirect3DBaseTexture9* base = nullptr;
    IDirect3DTexture9* live = nullptr;
    if (SUCCEEDED(device->GetTexture(7, &base)) && base) {
        if (base->GetType() == D3DRTYPE_TEXTURE) live = static_cast<IDirect3DTexture9*>(base);
        else base->Release();
    }
    bind(0, 7);
    return live;
}

// --- The phone as its 3D model ------------------------------------------------
//
// phone3d draws it; this finds it its model, textures and a device reset it
// survives. Once tried and failed it stays off, and the flat handset is drawn.

// Taking a picture, the game camera goes to the lens for a few frames, with
// the HUD and the phone put away, because the game saves the picture from
// the screen. The player is not shown that: the last ordinary frame is
// copied as the picture is asked for and laid over those frames. The game
// has saved its picture by then (CPostEffects, before the HUD is drawn).
IDirect3DTexture9* g_freeze = nullptr;
UINT g_freezeSize[2] = {};
bool g_frozen = false;

void FreezeFrame() {
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    if (!device) return;
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetRenderTarget(0, &back)) || !back) return;
    D3DSURFACE_DESC desc{};
    back->GetDesc(&desc);
    if (g_freeze && (g_freezeSize[0] != desc.Width || g_freezeSize[1] != desc.Height)) {
        g_freeze->Release();
        g_freeze = nullptr;
    }
    if (!g_freeze && SUCCEEDED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                      D3DPOOL_DEFAULT, &g_freeze, nullptr))) {
        g_freezeSize[0] = desc.Width;
        g_freezeSize[1] = desc.Height;
    }
    IDirect3DSurface9* copy = nullptr;
    if (g_freeze && SUCCEEDED(g_freeze->GetSurfaceLevel(0, &copy))) {
        g_frozen = SUCCEEDED(device->StretchRect(back, nullptr, copy, nullptr, D3DTEXF_NONE));
        copy->Release();
    }
    back->Release();
}

void CaptureCameraPreview() {
    g_cameraPreviewReady = false;
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    if (!device) return;
    IDirect3DSurface9* source = nullptr;
    if (FAILED(device->GetRenderTarget(0, &source)) || !source) return;
    D3DSURFACE_DESC desc{};
    source->GetDesc(&desc);
    if (g_cameraPreview && (g_cameraPreviewSize[0] != desc.Width || g_cameraPreviewSize[1] != desc.Height)) {
        g_cameraPreview->Release();
        g_cameraPreview = nullptr;
    }
    if (!g_cameraPreview && SUCCEEDED(device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
            desc.Format, D3DPOOL_DEFAULT, &g_cameraPreview, nullptr))) {
        g_cameraPreviewSize[0] = desc.Width;
        g_cameraPreviewSize[1] = desc.Height;
    }
    IDirect3DSurface9* target = nullptr;
    if (g_cameraPreview && SUCCEEDED(g_cameraPreview->GetSurfaceLevel(0, &target))) {
        g_cameraPreviewReady = SUCCEEDED(device->StretchRect(source, nullptr, target, nullptr, D3DTEXF_NONE));
        target->Release();
    }
    source->Release();
}

void ReleaseFreeze() {
    if (g_freeze) g_freeze->Release();
    g_freeze = nullptr;
    g_frozen = false;
}

using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn g_previousReset = nullptr;

HRESULT __stdcall ResetDevice(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    // Its render targets live in the device's own memory, which a reset needs
    // given back first.
    phone3d::ReleaseDeviceObjects();
    viewfinder::ReleaseDeviceObjects();
    if (g_cameraPreview) g_cameraPreview->Release();
    g_cameraPreview = nullptr;
    g_cameraPreviewReady = false;
    ReleaseFreeze();
    return g_previousReset(device, params);
}

IDirect3DTexture9* g_parts3d[phone3d::kParts] = {};

bool Phone3dReady(IDirect3DDevice9* device) {
    static int state = 0;  // 0 untried, 1 ready, -1 not to be had
    if (state) return state > 0 && device && device->TestCooperativeLevel() == D3D_OK;
    state = -1;
    if (!device || !config::Get().model3d) return false;
    std::string txd = KeypadSkin() ? bundle::Path("valkyrie-phone-model-sm.txd") : std::string();
    if (txd.empty()) txd = bundle::Path("valkyrie-phone-model.txd");
    const std::string dff = bundle::Path("valkyrie-phone-model.dff");
    if (dff.empty() || txd.empty() || !phone3d::Load(device, dff)) {
        logfile::Line("phone: the 3D phone could not be loaded - the flat one is drawn");
        return false;
    }
    const int slot = sprite::LoadDictionary("valkyrie_phone_3d", txd.c_str());
    for (int k = 0; k < phone3d::kParts; ++k) {
        if (k == phone3d::kScreen) continue;
        const uintptr_t tex = slot >= 0 ? sprite::Find(slot, phone3d::kPartTextures[k]) : 0;
        g_parts3d[k] = tex ? DeviceCopyOf(tex, std::string("3d:") + phone3d::kPartTextures[k]) : nullptr;
        if (!g_parts3d[k]) {
            logfile::Line("phone: the 3D phone has no %s - the flat one is drawn", phone3d::kPartTextures[k]);
            return false;
        }
    }
    auto** table = *reinterpret_cast<void***>(device);
    DWORD protection = 0;
    if (!VirtualProtect(&table[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &protection)) return false;
    g_previousReset = reinterpret_cast<ResetFn>(table[16]);
    table[16] = reinterpret_cast<void*>(&ResetDevice);
    VirtualProtect(&table[16], sizeof(void*), protection, &protection);
    logfile::Line("phone: the 3D phone is ready");
    state = 1;
    return true;
}

// The handset lit as the game lights its own things: a small pixel shader,
// compiled at run time with Windows' d3dcompiler_47.dll, lights every pixel
// of the body from its shape (phone_normal) and what it is made of
// (phone_material) with the game's own light - the sun from where the time
// cycle puts it, the ambient it lights objects with - and mirrors its
// surroundings (phone_env) turned with the camera. A second pass lays the
// glass's share of it over the screen once the screen is drawn.
struct Lighting {
    float light[4];    // the sun's direction as the phone sees it: x right, y up, z out of the glass
    float sun[4];      // its colour, and strength
    float ambient[4];
    float env[4];      // the reflection's turn and tilt, and strength
};

// The textbook model, as real-time renderers have used it for decades:
// Lambert diffuse, a Blinn-Phong highlight normalised so a sharper one is
// smaller rather than dimmer, and Schlick's approximation of Fresnel, which
// makes glass reflect 4% head on and far more at a glancing angle, and a
// metal reflect in its own colour. The view direction is worked out per
// pixel from an eye in front of the game's screen, so a flat pane catches
// the sun in a spot that moves as the phone does, never all over at once.
const char kLightingShader[] = R"(
sampler2D albedo : register(s0);
sampler2D normals : register(s1);
sampler2D material : register(s2);
sampler2D env : register(s3);
float4 lightDir : register(c0);
float4 sunColour : register(c1);
float4 ambient : register(c2);
float4 envParams : register(c3);
float4 mode : register(c4);
float4 screenRect : register(c5);
float4 rect : register(c6);  // the handset on the game's screen, in pixels: left, top, width, height
float4 eye : register(c7);   // the eye: the screen's middle x, y, and its distance in pixels
float3 schlick(float3 f0, float3 f90, float c) {
    float k = 1 - c, k2 = k * k;
    return f0 + (f90 - f0) * (k2 * k2 * k);
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float4 a = tex2D(albedo, uv);
    float3 n = tex2D(normals, uv).xyz * 2 - 1;
    n.z = sqrt(saturate(1 - dot(n.xy, n.xy)));
    n = normalize(n);
    float4 m = tex2D(material, uv);  // r gloss, g smoothness, b metal, a how much it mirrors
    float2 px = rect.xy + uv * rect.zw;
    float3 v = normalize(float3(eye.x - px.x, px.y - eye.y, eye.z));
    float3 l = normalize(lightDir.xyz);
    float3 h = normalize(l + v);
    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v));
    // Smooth glass gives the sun back as a small, tight glint; its peak is
    // held down so it never blinds.
    float power = exp2(1 + m.g * 12);
    float3 f0 = lerp(float3(0.06, 0.06, 0.06), a.rgb, m.b);
    float3 sun = sunColour.rgb * sunColour.a;
    float3 spec = schlick(f0, 1, saturate(dot(h, v))) * (pow(saturate(dot(n, h)), power) * min((power + 8) / 8, 40) * ndl * m.r);
    // The surroundings, along the reflected ray. envParams.w: 1 when env is
    // the live mirror render (looking back from the phone at the camera:
    // what lies to the viewer's right is on its left); 0 for the studio
    // picture, turned with the camera and lit by the game's light.
    // The live picture is what a flat mirror at the phone shows: laid over
    // the face of the phone as it is (turned left for right, as a mirror
    // turns it), each point bent by the surface's own slope and the angle
    // it is seen from.
    float3 r = reflect(-v, n);
    float2 euv = envParams.w > 0.5 ? float2(0.5 - (uv.x - 0.5) * 0.34 - 0.4 * r.x, 0.5 + (uv.y - 0.5) * 0.62 - 0.5 * r.y)
                                   : float2(0.5 + 0.5 * r.x + envParams.x, 0.5 - 0.5 * r.y + envParams.y);
    float3 around = tex2D(env, euv).rgb * (envParams.w > 0.5 ? float3(1, 1, 1) : ambient.rgb + sun * 0.6);
    // A rougher surface sees its surroundings blurred toward their average.
    around = lerp(ambient.rgb, around, m.g);
    float3 fe = schlick(f0, max(m.g, f0), ndv);
    // Glass mirrors more than its 4% would show over a lit display, as a
    // game's glass does, so the world above can be seen in it.
    // Over the lit display the world shows only faintly, as on a real phone
    // whose screen is on - never a second picture over the first.
    float faint = mode.x > 0.5 ? 0.3 : 1;
    float3 shine = spec * sun + fe * around * (m.a * envParams.z * (1 + (1 - m.b) * 1.5) * faint);
    // Rolled off softly toward white rather than clipped.
    shine = 1 - exp(-shine);
    if (mode.x > 0.5) {
        // Built as the original iPhone is: the display, a hair of air, then
        // the glass, black-printed round the opening. Seen at an angle, the
        // print's edge lies over the display's own a little way in (the gap
        // times the tangent of the angle), so the picture's edge shifts as
        // the phone does; the gap is always a touch dark. In the display's
        // own points, 320 x 480, as big as its pixels.
        float2 span = screenRect.zw - screenRect.xy;
        float inside = step(screenRect.x, uv.x) * step(uv.x, screenRect.z) * step(screenRect.y, uv.y) * step(uv.y, screenRect.w);
        float2 pt = (uv - screenRect.xy) / span * float2(320, 480);
        float2 shift = float2(-v.x, v.y) / max(v.z, 0.2) * 6;
        float2 lo = pt - max(shift, 0);
        float2 hi = float2(320, 480) - pt - max(-shift, 0);
        float edge = min(min(lo.x, lo.y), min(hi.x, hi.y));
        float cover = lerp(0.55, 1, saturate(edge / 1.5)) * saturate(edge + 1);
        // Its pixel grid: faint dark lines between the pixels, only where
        // the game's screen has pixels enough to show them without shimmer.
        float perPoint = rect.z * span.x / 320;
        float2 f = abs(frac(pt) - 0.5) * 2;
        float grid = 1 - 0.1 * saturate((perPoint - 2) / 2) * saturate(max(f.x, f.y) * 4 - 3);
        return float4(saturate(shine * inside), lerp(1, cover * grid, inside));
    }
    float3 diffuse = (1 - fe) * (1 - m.b) * a.rgb * (ambient.rgb + sun * ndl);
    return float4(saturate(diffuse + shine), a.a);
}
)";

IDirect3DPixelShader9* LightingShader() {
    static IDirect3DPixelShader9* shader = nullptr;
    static bool tried = false;
    if (tried) return shader;
    tried = true;
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!device || !compile) {
        logfile::Line("phone: no shader compiler - the handset is lit plainly");
        return nullptr;
    }
    // The plainest profile that holds it, then the larger ones cards of the
    // day added, until the device takes one.
    for (const char* profile : {"ps_2_0", "ps_2_a", "ps_2_b"}) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = compile(kLightingShader, sizeof kLightingShader - 1, "phone-light", nullptr, nullptr,
                                   "main", profile, 0, 0, &code, &errors);
        if (FAILED(hr) || !code) {
            logfile::Line("phone: the lighting shader did not compile for %s: %s", profile,
                          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no reason given");
        } else if (SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader))) {
            logfile::Line("phone: handset lighting shader ready (%s)", profile);
        } else {
            shader = nullptr;
            logfile::Line("phone: the device would not take the lighting shader as %s", profile);
        }
        if (code) code->Release();
        if (errors) errors->Release();
        if (shader) break;
    }
    return shader;
}

// Draw the handset lit (glassOnly false), or the glass's light over the
// screen (true). False when it cannot, so the plain handset is drawn.
bool LightHandset(bool glassOnly, float x, float y, float w, float h, const Lighting& light) {
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    IDirect3DPixelShader9* shader = LightingShader();
    IDirect3DTexture9* body = DeviceCopy(BodyArt("body"));
    IDirect3DTexture9* normals = DeviceCopy(BodyArt("phone_normal"));
    IDirect3DTexture9* material = DeviceCopy(BodyArt("phone_material"));
    IDirect3DTexture9* env = DeviceCopy("phone_env");
    // The live reflection, when the world is being drawn for it: its
    // Direct3D texture is had by letting RenderWare bind it to a spare stage
    // (RwD3D9SetTexture) and reading that back, then unbinding it the same
    // way so RenderWare's own record of the stage stays true.
    IDirect3DTexture9* live = nullptr;
    if (g_reflecting && device) {
        if (const uintptr_t mirror = viewfinder::Texture()) {
            auto bind = reinterpret_cast<int(__cdecl*)(uintptr_t, unsigned)>(0x7FDE70);
            bind(mirror, 7);
            IDirect3DBaseTexture9* base = nullptr;
            if (SUCCEEDED(device->GetTexture(7, &base)) && base) {
                if (base->GetType() == D3DRTYPE_TEXTURE) live = static_cast<IDirect3DTexture9*>(base);
                else base->Release();
            }
            bind(0, 7);
        }
    }
    if (false && !g.camera.on && config::Get().reflections && g_cameraPreviewReady && g_cameraPreview) {
        live = g_cameraPreview;
        live->AddRef();
    }
    if (live) env = live;
    struct Holder {
        IDirect3DTexture9* t;
        ~Holder() { if (t) t->Release(); }
    } holder{live};
    if (!device || !shader || !body || !normals || !material || !env) return false;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) return false;
    saved->Capture();
    struct V {
        float x, y, z, rhw, u, v;
    } quad[4] = {{x - 0.5f, y - 0.5f, 0, 1, 0, 0}, {x + w - 0.5f, y - 0.5f, 0, 1, 1, 0},
                 {x - 0.5f, y + h - 0.5f, 0, 1, 0, 1}, {x + w - 0.5f, y + h - 0.5f, 0, 1, 1, 1}};
    device->SetVertexShader(nullptr);
    device->SetPixelShader(shader);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    // The glass adds its light and, through alpha, lays the display's depth
    // and pixel grid over what is under it.
    device->SetRenderState(D3DRS_SRCBLEND, glassOnly ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, glassOnly ? D3DBLEND_SRCALPHA : D3DBLEND_INVSRCALPHA);
    IDirect3DTexture9* textures[4] = {body, normals, material, env};
    for (DWORD i = 0; i < 4; ++i) {
        device->SetTexture(i, textures[i]);
        device->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(i, D3DSAMP_MIPFILTER, i == 3 && live ? D3DTEXF_NONE : D3DTEXF_LINEAR);
        const DWORD address = i == 3 && !live ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
        device->SetSamplerState(i, D3DSAMP_ADDRESSU, address);
        device->SetSamplerState(i, D3DSAMP_ADDRESSV, i == 3 ? static_cast<DWORD>(D3DTADDRESS_CLAMP) : address);
    }
    const float mode[4] = {glassOnly ? 1.0f : 0.0f, 0, 0, 0};
    const float screen[4] = {phone_art::kScreenLeft, phone_art::kScreenTop, phone_art::kScreenRight,
                             phone_art::kScreenBottom};
    device->SetPixelShaderConstantF(0, light.light, 1);
    device->SetPixelShaderConstantF(1, light.sun, 1);
    device->SetPixelShaderConstantF(2, light.ambient, 1);
    float env4[4] = {light.env[0], light.env[1], light.env[2], live ? 1.0f : 0.0f};
    device->SetPixelShaderConstantF(3, env4, 1);
    device->SetPixelShaderConstantF(4, mode, 1);
    device->SetPixelShaderConstantF(5, screen, 1);
    const game::Point size = game::ScreenSize();
    const float where[4] = {x, y, w, h};
    // About as far as the game's own camera sits from its picture.
    const float eye[4] = {size.x * 0.5f, size.y * 0.5f, size.y * 1.2f, 0};
    device->SetPixelShaderConstantF(6, where, 1);
    device->SetPixelShaderConstantF(7, eye, 1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    saved->Apply();
    saved->Release();
    return true;
}

}  // namespace

void Configure(const std::string& gameDir) {
    g_gameDir = gameDir;
    // What the ASI carries, before the ini lists the tones it holds.
    bundle::Unpack();
    config::Load(gameDir);
    ApplyLook();
    sound::SetGameDir(gameDir);
    const config::Config& c = config::Get();
    g_config.key = c.key;
    g_config.height = c.height;
    g_config.right = c.right;
    logfile::Line("phone: key %d, height %.2f, %s side", g_config.key, g_config.height,
                  g_config.right ? "right" : "left");
}

void BeforeFrame() {
    if (g.loaded) CameraBeforeFrame();
}

void BloodFrame();
void Frame() {
    ++g.frame;
    input::Install();
    static bool iconHooked = false;
    if (!iconHooked) {
        iconHooked = true;
        phone_model::HookWeaponPass();
    }

    if (!g.loaded && !*reinterpret_cast<const bool*>(kGameNotLoaded) && PlayerPed()) {
        g.loaded = true;
        const char* user = reinterpret_cast<const char*(__cdecl*)()>(kUserFolder)();
        std::string folder = user ? user : g_gameDir;
        if (!folder.empty() && folder.back() != '\\') folder += '\\';
        phone_data::Init(folder + "valkyrie-phone.dat");
        phone_data::Load();
        for (const auto& c : config::Get().contacts) {
            if (!BuiltInHidden(c.name)) phone_data::Get().builtIn.push_back({c.name, c.number, c.picture});
        }
        // The phone's own pictures, from the ASI (the game folder's, for a
        // build without them).
        std::string txd = bundle::Path("valkyrie-phone.txd");
        if (txd.empty()) txd = g_gameDir + "valkyrie-phone.txd";
        g.dictionary = sprite::LoadDictionary("valkyrie_phone", txd.c_str());
        // The game's own pictures, from the player's own files: the radar
        // icons and the menu's mouse cursor.
        g.hud = sprite::LoadDictionary("valkyrie_phone_hud", (g_gameDir + "models\\hud.txd").c_str());
        g.frontend = sprite::LoadDictionary("valkyrie_phone_frontend", (g_gameDir + "models\\fronten_pc.txd").c_str());
        ui::SetDictionary(g.dictionary);
        // The internet: the pack built into the ASI, else one beside the game.
        std::string pages = bundle::Path("valkyrie-web.dat");
        if (pages.empty()) pages = g_gameDir + "valkyrie-web.dat";
        web::Open(pages);
        // The signal: expanded-map's masts under Eagle, San Andreas' own
        // elsewhere.
        std::string signal = bundle::Path("valkyrie-signal.bin");
        if (signal.empty()) signal = g_gameDir + "valkyrie-signal.bin";
        coverage::Load(signal);
        // The phone's own model, from the ASI, unless the ini names another
        // one from the game folder.
        std::string modelFile = g_gameDir + config::Get().model;
        std::string modelTextures = g_gameDir + config::Get().modelTextures;
        if (_stricmp(config::Get().model.c_str(), "valkyrie-phone-model.dff") == 0 &&
            !bundle::Path("valkyrie-phone-model.dff").empty()) {
            modelFile = bundle::Path("valkyrie-phone-model.dff");
            modelTextures = KeypadSkin() ? bundle::Path("valkyrie-phone-model-sm.txd") : std::string();
            if (modelTextures.empty()) modelTextures = bundle::Path("valkyrie-phone-model.txd");
        }
        if (phone_model::Load(modelFile, modelTextures) &&
            config::Get().shine > 0.0f) {
            // The game's own car reflection, from the vehicle dictionary it
            // keeps loaded; this phone's own if that is not there.
            const int vehicle = sprite::LoadDictionary("vehicle", "");
            uintptr_t env = vehicle >= 0 ? sprite::Find(vehicle, "vehicleenvmap128") : 0;
            if (!env) env = ui::Tex("phone_env");
            phone_model::MakeShiny(env, config::Get().shine);
        }
        g.screen = phone_data::Get().settings.slideToUnlock ? Screen::Lock : Screen::Home;
        sound::SetVolume(phone_data::Get().settings.volume / 10.0f);
        // Everything taking the phone out would otherwise wait on, made now
        // while the game loads: the 3D phone, its shaders and its glass, and
        // the game's phone model for CJ's hand, kept loaded from here on.
        const ULONGLONG readyFrom = GetTickCount64();
        auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
        if (Phone3dReady(device)) {
            phone3d::Pose pose{};
            pose.wear = config::Get().features.scratches ? config::Get().features.scratchAmount : 0.0f;
            pose.wearStrong = config::Get().features.scratchStrong;
            if (!phone3d::Prepare(device, pose)) logfile::Line("phone: the 3D phone's shaders would not compile");
        }
        EnsureModel(kCellphoneModel);
        GuardPhoneRadio();
        logfile::Line("phone: made ready in %llu ms", GetTickCount64() - readyFrom);
    }

    // The hotkey: out and raised, then put away. Typing into a field takes
    // the key as a letter instead.
    const bool typing = g.focused && g.field.value && !g.field.digits;
    // Asked for by another mod: taken out once the menu that asked has gone.
    // Already out, it is raised; asked while it cannot be used, the ask is
    // dropped after a couple of seconds and the mouse given back.
    if (g_openAsked) {
        if (g.out) {
            InterlockedExchange(&g_openAsked, 0);
            if (!g.focused) Raise();
        } else if (PlayerCanUsePhone() && !PlayerSwimming()) {
            InterlockedExchange(&g_openAsked, 0);
            Open();
            if (!g.out) input::Capture(false);
        } else if (GetTickCount64() - g_openAskedAt > 2000) {
            InterlockedExchange(&g_openAsked, 0);
            logfile::Line("phone: asked to open but the phone cannot be used now");
            input::Capture(false);
        }
    }
    if (g_config.key && input::KeyPressed(g_config.key) && !typing) {
        // The key wins over an ask still waiting.
        InterlockedExchange(&g_openAsked, 0);
        if (g.camera.on) PutAway();
        else if (!g.out) Open();
        else if (!g.focused) Raise();
        else PutAway();
    }
    // Lowered at his side, in his hand: the right button brings it up, as
    // it takes it down again. Meanwhile the game does not get that button -
    // the phone is nothing to aim.
    const bool rightRaises = g.out && !g.focused && g.holding && !g.phoneAtEar && !g.camera.on &&
                             PlayerCanUsePhone() && !PlayerSwimming();
    input::ClaimRight(rightRaises);
    if (rightRaises && input::RightClaimPressed()) {
        InterlockedExchange(&g_openAsked, 0);
        Raise();
    }

    // Never hold on to the mouse when the phone is not on screen to use it -
    // a cutscene, a death, the pause menu. The menu only lowers it: it is
    // still in CJ's hand when the game goes on.
    // Just taken out or raised it has until its first frame drawn (a menu
    // that asked for it may still be closing), then it must go on being drawn.
    const bool drawing = !(g.drawnSinceUp || g.frame > g.drawGrace) || g.frame - g.drawnFrame <= 3;
    if (g.out && (!PlayerCanUsePhone() || !drawing)) {
        if (!PlayerAble()) PutAway();
        else if (g.focused) Lower();
    }
    // Out but not drawn for a couple of seconds of play (the HUD hidden,
    // another mod drawing instead): put away, so CJ is not left holding a
    // phone nobody can see, with his weapons locked away.
    if (g.out && PlayerCanUsePhone() && (g.drawnSinceUp || g.frame > g.drawGrace) && g.frame - g.drawnFrame > 120) {
        logfile::Line("phone: not drawn for a while - putting it away");
        PutAway();
    }
    // The keyboard and mouse are the phone's only while it is up and in use
    // (or an ask from another mod is about to take it out): never left held
    // with the phone lowered, put away or not on screen, which would stop CJ
    // moving with nothing to show why.
    if (input::Captured() && !g_openAsked && !(g.out && g.focused)) {
        logfile::Line("phone: had the keyboard and mouse while not in use - given back");
        input::Capture(false);
    }
    // Swimming: into the pocket, unless a call is on (it stays at his ear).
    if (g.out && g.loaded && !g.call.active && PlayerAble() && PlayerSwimming()) PutAway();
    // Nobody to talk to once CJ is dead or a cutscene starts.
    if (g.call.active && g.loaded && !PlayerAble()) EndCall();

    if (g.pendingSave && !g.out && g.slide <= 0.0f) {
        g.pendingSave = false;
        script::Command(kActivateSaveMenu, {});
    }

    UpdateCall();
    if (g.loaded) RadioFrame();

    // CJ's pose first: a call about to go to his ear needs his hands free
    // before the game's phone task takes them.
    if (g.loaded) UpdatePose();

    // The phone at CJ's ear for as long as a call is up.
    // Swimming, the call goes on hands-free (the game's phone task does not
    // swim) and back to his ear once he is out of the water.
    const bool wantEar = g.call.active && !g.call.speaker && PlayerAble() && !PlayerSwimming();
    if (wantEar != g.phoneAtEar) {
        // The call's phone is this one too: the task puts model 330 in CJ's
        // hand when it starts.
        if (wantEar) phone_model::Use(true);
        if (const uintptr_t ped = PlayerPed()) {
            script::Command(kTaskUseMobilePhone, {script::PedHandle(ped), wantEar});
            logfile::Line("phone: %s the ear", wantEar ? "to" : "away from");
            // Once it has had time to put the phone away, the game's talking
            // animation must be off him too: looked for, and cleared if not.
            if (!wantEar) {
                for (const char* name : {"phone_talk", "phone_in"}) {
                    if (std::find(g_pose.used.begin(), g_pose.used.end(), name) == g_pose.used.end())
                        g_pose.used.push_back(name);
                }
                const ULONGLONG t = GetTickCount64();
                g_pose.sweepFrom = t + 2500;
                g_pose.sweepUntil = t + 5500;
                g_pose.sweeps = 0;
            }
        }
        g.phoneAtEar = wantEar;
        g.earLostAt = 0;
    }
    // At his ear for a call, the game's talking animation must be on him. A
    // jump, a fall, a shove or a punch takes him out of the phone task, and
    // it is not given back by itself: once he is on his feet again, with the
    // call still on, the phone goes back to his ear.
    if (g.phoneAtEar && wantEar) {
        if (const uintptr_t ped = PlayerPed()) {
            const int handle = script::PedHandle(ped);
            const bool talking = script::Command(kPlayingAnim, {handle, "phone_talk"}) ||
                                 script::Command(kPlayingAnim, {handle, "phone_in"});
            const bool steady = !script::Command(kInAir, {handle}) && !script::Command(kInWater, {handle}) &&
                                !InTransition(ped);
            const ULONGLONG now = GetTickCount64();
            if (talking || !steady || now < g.earRetryAt) {
                g.earLostAt = 0;
            } else if (!g.earLostAt) {
                g.earLostAt = now;
            } else if (now - g.earLostAt > 1200) {
                logfile::Line("phone: CJ lost the phone from his ear mid-call - putting it back");
                phone_model::Use(true);
                script::Command(kTaskUseMobilePhone, {handle, true});
                g.earLostAt = 0;
                g.earRetryAt = now + 2500;  // time for the task to start before looking again
            }
        }
    }
    if (g.loaded) HoldPhone();
    phone_model::SetHand(g.holding && !g.phoneAtEar && g.loaded, config::Get().handTurn,
                         config::Get().handOffset, config::Get().handFlip);
    // Neither in the hand nor at the ear: model 330 is the game's again.
    if (!g.holding && !g.phoneAtEar) phone_model::Use(false);
    UpdateCamera();
    if (g.loaded) CheckAlarm();
    CheckTimer();
    sound::Update();

    FlashlightFrame();
    BloodFrame();
    coverage::Update(config::Get().features.signal);
    // Texts held while out of service, now it is back.
    if (coverage::HasService() && !g_waitingTexts.empty()) {
        const auto waiting = g_waitingTexts;
        g_waitingTexts.clear();
        for (const auto& [number, body] : waiting) IncomingText(number.c_str(), body.c_str());
    }
    static bool told = false;
    if (!told) {
        told = true;
        const config::Config& cfg = config::Get();
        services::SetMessenger(&IncomingText);
        services::SetNumbers(cfg.pizza.c_str(), cfg.burger.c_str(), cfg.chicken.c_str());
    }
    services::Process();
}

// --- Rain on the glass ------------------------------------------------------
//
// Out in the rain, drops land on the screen, the bigger ones run down it, and
// out of the rain they dry off. It rains on the phone where the game lets rain
// fall (CWeather::AddRain: raining, neither the player nor the camera in a
// no-rain zone, not in an interior, not under water), unless CJ is shut in a
// vehicle.

// The front glass, in the screen's own points (its top left is 0, 0): the
// whole front of the phone, past the screen's edges. Rain and cracks lie on
// all of it. Set each frame from where the handset is drawn.
struct GlassArea {
    float left = 0.0f, top = 0.0f, right = ui::kScreenW, bottom = ui::kScreenH;
};
GlassArea g_glassArea;

struct Drop {
    float x, y, r;     // on the glass, in the screen's points
    float speed;       // running down it, points a second; 0 while it clings
    ULONGLONG born, dries;
    // A runner: a drop grown heavy enough to slide, as rain on a window
    // does - stopping and starting, swallowing the small drops in its way,
    // leaving a line of beads behind it.
    bool runner = false;
    ULONGLONG pauseUntil = 0;
    float wobble = 0.0f, lastBead = 0.0f;
    float px = 0.0f, py = 0.0f;  // where it was last frame
    // Caught on a crack, it follows the break down a little way.
    float followX = 0.0f, followY = 0.0f, followLeft = 0.0f;
    float blood = 0.0f;  // how red it has turned, 0 to 1
    bool landed = false;  // looked at for blood under it when it landed
};
std::vector<Drop> g_drops;
float g_dropsDue = 0.0f;

// Vehicles with nothing over CJ's head: the bikes, quads and boats.
bool OpenVehicle(int model) {
    static const int kOpenTop[] = {448, 461, 462, 463, 468, 471, 481, 509, 510, 521, 522, 523, 581, 586,  // bikes
                                430, 446, 452, 453, 454, 472, 473, 484, 493, 595};                     // boats
    return std::find(std::begin(kOpenTop), std::end(kOpenTop), model) != std::end(kOpenTop);
}

// How hard it is raining on the phone: 0 to 1.
float RainOnPhone() {
    const float rain = *reinterpret_cast<const float*>(0xC81324);  // CWeather::Rain
    if (rain <= 0.0f) return 0.0f;
    if (*reinterpret_cast<const int*>(0xB72914) != 0) return 0.0f;        // CGame::currArea: an interior
    if (*reinterpret_cast<const float*>(0xC8132C) > 0.0f) return 0.0f;    // CWeather::UnderWaterness
    using NoRain = bool(__cdecl*)();
    if (reinterpret_cast<NoRain>(0x72DDB0)() || reinterpret_cast<NoRain>(0x72DDC0)()) return 0.0f;  // CCullZones
    game::VehicleState v{};
    if (game::PlayerVehicleState(v) && !OpenVehicle(v.model)) return 0.0f;
    return std::min(rain, 1.0f);
}

// Rain on the glass, a frame on: new drops land, heavy ones run.
void RainUpdate(float dt) {
    static std::mt19937 rng(GetTickCount());
    auto rnd = [](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    const ULONGLONG now = GetTickCount64();
    const auto& features = config::Get().features;
    const float rain = features.rain ? RainOnPhone() * features.rainAmount : 0.0f;
    // Out of the rain every drop dries off within a few seconds.
    if (rain <= 0.0f) {
        for (Drop& d : g_drops) d.dries = std::min<ULONGLONG>(d.dries, now + 2500 + static_cast<ULONGLONG>(d.r * 400));
    }
    // As many for the glass as its size gives it.
    const GlassArea& area = g_glassArea;
    const float share = (area.right - area.left) * (area.bottom - area.top) / (ui::kScreenW * ui::kScreenH);
    g_dropsDue += rain * 18.0f * share * dt;
    while (g_dropsDue >= 1.0f && g_drops.size() < static_cast<size_t>(140.0f * share)) {
        g_dropsDue -= 1.0f;
        const float r = rnd(0.0f, 1.0f) < 0.8f ? rnd(1.0f, 2.8f) : rnd(2.8f, 5.0f);
        Drop d{rnd(area.left + 6.0f, area.right - 6.0f), rnd(area.top + 6.0f, area.bottom - 6.0f), r, 0.0f,
               now, now + static_cast<ULONGLONG>(rnd(7000.0f, 16000.0f))};
        d.wobble = rnd(0.0f, 6.28f);
        d.px = d.x;
        d.py = d.y;
        g_drops.push_back(d);
    }
    if (g_dropsDue >= 1.0f) g_dropsDue = 0.0f;
    std::vector<Drop> beads;
    for (size_t i = 0; i < g_drops.size(); ++i) {
        Drop& d = g_drops[i];
        d.px = d.x;
        d.py = d.y;
        if (now >= d.dries) continue;
        // Heavy enough, and settled a moment: it lets go.
        if (!d.runner && d.r >= 3.2f && now - d.born > 1200) {
            d.runner = true;
            d.lastBead = d.y;
        }
        if (!d.runner || now < d.pauseUntil) continue;
        // Stick and slip: now and then it holds on for a moment.
        if (rnd(0.0f, 1.0f) < dt * 0.7f) {
            d.pauseUntil = now + static_cast<ULONGLONG>(rnd(150.0f, 700.0f));
            d.speed *= 0.3f;
            continue;
        }
        d.speed = std::min(75.0f, d.speed + 45.0f * dt);
        const float step = d.speed * dt;
        if (d.followLeft > 0.0f) {
            d.x += d.followX * step;
            d.y += std::max(0.2f, d.followY) * step;
            d.followLeft -= step;
        } else {
            d.x += std::sin(d.wobble + d.y * 0.07f) * 0.35f * step;
            d.y += step;
        }
        // It swallows the small drops it runs into.
        for (size_t j = 0; j < g_drops.size(); ++j) {
            Drop& o = g_drops[j];
            if (j == i || o.runner || now >= o.dries) continue;
            if (std::hypot(o.x - d.x, o.y - d.y) < d.r + o.r * 0.7f) {
                d.r = std::min(7.0f, std::sqrt(d.r * d.r + o.r * o.r * 0.6f));
                d.blood = std::max(d.blood, o.blood * 0.8f);
                o.dries = now;
            }
        }
        // And leaves beads behind it, getting smaller as it goes.
        if (d.y - d.lastBead > rnd(5.0f, 9.0f)) {
            d.lastBead = d.y;
            Drop bead{d.x + rnd(-0.6f, 0.6f), d.y - d.r * 0.9f, rnd(0.5f, 1.3f), 0.0f, now,
                      now + static_cast<ULONGLONG>(rnd(3500.0f, 9000.0f))};
            bead.blood = d.blood * 0.7f;
            bead.landed = true;
            bead.px = bead.x;
            bead.py = bead.y;
            beads.push_back(bead);
            d.r = std::max(2.3f, d.r - 0.05f);
        }
        if (d.r < 2.6f) {
            d.runner = false;
            d.speed = 0.0f;
        }
    }
    g_drops.erase(std::remove_if(g_drops.begin(), g_drops.end(),
                                 [&](const Drop& d) { return now >= d.dries || d.y - d.r > area.bottom; }),
                  g_drops.end());
    if (g_drops.size() + beads.size() < 400) g_drops.insert(g_drops.end(), beads.begin(), beads.end());
}

// A drop is a little lens: the screen behind it darker at its lower edge, a
// faint film over it, and a bright point where it catches the light from
// above; with blood in it, red.
void DrawRain() {
    const ULONGLONG now = GetTickCount64();
    for (const Drop& d : g_drops) {
        const float fadeIn = std::min(1.0f, (now - d.born) / 150.0f);
        const float fadeOut = std::min(1.0f, (d.dries - now) / 1500.0f);
        const float a = fadeIn * fadeOut;
        auto alpha = [&](uint32_t argb) {
            return (static_cast<uint32_t>((argb >> 24) * a) << 24) | (argb & 0xFFFFFF);
        };
        auto mix = [](uint32_t c0, uint32_t c1, float t) {
            uint32_t out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                const float v0 = static_cast<float>((c0 >> sh) & 0xFF), v1 = static_cast<float>((c1 >> sh) & 0xFF);
                out |= static_cast<uint32_t>(v0 + (v1 - v0) * t) << sh;
            }
            return out;
        };
        const float r = d.r;
        const bool moving = d.runner && d.speed > 0.0f;
        const float sx = moving ? 0.85f : 1.0f, sy = moving ? 1.3f : 1.0f;
        ui::Image("disc", d.x - r * sx, d.y - r * sy + r * 0.35f, 2 * r * sx, 2 * r * sy, alpha(0x46000000));
        ui::Image("disc", d.x - r * sx, d.y - r * sy, 2 * r * sx, 2 * r * sy,
                  alpha(mix(0x30DCE8F0, 0x90A01010, d.blood)));
        ui::Image("disc", d.x - r * 0.55f, d.y - r * 0.6f, r * 0.55f, r * 0.55f,
                  alpha(mix(0xD0FFFFFF, 0x90FFD0D0, d.blood)));
    }
}

// The drops' lenses, into the bend layer.
void BendRain() {
    const ULONGLONG now = GetTickCount64();
    for (const Drop& d : g_drops) {
        const float a = std::min(1.0f, (now - d.born) / 150.0f) * std::min(1.0f, (d.dries - now) / 1500.0f);
        const bool moving = d.runner && d.speed > 0.0f;
        const float sx = moving ? 0.85f : 1.0f, sy = moving ? 1.3f : 1.0f;
        ui::Image("bend_drop", d.x - d.r * sx, d.y - d.r * sy, 2 * d.r * sx, 2 * d.r * sy,
                  (static_cast<uint32_t>(235.0f * a) << 24) | 0xFFFFFF);
    }
}

// --- Cracked glass ----------------------------------------------------------
//
// Tapped ten times fast in one spot, the glass breaks there as a phone's does:
// a crushed spot where it was struck, cracks running out from it, wandering
// and forking, and broken rings joining them near the middle, the web
// thinning further out. Each crack line is dark on one side and catches the
// light on the other, as a fracture in glass does. Every break is its own.
// It mends by itself: held for most of a minute, then gone over the last ten
// seconds.

struct Crack {
    struct Line {
        float x0, y0, x1, y1, width;
    };
    // A piece of glass between two cracks and two rings, bending the light a
    // little differently from its neighbours: lighter or darker.
    struct Shard {
        float x[4], y[4];
        uint32_t argb;
        float leanX, leanY;  // which way the piece is tipped, for the bend layer
    };
    // Struck over the display, the panel under the glass breaks too: a spill
    // of black where its liquid crystal ran, and dead columns of one colour
    // top to bottom.
    struct Blot {
        float x, y, r;
    };
    struct Column {
        float x, width;
        uint32_t argb;
    };
    float x, y;  // where it was struck, in points
    ULONGLONG at;
    std::vector<Line> lines;
    std::vector<Shard> shards;
    std::vector<Blot> blots;
    std::vector<Column> columns;
};
std::vector<Crack> g_cracks;

struct Press {
    float x, y;
    ULONGLONG at;
};
std::vector<Press> g_presses;

// [Features] CrackSeconds: held for all but the last sixth, fading over that.
ULONGLONG CrackFade() { return static_cast<ULONGLONG>(config::Get().features.crackSeconds * 1000.0f / 6.0f); }
ULONGLONG CrackHold() { return static_cast<ULONGLONG>(config::Get().features.crackSeconds * 1000.0f) - CrackFade(); }
constexpr ULONGLONG kCrackWithin = 4000;  // the ten presses, in this long
constexpr float kCrackSpot = 14.0f;       // and this close together, in points

Crack MakeCrack(float cx, float cy) {
    std::mt19937 rng(static_cast<unsigned>(GetTickCount64()) ^ static_cast<unsigned>(cx * 131.0f) ^
                     static_cast<unsigned>(cy * 7919.0f));
    auto rnd = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    Crack c{cx, cy, GetTickCount64(), {}, {}};
    const GlassArea area = g_glassArea;
    auto inside = [&](float x, float y) { return x > area.left && y > area.top && x < area.right && y < area.bottom; };
    // The cracks running out, each as its points, in order round the impact.
    std::vector<std::vector<std::pair<float, float>>> rays;
    std::function<void(float, float, float, float, float, int)> grow = [&](float x, float y, float angle, float length,
                                                                          float width, int depth) {
        std::vector<std::pair<float, float>> points{{x, y}};
        float walked = 0.0f;
        while (walked < length) {
            // Short steps, each turned a little: glass breaks in straight
            // runs with small kinks.
            const float step = rnd(6.0f, 14.0f);
            angle += rnd(-0.12f, 0.12f);
            const float nx = x + std::sin(angle) * step, ny = y - std::cos(angle) * step;
            if (!inside(nx, ny)) break;
            c.lines.push_back({x, y, nx, ny, width * (1.0f - 0.5f * walked / length)});
            x = nx;
            y = ny;
            walked += step;
            points.push_back({x, y});
            // Now and then it forks, off at a sharper angle and shorter.
            if (depth < 2 && rnd(0.0f, 1.0f) < 0.10f) {
                const float side = rnd(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
                grow(x, y, angle + side * rnd(0.35f, 0.8f), (length - walked) * rnd(0.3f, 0.6f), width * 0.7f,
                     depth + 1);
            }
        }
        if (depth == 0) rays.push_back(points);
    };
    // Long and nearly straight, most of them to the edge of the glass.
    const int count = static_cast<int>(rnd(11.0f, 18.0f));
    const float first = rnd(0.0f, 6.2831853f);
    for (int i = 0; i < count; ++i) {
        grow(cx, cy, first + i * 6.2831853f / count + rnd(-0.15f, 0.15f), rnd(90.0f, 420.0f), rnd(1.1f, 1.7f), 0);
    }
    // The rings: at a few distances out, neighbouring cracks joined by a
    // jagged break round the impact - most near the middle, few far out.
    auto reach = [&](const std::vector<std::pair<float, float>>& ray, float r, float& ox, float& oy) {
        for (const auto& q : ray) {
            if (std::hypot(q.first - cx, q.second - cy) >= r) {
                ox = q.first;
                oy = q.second;
                return true;
            }
        }
        return false;
    };
    const float rings[5] = {rnd(6.0f, 10.0f), rnd(14.0f, 20.0f), rnd(26.0f, 36.0f), rnd(45.0f, 60.0f),
                            rnd(75.0f, 100.0f)};
    // Where each ring crossed between crack i and the next, for the shards.
    std::vector<std::array<float, 4>> joined(5 * rays.size(), std::array<float, 4>{0, 0, 0, 0});
    std::vector<bool> joins(5 * rays.size(), false);
    for (int k = 0; k < 5; ++k) {
        for (size_t i = 0; i < rays.size(); ++i) {
            if (rnd(0.0f, 1.0f) > 0.95f - k * 0.17f) continue;
            float ax, ay, bx, by;
            if (!reach(rays[i], rings[k], ax, ay) ||
                !reach(rays[(i + 1) % rays.size()], rings[k] * rnd(0.92f, 1.08f), bx, by))
                continue;
            joins[k * rays.size() + i] = true;
            joined[k * rays.size() + i] = {ax, ay, bx, by};
            float px = ax, py = ay;
            for (int piece = 1; piece <= 3; ++piece) {
                const float t = piece / 3.0f;
                float qx = ax + (bx - ax) * t, qy = ay + (by - ay) * t;
                if (piece < 3) {
                    // Bowed outward a little, and not quite straight.
                    const float ox = qx - cx, oy = qy - cy, l = std::max(0.01f, std::hypot(ox, oy));
                    const float bow = rnd(0.0f, 3.0f);
                    qx += ox / l * bow + rnd(-1.2f, 1.2f);
                    qy += oy / l * bow + rnd(-1.2f, 1.2f);
                }
                c.lines.push_back({px, py, qx, qy, 0.95f - k * 0.12f});
                px = qx;
                py = qy;
            }
        }
    }
    // The shards: a piece closed by two rings between the same two cracks,
    // near the middle, now lighter, now darker.
    for (int k = 0; k < 3; ++k) {
        for (size_t i = 0; i < rays.size(); ++i) {
            const size_t in = k * rays.size() + i, out = (k + 1) * rays.size() + i;
            if (!joins[in] || !joins[out] || rnd(0.0f, 1.0f) > 0.6f) continue;
            const auto& a = joined[in];
            const auto& b = joined[out];
            const bool light = rnd(0.0f, 1.0f) < 0.5f;
            const uint32_t alpha = static_cast<uint32_t>(rnd(18.0f, light ? 40.0f : 36.0f));
            const float tip = rnd(0.0f, 6.2832f), how = rnd(0.12f, 0.4f) * (1.0f - k * 0.25f);
            c.shards.push_back({{a[0], a[2], b[2], b[0]}, {a[1], a[3], b[3], b[1]},
                                (alpha << 24) | (light ? 0xFFFFFFu : 0x000000u), std::cos(tip) * how,
                                std::sin(tip) * how});
        }
    }
    // The crushed spot: short splinters every way.
    for (int i = 0; i < 40; ++i) {
        const float a = rnd(0.0f, 6.2831853f), r0 = rnd(0.0f, 5.0f), r1 = r0 + rnd(1.5f, 5.0f);
        c.lines.push_back({cx + std::sin(a) * r0, cy - std::cos(a) * r0, cx + std::sin(a) * r1, cy - std::cos(a) * r1,
                           rnd(0.5f, 1.0f)});
    }
    // Over the display: the panel beneath broken too.
    if (cx > 0.0f && cy > 0.0f && cx < ui::kScreenW && cy < ui::kScreenH) {
        const int blots = static_cast<int>(rnd(5.0f, 9.0f));
        for (int i = 0; i < blots; ++i) {
            const float a = rnd(0.0f, 6.2832f), d = rnd(0.0f, 16.0f);
            c.blots.push_back({cx + std::cos(a) * d, cy + std::sin(a) * d, rnd(5.0f, 16.0f)});
        }
        static const uint32_t kDead[] = {0xD8FF30FF, 0xD830FF60, 0xD82850FF, 0xE8FFFFFF, 0xE0101010};
        const int columns = static_cast<int>(rnd(1.0f, 3.99f));
        for (int i = 0; i < columns; ++i) {
            c.columns.push_back({std::clamp(cx + rnd(-40.0f, 40.0f), 2.0f, ui::kScreenW - 2.0f), rnd(1.0f, 2.6f),
                                 kDead[static_cast<int>(rnd(0.0f, 4.99f))]});
        }
    }
    return c;
}

// A blow with the phone can break its glass: about one in three does, and
// the third since the last break always does. Somewhere on the front.
void CrackFromHit() {
    if (!config::Get().features.cracks) return;
    static std::mt19937 rng(GetTickCount());
    static int since = 0;
    if (++since < 3 && std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) > 0.33f) return;
    since = 0;
    const GlassArea& area = g_glassArea;
    const float x = std::uniform_real_distribution<float>(area.left + 20.0f, area.right - 20.0f)(rng);
    const float y = std::uniform_real_distribution<float>(area.top + 20.0f, area.bottom - 20.0f)(rng);
    if (g_cracks.size() >= 6) g_cracks.erase(g_cracks.begin());
    g_cracks.push_back(MakeCrack(x, y));
    logfile::Line("phone: the glass cracked from a blow at %.0f, %.0f", x, y);
}

// A press on the screen, in points. Ten close together and quick break the
// glass there.
void NotePress(float x, float y) {
    if (!config::Get().features.cracks) return;
    const int kCrackPresses = config::Get().features.crackTaps;
    const ULONGLONG now = GetTickCount64();
    g_presses.push_back({x, y, now});
    g_presses.erase(std::remove_if(g_presses.begin(), g_presses.end(),
                                   [&](const Press& p) { return now - p.at > kCrackWithin; }),
                    g_presses.end());
    if (static_cast<int>(g_presses.size()) < kCrackPresses) return;
    float mx = 0.0f, my = 0.0f;
    for (const Press& p : g_presses) {
        mx += p.x;
        my += p.y;
    }
    mx /= g_presses.size();
    my /= g_presses.size();
    for (const Press& p : g_presses) {
        if (std::hypot(p.x - mx, p.y - my) > kCrackSpot) return;
    }
    g_presses.clear();
    if (g_cracks.size() >= 6) g_cracks.erase(g_cracks.begin());
    g_cracks.push_back(MakeCrack(mx, my));
    logfile::Line("phone: the glass cracked at %.0f, %.0f", mx, my);
}

// --- Blood ------------------------------------------------------------------
//
// Hit someone with the phone and their blood is on it: a splash across the
// glass where it struck, spattered drops round it and a run or two down, in
// the game's own blood (models\particle.txd's bloodpool_64). It stays a
// minute - three quarters of it as it landed, then fading away.

// [Features] BloodSeconds: three quarters held, the last quarter fading.
ULONGLONG BloodHold() { return static_cast<ULONGLONG>(config::Get().features.bloodSeconds * 750.0f); }
ULONGLONG BloodFade() { return static_cast<ULONGLONG>(config::Get().features.bloodSeconds * 250.0f); }
constexpr uintptr_t kPedPool = 0xB74490;  // CPools::ms_pPedPool
constexpr size_t kPedPoolStride = 0x7C4;  // sizeof(CCopPed), the pool's slot
constexpr size_t kPedLastWeapon = 0x760, kPedLastDamager = 0x764;

struct Blood {
    float x, y, size, turn;
    ULONGLONG at;
    float wash = 0.0f;  // how much rain has thinned and washed it off, 0 to 1
};
std::vector<Blood> g_blood;

void Splatter(float strength) {
    if (!config::Get().features.blood) return;
    static std::mt19937 rng(GetTickCount());
    auto r = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    Blood b{};
    // Anywhere on the front: the screen, and the glass above and below it.
    b.x = r(20.0f, ui::kScreenW - 20.0f);
    b.y = r(-40.0f, ui::kScreenH + 50.0f);
    b.size = r(70.0f, 120.0f) * std::clamp(strength, 0.7f, 1.4f);
    b.turn = r(0.0f, 360.0f);
    b.at = GetTickCount64();
    // Just the splash: droplets round it and runs down from it looked
    // scattered and wrong on the small screen.
    if (g_blood.size() >= 10) g_blood.erase(g_blood.begin());
    g_blood.push_back(b);
    logfile::Line("phone: blood on the phone");
}

// Each frame: anyone just hurt by CJ while the phone is the weapon in his
// hand, close enough to have been hit with it. What weapon the game records
// for the blow is not relied on (a loaded weapon can be booked under the
// melee type it is built on), only who struck it and that the phone was in
// his hand.
void BloodFrame() {
    const int type = PhoneWeaponType();
    const uintptr_t player = PlayerPed();
    const uintptr_t pool = *reinterpret_cast<const uintptr_t*>(kPedPool);
    static std::vector<std::pair<uintptr_t, float>> seen;
    if (type <= 0 || !player || !pool) return;
    const uintptr_t objects = *reinterpret_cast<const uintptr_t*>(pool);
    const auto* flags = *reinterpret_cast<const uint8_t* const*>(pool + 4);
    const int size = *reinterpret_cast<const int*>(pool + 8);
    if (!objects || !flags || size <= 0 || size > 100000) return;
    // CJ is in the pool himself: where he sits says whether its slots are
    // the size this reads them at.
    static int checked = 0;
    if (!checked) {
        const bool fits = player >= objects && (player - objects) % kPedPoolStride == 0 &&
                          (player - objects) / kPedPoolStride < static_cast<uintptr_t>(size);
        checked = fits ? 1 : -1;
        logfile::Line("phone: blood - %d people in the game's list, CJ %s in it where expected", size,
                      fits ? "is" : "is NOT");
    }
    if (checked < 0) return;
    if (static_cast<int>(seen.size()) != size) seen.assign(size, {0, 0.0f});
    const bool wielding = CurrentSlot(player) == kPhoneWeaponSlot &&
                          *reinterpret_cast<const int*>(player + kPedWeapons + kPhoneWeaponSlot * kWeaponSize) == type;
    const float* me = reinterpret_cast<const float*>(*reinterpret_cast<const uintptr_t*>(player + 0x14) + 0x30);
    for (int i = 0; i < size; ++i) {
        if (flags[i] & 0x80) {
            seen[i] = {0, 0.0f};
            continue;
        }
        const uintptr_t ped = objects + i * kPedPoolStride;
        const float health = *reinterpret_cast<const float*>(ped + kPedHealth);
        if (ped != player && seen[i].first == ped && health < seen[i].second - 0.5f &&
            *reinterpret_cast<const uintptr_t*>(ped + kPedLastDamager) == player) {
            const uintptr_t matrix = *reinterpret_cast<const uintptr_t*>(ped + 0x14);
            const float* at = matrix ? reinterpret_cast<const float*>(matrix + 0x30) : nullptr;
            const float reach = at && *reinterpret_cast<const uintptr_t*>(player + 0x14)
                                    ? std::hypot(at[0] - me[0], at[1] - me[1]) : 99.0f;
            static int told = 0;
            if (told < 5) {
                ++told;
                logfile::Line("phone: blood - someone hurt by CJ (weapon %d, %.1f m away, phone %s)",
                              *reinterpret_cast<const int*>(ped + kPedLastWeapon), reach,
                              wielding ? "in hand" : "not in hand");
            }
            if (wielding && reach < 3.5f) {
                Splatter((seen[i].second - health) / 10.0f);
                CrackFromHit();
            }
        }
        seen[i] = {ped, health};
    }
}

void DrawBlood() {
    const ULONGLONG now = GetTickCount64();
    g_blood.erase(std::remove_if(g_blood.begin(), g_blood.end(),
                                 [&](const Blood& b) {
                                     return now - b.at > BloodHold() + BloodFade() || b.wash >= 1.0f;
                                 }),
                  g_blood.end());
    if (g_blood.empty()) return;
    static int particles = -2;
    if (particles == -2) particles = sprite::LoadDictionary("valkyrie_phone_particle", (g_gameDir + "models\\particle.txd").c_str());
    const uintptr_t pool = particles >= 0 ? sprite::Find(particles, "bloodpool_64") : 0;
    for (const Blood& b : g_blood) {
        const ULONGLONG age = now - b.at;
        const float a = age < BloodHold() ? 1.0f
                                          : std::max(0.0f, 1.0f - static_cast<float>(age - BloodHold()) /
                                                                    std::max<ULONGLONG>(1, BloodFade()));
        // Drying darker as it ages; rain thins it out, paler and more
        // see-through, before it is gone.
        const float dry = std::min(1.0f, age / 30000.0f) * (1.0f - b.wash);
        const uint32_t red = static_cast<uint32_t>(150 - 60 * dry + 60 * b.wash);
        const uint32_t pale = static_cast<uint32_t>(4 + 70 * b.wash);
        const float thin = 1.0f - 0.75f * b.wash;
        auto colour = [&](float alpha) {
            return (static_cast<uint32_t>(alpha * a * thin) << 24) | (red << 16) | (pale << 8) | pale;
        };
        // It lands in a blink.
        const float grow = std::min(1.0f, age / 70.0f);
        const float sz = b.size * grow;
        if (pool) {
            const float c = std::cos(b.turn * 0.0174533f), sn = std::sin(b.turn * 0.0174533f), h = sz * 0.5f;
            auto P = [&](float dx, float dy) {
                return sprite::Corner{ui::ToPixelX(b.x + dx * c - dy * sn), ui::ToPixelY(b.y + dx * sn + dy * c)};
            };
            sprite::Quad(pool, P(-h, h), P(h, h), P(-h, -h), P(h, -h), colour(235.0f));
        } else {
            ui::Image("disc", b.x - sz * 0.3f, b.y - sz * 0.3f, sz * 0.6f, sz * 0.6f, colour(220.0f));
        }
    }
}

void DrawCracks() {
    const ULONGLONG now = GetTickCount64();
    g_cracks.erase(std::remove_if(g_cracks.begin(), g_cracks.end(),
                                  [&](const Crack& c) { return now - c.at > CrackHold() + CrackFade(); }),
                   g_cracks.end());
    const auto& features = config::Get().features;
    const float wet = features.rain ? std::min(1.0f, RainOnPhone() * features.rainAmount) : 0.0f;
    for (const Crack& c : g_cracks) {
        const ULONGLONG age = now - c.at;
        const ULONGLONG hold = CrackHold(), fade = std::max<ULONGLONG>(1, CrackFade());
        const float a = age < hold ? 1.0f : std::max(0.0f, 1.0f - static_cast<float>(age - hold) / fade);
        // It runs out from the impact in a blink.
        const float reach = std::min(1.0f, age / 90.0f) * 400.0f;
        auto faded = [&](uint32_t argb) {
            return (static_cast<uint32_t>((argb >> 24) * a) << 24) | (argb & 0xFFFFFF);
        };
        // The panel under it: the dead columns, then the spill of black with
        // its oily purple edge, spreading out over the first second.
        const float spill = std::min(1.0f, age / 900.0f);
        for (const Crack::Column& col : c.columns) {
            ui::Fill(col.x - col.width / 2, 0.0f, col.width, ui::kScreenH, faded(col.argb));
        }
        for (const Crack::Blot& bl : c.blots) {
            const float r = bl.r * spill;
            ui::Image("disc", bl.x - r * 1.25f, bl.y - r * 1.25f, r * 2.5f, r * 2.5f, faded(0x60402070));
            ui::Image("disc", bl.x - r, bl.y - r, r * 2.0f, r * 2.0f, faded(0xF0050508));
        }
        for (const Crack::Shard& sh : c.shards) {
            if (std::hypot(sh.x[0] - c.x, sh.y[0] - c.y) > reach) continue;
            auto P = [](float x, float y) { return sprite::Corner{ui::ToPixelX(x), ui::ToPixelY(y)}; };
            sprite::Quad(ui::Tex("white"), P(sh.x[0], sh.y[0]), P(sh.x[1], sh.y[1]), P(sh.x[3], sh.y[3]),
                         P(sh.x[2], sh.y[2]), faded(sh.argb));
        }
        // The crushed glass is frosted a little where it was struck.
        ui::Image("disc", c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f, faded(0x50FFFFFF));
        for (const Crack::Line& l : c.lines) {
            if (std::hypot(l.x0 - c.x, l.y0 - c.y) > reach) continue;
            const float dx = l.x1 - l.x0, dy = l.y1 - l.y0, len = std::hypot(dx, dy);
            if (len < 0.01f) continue;
            const float angle = std::atan2(dx, -dy);
            // The shadowed side of the break, then the edge catching the light.
            ui::Needle(l.x0 + 0.45f, l.y0 + 0.55f, angle, len, 0.0f, l.width + 0.6f, faded(0x78000000));
            ui::Needle(l.x0, l.y0, angle, len, 0.0f, l.width * 0.55f, faded(0xD0FFFFFF));
            // Blood near it runs into the break and stains it dark red.
            float stain = 0.0f;
            const float mx = (l.x0 + l.x1) / 2, my = (l.y0 + l.y1) / 2;
            for (const Blood& b : g_blood) {
                const float d = std::hypot(mx - b.x, my - b.y);
                const float spread = b.size * (0.75f + 0.5f * std::min(1.0f, (now - b.at) / 8000.0f));
                if (d < spread) stain = std::max(stain, (1.0f - d / spread) * (1.0f - b.wash));
            }
            if (stain > 0.02f) {
                ui::Needle(l.x0, l.y0, angle, len, 0.0f, l.width + 0.9f,
                           faded((static_cast<uint32_t>(220.0f * std::min(1.0f, stain * 1.6f)) << 24) | 0x700606));
            }
            // In the rain, water stands in the break and glints along it.
            if (wet > 0.0f) {
                ui::Needle(l.x0 - 0.3f, l.y0 - 0.3f, angle, len, 0.0f, l.width * 0.4f,
                           faded((static_cast<uint32_t>(110.0f * wet) << 24) | 0xDDEEFF));
            }
        }
    }
}

// The cracks into the bend layer: each shard tipped its own way, each line
// a fold across it, the crushed spot all ways at once.
void BendCracks() {
    const ULONGLONG now = GetTickCount64();
    auto lean = [](float x, float y, float alpha) {
        const uint32_t r = static_cast<uint32_t>(std::clamp(128.0f + x * 127.0f, 0.0f, 255.0f));
        const uint32_t gr = static_cast<uint32_t>(std::clamp(128.0f + y * 127.0f, 0.0f, 255.0f));
        return (static_cast<uint32_t>(std::clamp(alpha, 0.0f, 255.0f)) << 24) | (r << 16) | (gr << 8) | 0x80;
    };
    for (const Crack& c : g_cracks) {
        const ULONGLONG age = now - c.at;
        const ULONGLONG hold = CrackHold(), fade = std::max<ULONGLONG>(1, CrackFade());
        const float a = age < hold ? 1.0f : std::max(0.0f, 1.0f - static_cast<float>(age - hold) / fade);
        const float reach = std::min(1.0f, age / 90.0f) * 400.0f;
        for (const Crack::Shard& sh : c.shards) {
            if (std::hypot(sh.x[0] - c.x, sh.y[0] - c.y) > reach) continue;
            auto P = [](float x, float y) { return sprite::Corner{ui::ToPixelX(x), ui::ToPixelY(y)}; };
            sprite::Quad(ui::Tex("white"), P(sh.x[0], sh.y[0]), P(sh.x[1], sh.y[1]), P(sh.x[3], sh.y[3]),
                         P(sh.x[2], sh.y[2]), lean(sh.leanX, sh.leanY, 230.0f * a));
        }
        for (const Crack::Line& l : c.lines) {
            if (std::hypot(l.x0 - c.x, l.y0 - c.y) > reach) continue;
            const float dx = l.x1 - l.x0, dy = l.y1 - l.y0, len = std::hypot(dx, dy);
            if (len < 0.01f) continue;
            // Across the break, the two sides lean apart.
            ui::Needle(l.x0, l.y0, std::atan2(dx, -dy), len, 0.0f, l.width * 2.2f,
                       lean(-dy / len * 0.7f, dx / len * 0.7f, 220.0f * a));
        }
        for (int i = 0; i < 6; ++i) {
            const float r = 3.0f + i * 1.5f;
            ui::Image("bend_drop", c.x - r, c.y - r, r * 2, r * 2, (static_cast<uint32_t>(150.0f * a) << 24) | 0xFFFFFF);
        }
    }
}

// Blood is thick: its splash is a low, broad lens.
void BendBlood() {
    const ULONGLONG now = GetTickCount64();
    for (const Blood& b : g_blood) {
        const float grow = std::min(1.0f, (now - b.at) / 70.0f), sz = b.size * grow * 0.75f;
        ui::Image("bend_drop", b.x - sz / 2, b.y - sz / 2, sz, sz,
                  (static_cast<uint32_t>(90.0f * (1.0f - b.wash)) << 24) | 0xFFFFFF);
    }
}

// What the rain, the blood and the cracks do to one another, a frame on.
// Rain thins blood and washes it off; a runner through blood carries it
// down, red; a drop landing in blood takes its colour; a runner meeting a
// crack catches on it, and follows the break down a way before it lets go.
void GlassInteractions(float dt) {
    static std::mt19937 rng(GetTickCount() ^ 0x5EED);
    auto rnd = [](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    const ULONGLONG now = GetTickCount64();
    const auto& features = config::Get().features;
    const float rain = features.rain ? std::min(1.0f, RainOnPhone() * features.rainAmount) : 0.0f;
    for (Blood& b : g_blood) b.wash = std::min(1.0f, b.wash + rain * dt / 45.0f);
    for (Drop& d : g_drops) {
        for (Blood& b : g_blood) {
            const float grow = std::min(1.0f, (now - b.at) / 70.0f);
            const float dist = std::hypot(d.x - b.x, d.y - b.y);
            if (dist > b.size * 0.45f * grow) continue;
            const float thick = 1.0f - b.wash;
            if (!d.landed) d.blood = std::max(d.blood, 0.45f * thick);
            if (d.runner && d.speed > 0.0f) {
                d.blood = std::min(1.0f, d.blood + dt * 3.0f * thick);
                b.wash = std::min(1.0f, b.wash + dt * 0.12f);
            }
        }
        d.landed = true;
        if (!d.runner || d.speed <= 0.0f || d.followLeft > 0.0f || now < d.pauseUntil) continue;
        // Did it cross a crack's line this frame?
        for (const Crack& c : g_cracks) {
            bool caught = false;
            for (const Crack::Line& l : c.lines) {
                if (l.width < 0.8f) continue;
                const float ax = d.px, ay = d.py, bx = d.x, by = d.y;
                const float cx = l.x0, cy = l.y0, ex = l.x1, ey = l.y1;
                const float den = (bx - ax) * (ey - cy) - (by - ay) * (ex - cx);
                if (std::fabs(den) < 1e-5f) continue;
                const float t = ((cx - ax) * (ey - cy) - (cy - ay) * (ex - cx)) / den;
                const float u = ((cx - ax) * (by - ay) - (cy - ay) * (bx - ax)) / den;
                if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f) continue;
                float fx = ex - cx, fy = ey - cy;
                if (fy < 0.0f) {
                    fx = -fx;
                    fy = -fy;
                }
                const float fl = std::max(0.01f, std::hypot(fx, fy));
                d.followX = fx / fl;
                d.followY = fy / fl;
                d.followLeft = rnd(6.0f, 22.0f);
                d.pauseUntil = now + static_cast<ULONGLONG>(rnd(250.0f, 1200.0f));
                d.speed *= 0.2f;
                caught = true;
                break;
            }
            if (caught) break;
        }
    }
}

void RequestOpen() {
    // The mouse is the phone's from now: the click that chose it in the
    // other mod's menu is still held as that menu closes, and must not reach
    // the game as a shot.
    g_openAskedAt = GetTickCount64();
    if (input::Install()) input::Capture(true);
    InterlockedExchange(&g_openAsked, 1);
    logfile::Line("phone: asked to open by another mod");
}

void Draw() {
    // The frames a picture is taken on are the picture: nothing of the
    // phone's is drawn over them, only the frame from before, over the
    // screen, once the game has its picture.
    if (g.camera.on && g.camera.shot >= 2) {
        if (g_frozen) {
            const game::Point screen = game::ScreenSize();
            DrawDeviceTexture(g_freeze, 0.0f, 0.0f, screen.x, screen.y);
        }
        return;
    }
    if (g.camera.shot == 0) g_frozen = false;
    // Not over the pause menu. The game does not process frames while it is
    // paused, so the frame hook cannot lower the phone; it is let go here.
    if (game::MenuIsOpen() || !PlayerAble()) {
        if (g.focused) Lower();
        return;
    }
    if (false && g.out && (g.camera.on || config::Get().reflections)) CaptureCameraPreview();
    else g_cameraPreviewReady = false;
    g.drawnFrame = g.frame;
    g.drawnSinceUp = true;
    // The phone in CJ's left hand, into the game's own picture before
    // anything of the phone's is drawn over it.
    if (g.holding && !g.phoneAtEar && g.loaded) {
        phone_model::RenderInLeftHand(PlayerPed(), config::Get().handTurn, config::Get().handOffset,
                                      config::Get().handFlip);
    }
    const ULONGLONG now = GetTickCount64();
    const float dt = g.lastTick ? std::min(0.1f, (now - g.lastTick) / 1000.0f) : 0.0f;
    g.lastTick = now;
    g.dt = dt;

    const float target = !g.out ? 0.0f : g.focused ? 1.0f : 0.42f;
    // On a spring, a touch under critically damped: it comes up quickly,
    // runs a hair past where it rests and settles back, as a hand lifting
    // it would. Going away it never dips below the bottom.
    {
        // In small steps, so a slow frame cannot make it swing wider.
        const float k = 190.0f, damping = 2.0f * std::sqrt(k) * 0.8f;
        const int steps = std::max(1, static_cast<int>(std::ceil(dt * 240.0f)));
        const float h = dt / steps;
        for (int i = 0; i < steps; ++i) {
            g.slideVel += (k * (target - g.slide) - damping * g.slideVel) * h;
            g.slide += g.slideVel * h;
        }
        if (std::fabs(target - g.slide) < 0.003f && std::fabs(g.slideVel) < 0.05f) {
            g.slide = target;
            g.slideVel = 0.0f;
        }
        if (g.slide < 0.0f) {
            g.slide = 0.0f;
            g.slideVel = 0.0f;
        }
    }
    if (g.slide <= 0.0f || g.dictionary < 0) return;
    // A machine keeps the phone raised: the mouse stays with the game.
    if (g.game && !g.focused) Raise();

    const game::Point screen = game::ScreenSize();
    // The body texture is drawn w wide and 2w tall; the handset fills the top
    // kBodyBottom of it.
    const float bodyH = screen.y * g_config.height;
    const float w = bodyH / (phone_art::kBodyAspect * phone_art::kBodyBottom);
    const float margin = screen.y * 0.03f;
    const float xRest = g_config.right ? screen.x - w - margin * 1.5f : margin * 1.5f;
    const float restY = screen.y - bodyH - margin;
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
    const bool model3d = Phone3dReady(device);
    // The flat handset slides up from the bottom; the 3D one keeps its place
    // here and comes in from the corner by its pose (below).
    const float y = model3d ? restY : restY + (1.0f - g.slide) * (bodyH + margin * 2.0f);
    // Buzzing: the handset shakes side to side.
    float x = xRest;
    const float shake = now < g.shakeUntil ? std::sin(now * 0.11f) * w * 0.012f : 0.0f;
    if (!model3d) x += shake;
    const float texH = w * phone_art::kBodyAspect;

    // Lit by the light CJ stands in, all of it the game's own: the sun where
    // the time cycle puts it and as high as it stands, and the ambient the
    // game lights objects with, so it is bright at noon, dim at night and
    // takes the colour of a room. The screen gives its own light.
    auto* sunTable = reinterpret_cast<const float*>(0xB7CA50);  // CTimeCycle::m_VectorToSun[16]
    const uint32_t sunSlot = *reinterpret_cast<const uint32_t*>(0xB79FD0) & 15;  // m_CurrentStoredValue
    const float sunX = sunTable[sunSlot * 3], sunY = sunTable[sunSlot * 3 + 1], sunZ = sunTable[sunSlot * 3 + 2];
    const float sunUp = std::clamp(sunZ * 2.0f, 0.0f, 1.0f);
    auto ambientOf = [](uintptr_t f) { return reinterpret_cast<float(__cdecl*)()>(f)(); };
    const float ambR = ambientOf(0x560360), ambG = ambientOf(0x560370), ambB = ambientOf(0x560380);  // GetAmbient*_Obj
    {
        static ULONGLONG logged = 0;
        if (now - logged > 30000) {
            logged = now;
            logfile::Line("phone: light - ambient %.3f %.3f %.3f, sun height %.2f", ambR, ambG, ambB, sunZ);
        }
        const float peak = std::max({ambR, ambG, ambB, 0.01f});
        const float level = std::clamp(0.22f + peak * 0.9f + sunUp * 0.5f, 0.22f, 1.0f);
        const float k = std::min(1.0f, dt * 3.0f);
        auto toward = [&](float& v, float c) { v += (std::clamp(level * (0.7f + 0.3f * c / peak), 0.15f, 1.0f) - v) * k; };
        toward(g.lightR, ambR);
        toward(g.lightG, ambG);
        toward(g.lightB, ambB);
    }
    // The light as the phone sees it. It faces the camera: x across the
    // screen (the camera's own "right" points to the left of the picture),
    // y up it, z out of the glass toward the viewer.
    std::optional<Lighting> lighting;
    {
        const float* m = reinterpret_cast<const float*>(0xB6F028 + 0x974);  // TheCamera's matrix
        const float rx = m[0], ry = m[1], rz = m[2], fx = m[4], fy = m[5], fz = m[6], ux = m[8], uy = m[9], uz = m[10];
        Lighting l{};
        l.light[0] = -(sunX * rx + sunY * ry + sunZ * rz);
        l.light[1] = sunX * ux + sunY * uy + sunZ * uz;
        l.light[2] = -(sunX * fx + sunY * fy + sunZ * fz);
        l.sun[0] = 1.0f;
        l.sun[1] = 0.95f;
        l.sun[2] = 0.86f;
        l.sun[3] = sunUp;
        l.ambient[0] = std::clamp(ambR, 0.03f, 1.0f);
        l.ambient[1] = std::clamp(ambG, 0.03f, 1.0f);
        l.ambient[2] = std::clamp(ambB, 0.03f, 1.0f);
        l.env[0] = std::atan2(-fx, fy) / 6.2831853f;
        l.env[1] = -std::asin(std::clamp(fz, -1.0f, 1.0f)) * 0.3f;
        l.env[2] = 1.0f;
        lighting = l;
    }
    // The 3D phone's screen is drawn into a texture of its own and laid on
    // its face; the flat handset is drawn first and the screen straight onto
    // the picture over it.
    const float sw = w * (phone_art::kScreenRight - phone_art::kScreenLeft);
    bool inTexture = false;
    if (model3d) {
        sprite::Flush();
        inTexture = phone3d::BeginScreen(device, static_cast<int>(std::lround(sw)),
                                         static_cast<int>(std::lround(sw * ui::kScreenH / ui::kScreenW)));
    }
    // A soft shadow under it, so it sits on the picture rather than floats.
    if (!inTexture) {
        const float bodyBottom = y + texH * phone_art::kBodyBottom;
        const float sx = w * 0.06f, sy = w * 0.05f;
        sprite::Draw(ui::Tex("glow"), x - sx + w * 0.03f, y + sy, x + w + sx + w * 0.03f, bodyBottom + sy * 2.0f,
                     0x58000000);
    }
    if (!inTexture && !LightHandset(false, x, y, w, texH, lighting.value())) {
        lighting.reset();
        const uint32_t lit = 0xFF000000 | (static_cast<uint32_t>(g.lightR * 255.0f) << 16) |
                             (static_cast<uint32_t>(g.lightG * 255.0f) << 8) | static_cast<uint32_t>(g.lightB * 255.0f);
        sprite::Draw(ui::Tex(BodyArt("body")), x, y, x + w, y + texH, lit);
    }

    const float sl = x + w * phone_art::kScreenLeft;
    const float st = y + texH * phone_art::kScreenTop;
    // Drawn into its texture, the screen's top left is the texture's; the
    // cursor is moved to match.
    const float originX = inTexture ? 0.0f : sl, originY = inTexture ? 0.0f : st;
    ui::SetScreen(originX, originY, sw / ui::kScreenW);
    g_screenLeft = originX;
    g_screenTop = originY;
    g_ppp = sw / ui::kScreenW;
    if (g.placeCursor && g.slide >= 1.0f) {
        g.placeCursor = false;
        input::SetCursor({sl + ui::kScreenW / 2 * g_ppp, st + ui::kScreenH * 0.6f * g_ppp});
    }

    const bool aiming = g.camera.on && g.screen == Screen::Camera;
    if (g.focused && aiming) {
        // The mouse aims the camera; nothing on the screen is tapped.
        CameraInput();
        ui::BeginTouch(-10000.0f, -10000.0f, false, false, false, 0);
        g.enter = g.tab = false;
    } else if (g.focused && Moving()) {
        // Nothing is touched while one screen slides over another.
        input::BeginFrame();
        TakeKeys();
        ui::CancelTouch();
    } else if (g.focused) {
        input::BeginFrame();
        TakeKeys();
        const input::Point c = input::Cursor();
        // On the 3D phone, the cursor is turned into a point on its screen
        // from where its corners last landed, however it is turned.
        float tx = c.x - (sl - originX), ty = c.y - (st - originY);
        float corners[6];
        if (inTexture && phone3d::ScreenCorners(corners)) {
            const float ax = corners[2] - corners[0], ay = corners[3] - corners[1];  // along the top
            const float bx = corners[4] - corners[0], by = corners[5] - corners[1];  // down the side
            const float det = ax * by - ay * bx;
            if (std::fabs(det) > 1.0f) {
                const float px = c.x - corners[0], py = c.y - corners[1];
                const float u = (px * by - py * bx) / det, v = (ax * py - ay * px) / det;
                tx = u * ui::kScreenW * g_ppp;
                ty = v * ui::kScreenH * g_ppp;
            }
        }
        ui::BeginTouch(tx, ty, input::LeftHeld(), input::LeftPressed(),
                       input::LeftReleased(),
                       input::Wheel());
        if (input::RightPressed() && !g.game) Lower();
    } else {
        ui::BeginTouch(-10000.0f, -10000.0f, false, false, false, 0);
        g.enter = g.tab = false;
    }
    g.field = {};

    // Auto-Lock. Anything done with the phone counts as using it; left alone
    // it dims ten seconds before the time is up, then locks as the sleep
    // button would (not in a call, the camera, a game or with an alarm up).
    if (g.focused && !aiming) {
        const input::Point c = input::Cursor();
        if (std::fabs(c.x - g.lastCursor.x) + std::fabs(c.y - g.lastCursor.y) > 2.0f || input::LeftHeld() ||
            input::Wheel() != 0) {
            g.activeAt = now;
        }
        g.lastCursor = c;
    }
    {
        const int minutes = AutoLockMinutes();
        const auto& set = phone_data::Get().settings;
        const bool exempt = !set.poweredOn || g.asleep || g.call.active || g.camera.on || g.game ||
                            g.alarmRinging || g.powerMenu || g.screen == Screen::Lock || g.screen == Screen::Off;
        float want = 0.0f;
        if (exempt || minutes == 0 || !g.activeAt) {
            g.activeAt = now;
        } else {
            const ULONGLONG limit = static_cast<ULONGLONG>(minutes) * 60000ull, idle = now - g.activeAt;
            if (idle + 10000 > limit) want = 1.0f;
            if (idle > limit && set.slideToUnlock) {
                logfile::Line("phone: auto-lock");
                Sleep();
                g.activeAt = now;
                want = 0.0f;
            } else if (idle > limit) {
                // Without the lock screen it only stays dim until used.
                want = 1.0f;
            }
        }
        g.dim += (want - g.dim) * std::min(1.0f, dt * (want > g.dim ? 2.5f : 12.0f));
        if (g.dim < 0.01f) g.dim = 0.0f;
    }

    DrawAnimated();
    // An app root has no back button; minus returns it to the menu.
    // During a transition leave the request for the settled screen.
    if (g.backPending && !Moving()) {
        g.backPending = false;
        if (g.screen != Screen::Lock && g.screen != Screen::Home) Go(Screen::Home);
    }
    // The page's textures are only held while the browser is open.
    if (g.screen != Screen::Internet || g.call.active || g.game) web::Release();
    if (g.screen != Screen::CameraRoll && g.screen != Screen::PhotoOpen) ReleaseRoll();
    ui::NoClip();
    ui::Flush();
    // The front glass: the whole face of the handset, in the screen's points.
    const float bodyLeft = x + w * phone_art::kBodyLeft;
    const float frontW = w * (phone_art::kBodyRight - phone_art::kBodyLeft), frontH = texH * phone_art::kBodyBottom;
    g_glassArea.left = (bodyLeft - sl) / g_ppp;
    g_glassArea.top = (y - st) / g_ppp;
    g_glassArea.right = g_glassArea.left + frontW / g_ppp;
    g_glassArea.bottom = g_glassArea.top + frontH / g_ppp;
    // A press anywhere on the glass counts toward breaking it.
    if (g.focused && !aiming && input::LeftPressed()) {
        const input::Point c = input::Cursor();
        const float px = (c.x - sl) / g_ppp, py = (c.y - st) / g_ppp;
        if (px > g_glassArea.left && py > g_glassArea.top && px < g_glassArea.right && py < g_glassArea.bottom)
            NotePress(px, py);
    }
    // The flat handset takes rain and cracks straight onto the picture; the
    // 3D one onto its glass layer, below.
    if (!inTexture) {
        RainUpdate(dt);
        GlassInteractions(dt);
        DrawRain();
        DrawCracks();
    }
    ui::Flush();
    if (inTexture) {
        sprite::Flush();
        phone3d::EndScreen(device);
        // Put back where the rest of the phone's drawing expects it.
        ui::SetScreen(sl, st, g_ppp);
        g_screenLeft = sl;
        g_screenTop = st;
        // Coming in from the corner on the same spring the flat one rises on:
        // pushed out past the corner, turned away and tipped, and swinging
        // round to face the viewer as it comes.
        const float away = 1.0f - g.slide;
        const float side = g_config.right ? 1.0f : -1.0f;
        phone3d::Pose pose{};
        pose.screen[0] = sl;
        pose.screen[1] = st;
        pose.screen[2] = sw;
        pose.screen[3] = sw * ui::kScreenH / ui::kScreenW;
        pose.offset[0] = side * away * w * 1.15f + shake;
        pose.offset[1] = away * bodyH * 1.1f;
        // Never quite square: a small tilt of its own each time it is taken out.
        pose.yaw = side * away * 0.9f + g.restTilt[0];
        pose.pitch = away * 0.35f + g.restTilt[1];
        pose.roll = -side * away * 0.45f + g.restTilt[2];
        pose.brightness = 1.0f;
        pose.lcd = config::Get().screenEffect;
        pose.wear = config::Get().features.scratches ? config::Get().features.scratchAmount : 0.0f;
        pose.wearStrong = config::Get().features.scratchStrong;
        const Lighting& l = lighting.value();
        phone3d::Light light{};
        for (int k = 0; k < 3; ++k) {
            light.dir[k] = l.light[k];
            light.ambient[k] = l.ambient[k];
            light.env[k] = l.env[k];
        }
        for (int k = 0; k < 4; ++k) light.sun[k] = l.sun[k];
        IDirect3DTexture9* live = LiveMirror(device);
        light.liveEnv = live != nullptr;
        viewfinder::ViewWindow(light.liveWindow);
        IDirect3DTexture9* env = live ? live : DeviceCopy("phone_env");
        const game::Point size = game::ScreenSize();
        // What lies on the glass, over the whole front, into its layer.
        pose.screenOnFront[0] = (phone_art::kScreenLeft - phone_art::kBodyLeft) /
                                (phone_art::kBodyRight - phone_art::kBodyLeft);
        pose.screenOnFront[1] = phone_art::kScreenTop / phone_art::kBodyBottom;
        pose.screenOnFront[2] = (phone_art::kScreenRight - phone_art::kBodyLeft) /
                                (phone_art::kBodyRight - phone_art::kBodyLeft);
        pose.screenOnFront[3] = phone_art::kScreenBottom / phone_art::kBodyBottom;
        if (phone3d::BeginGlass(device, static_cast<int>(std::lround(frontW)), static_cast<int>(std::lround(frontH)))) {
            ui::SetScreen(sl - bodyLeft, st - y, g_ppp);
            RainUpdate(dt);
            GlassInteractions(dt);
            DrawRain();
            DrawCracks();
            DrawBlood();
            ui::Flush();
            sprite::Flush();
            phone3d::EndGlass(device);
            pose.glass = true;
            // How it all bends the light, over the same front.
            if (phone3d::BeginBend(device, static_cast<int>(std::lround(frontW)), static_cast<int>(std::lround(frontH)))) {
                ui::SetScreen(sl - bodyLeft, st - y, g_ppp);
                BendRain();
                BendCracks();
                BendBlood();
                ui::Flush();
                sprite::Flush();
                phone3d::EndBend(device);
                pose.bend = true;
            }
            ui::SetScreen(sl, st, g_ppp);
        }
        if (g.slide > 0.97f) {
            const float bodyBottom = y + texH * phone_art::kBodyBottom;
            const float sx = w * 0.06f, sy = w * 0.05f;
            sprite::Draw(ui::Tex("glow"), x - sx + w * 0.03f, y + sy, x + w + sx + w * 0.03f, bodyBottom + sy * 2.0f,
                         0x58000000);
            sprite::Flush();
        }
        const auto& look = config::Get().look;
        pose.ink = look.ink;
        const float ink = config::Get().features.outline ? size.y / 1080.0f * look.inkWidth : 0.0f;
        if (!phone3d::Draw(device, pose, light, g_parts3d, env, ink, now / 1000.0f)) {
            // The flat handset this once, the screen laid over it.
            if (!LightHandset(false, x, y, w, texH, l)) sprite::Draw(ui::Tex(BodyArt("body")), x, y, x + w, y + texH);
            DrawDeviceTexture(phone3d::ScreenTexture(), sl, st, sl + sw, st + pose.screen[3]);
        }
        if (live) live->Release();
    }

    // Where a point of the flat handset's layout is on the 3D phone as it
    // was drawn this frame - turned, tipped and tilted - by the same mapping
    // the cursor goes through the other way (the screen's corners, carried
    // on past its edges). Without the 3D phone, where it is.
    float placedCorners[6];
    const bool placed3d = inTexture && phone3d::ScreenCorners(placedCorners);
    const float flatScreenH = sw * ui::kScreenH / ui::kScreenW;
    auto place = [&](float px, float py, float& ox, float& oy) {
        if (!placed3d) {
            ox = px;
            oy = py;
            return;
        }
        const float u = (px - sl) / sw, v = (py - st) / flatScreenH;
        ox = placedCorners[0] + u * (placedCorners[2] - placedCorners[0]) + v * (placedCorners[4] - placedCorners[0]);
        oy = placedCorners[1] + u * (placedCorners[3] - placedCorners[1]) + v * (placedCorners[5] - placedCorners[1]);
    };
    const float placedScale =
        placed3d ? std::hypot(placedCorners[2] - placedCorners[0], placedCorners[3] - placedCorners[1]) / sw : 1.0f;

    // The home button below the screen: out of a machine, else home.
    const float ppp = ui::PixelsPerPoint();
    const float homeX = (x + w * phone_art::kHomeX - sl) / ppp;
    const float homeY = (y + texH * phone_art::kHomeY - st) / ppp;
    const bool sm = KeypadSkin();
    // The keypad handset's pad stands where the home button is,
    // and larger.
    const float homeRadius = sm ? phone_art_sm::kPadRadius : phone_art::kHomeRadius;
    const float homeR = w * homeRadius / ppp;
    // Pressed, the button sinks a little - a shade darker and a hair smaller -
    // and clicks when it comes back up. It is round: the corners of the
    // square round it are not the button.
    const bool onHome = std::hypot(ui::CursorX() - homeX, ui::CursorY() - homeY) <= homeR;
    if (onHome && ui::Pressing(Rect{homeX - homeR, homeY - homeR, homeR * 2, homeR * 2})) {
        float cx, cy;
        place(x + w * phone_art::kHomeX, y + texH * phone_art::kHomeY, cx, cy);
        const float r = w * homeRadius * 0.92f * placedScale;
        sprite::Draw(ui::Tex("disc"), cx - r, cy - r, cx + r, cy + r, 0x50000000);
    }
    if (ui::Tapped(homeX - homeR, homeY - homeR, homeR * 2, homeR * 2) && onHome && !Dead()) {
        if (!phone_data::Get().settings.silent) sound::Click();
        if (g.asleep) {
            Wake();
        } else if (g.game) {
            g.game.reset();
        } else if (g.arranging) {
            EndArranging();
        } else if (phone_data::Get().settings.poweredOn && !g.call.active && g.screen != Screen::Lock &&
                   !g.alarmRinging) {
            g.pickingForCompose = false;
            Go(Screen::Home);
        }
    }

    // The keypad handset's other two keys, either side of the
    // pad: A selects, minus goes back.
    if (sm) {
        auto key = [&](float fx, float fy) {
            const float kx = (x + w * fx - sl) / ppp, ky = (y + texH * fy - st) / ppp;
            const float kr = w * phone_art_sm::kKeyRadius / ppp;
            const Rect r{kx - kr, ky - kr, kr * 2, kr * 2};
            const bool on = std::hypot(ui::CursorX() - kx, ui::CursorY() - ky) <= kr;
            if (on && ui::Pressing(r)) {
                float cx, cy;
                place(x + w * fx, y + texH * fy, cx, cy);
                const float pr = w * phone_art_sm::kKeyRadius * 0.92f * placedScale;
                sprite::Draw(ui::Tex("disc"), cx - pr, cy - pr, cx + pr, cy + pr, 0x50000000);
            }
            const bool tapped = on && ui::Tapped(r) && !Dead();
            if (tapped && !phone_data::Get().settings.silent) sound::Click();
            return tapped;
        };
        if (key(phone_art_sm::kAX, phone_art_sm::kAY)) KeyA();
        if (key(phone_art_sm::kMinusX, phone_art_sm::kMinusY)) KeyMinus();
    }

    // The side buttons. Each is given a little more room than it has, out
    // from the edge, so it can be hit.
    auto side = [&](const float* f, bool right) {
        const float pad = 10.0f;
        const float x0 = (x + w * f[0] - sl) / ppp - (right ? 0.0f : pad);
        const float x1 = (x + w * f[2] - sl) / ppp + (right ? pad : 0.0f);
        const float y0 = (y + texH * f[1] - st) / ppp, y1 = (y + texH * f[3] - st) / ppp;
        return Rect{x0, y0, x1 - x0, y1 - y0};
    };
    auto pushed = [&](const float* f) {
        // Pressed in: darker while held, over the button where it is drawn.
        float ax, ay, bx, by;
        place(x + w * f[0], y + texH * f[1], ax, ay);
        place(x + w * f[2], y + texH * f[3], bx, by);
        sprite::Draw(ui::Tex("white"), std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by),
                     0x70000000);
    };
    const Rect ring = side(phone_art::kRing, false), up = side(phone_art::kVolumeUp, false),
               down = side(phone_art::kVolumeDown, false), sleep = side(phone_art::kSleep, true);
    if (ui::Pressing(up)) pushed(phone_art::kVolumeUp);
    if (ui::Pressing(down)) pushed(phone_art::kVolumeDown);
    if (ui::Pressing(sleep)) pushed(phone_art::kSleep);
    if (!Dead()) {
        if (ui::Tapped(ring)) ToggleSilent();
        if (ui::Tapped(up)) ChangeVolume(1);
        if (ui::Tapped(down)) ChangeVolume(-1);
    }
    // Sleep: a press puts the screen to sleep or wakes it; held, it offers
    // to switch the phone off - or, off, switches it on.
    if (ui::Pressing(sleep) && !Booting()) {
        if (!g.sleepHeldSince) g.sleepHeldSince = now;
        if (!g.sleepHoldFired && now - g.sleepHeldSince > 900) {
            g.sleepHoldFired = true;
            if (!phone_data::Get().settings.poweredOn) {
                TogglePower();
                Wake();
            } else {
                Wake();
                g.powerMenu = true;
                g.powerKnob = 0.0f;
            }
        }
    } else {
        g.sleepHeldSince = 0;
    }
    if (ui::Tapped(sleep)) {
        if (g.sleepHoldFired || Dead()) {
            // The hold has been answered already; or the phone is off or
            // starting up, and only a hold switches it on.
        } else if (g.asleep) {
            Wake();
        } else {
            Sleep();
        }
        g.sleepHoldFired = false;
    } else if (!ui::Pressing(sleep) && !input::LeftHeld()) {
        g.sleepHoldFired = false;
    }

    // The glass over the screen catches the light too (the 3D phone's own
    // glass does that itself).
    if (lighting && !inTexture && config::Get().features.glass) LightHandset(true, x, y, w, texH, lighting.value());

    if (g.focused && !aiming) {
        // The pause menu's own cursor (fronten_pc.txd's "mouse"). Its arrow
        // runs to the texture's top and left edges and the texture wraps, so
        // drawn smoothed its far side takes in the near one as a stray line;
        // it is clamped while the phone draws it, and never drawn smaller
        // than its own 32 pixels, where it would drop pixels. The phone's
        // own arrow stands in if the menu's is not there.
        const input::Point c = input::Cursor();
        const uintptr_t menu = config::Get().features.menuCursor ? sprite::Find(g.frontend, "mouse") : 0;
        const uintptr_t cursor = menu ? menu : ui::Tex("cursor");
        const float cx = std::floor(c.x), cy = std::floor(c.y);
        const float cs = std::round(std::max(32.0f, 18.0f * game::UiScale()));
        uint32_t* sampling = menu ? reinterpret_cast<uint32_t*>(menu + 0x50) : nullptr;  // RwTexture::filterAddressing
        const uint32_t was = sampling ? *sampling : 0;
        if (sampling) *sampling = 0x3302;  // linear, clamped in u and v
        sprite::Draw(cursor, cx, cy, cx + cs, cy + cs);
        sprite::Flush();
        if (sampling) *sampling = was;
    }
    DrawFlash();
    // A picture just asked for: this frame, finished, is what is shown while
    // it is taken.
    if (g.camera.on && g.camera.shot == 1 && !g_frozen) {
        sprite::Flush();
        FreezeFrame();
    }
}

}  // namespace phone
