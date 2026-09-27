// valkyrie-phone.ini: everything about the phone a player may want to change.
//
// The file is written next to the game, filled in with the phone as it ships
// and a note on every line, the first time the phone starts. After that the
// phone only reads it; a section taken out of it falls back to what ships.
// A section a newer phone adds is appended to an older file, so nothing the
// player set is ever overwritten.
#pragma once

#include <cstdint>

#include <map>
#include <string>
#include <vector>

namespace config {

struct App {
    std::string id;     // Phone, Contacts, Text, ...
    std::string label;  // under the icon
    // Where the icon comes from: a texture in valkyrie-phone.txd, "hud:NAME"
    // for one in the game's models\hud.txd, or a .png file in the game folder.
    std::string icon;
};

struct Contact {
    std::string name, number;
    std::string picture;  // an icon, as [Icons] takes one; empty for none
};

// A sound the phone can make: a number is one of the game's own mission-audio
// sounds (the same numbers SA-MP's PlayerPlaySound takes; the phone's own
// Valkyrie tone is 20600); anything else is a .wav file in the game folder.
struct Tone {
    std::string name, sound;
};

// An animation CJ plays, and the .ifp it is in. `loop` plays it round; off,
// it holds its last frame.
struct Anim {
    std::string name, file;
    bool loop = true;
};

// A point on CJ, in metres from the middle of him: to his right, in front of
// him, and up. His eyes are about 0.65 up.
struct Lens {
    float right, forward, up;
};

struct Config {
    int key = 'P';
    float height = 0.78f;
    bool right = true;
    // The model CJ holds while this phone is in use (phone_model.h).
    std::string model = "valkyrie-phone-model.dff", modelTextures = "valkyrie-phone-model.txd";
    // How much the model in CJ's hand reflects, 0 matt to 1 mirror-bright.
    float shine = 1.0f;
    // The phone on screen mirrors the world around it (a second render of
    // the world while the phone is up, as a mirror in the game costs).
    bool reflections = true;
    // How the phone sits in CJ's left hand (phone_model::RenderInLeftHand).
    float handTurn[3] = {180.0f, 0.0f, 0.0f};
    float handOffset[3] = {0.04f, 0.05f, 0.0f};
    bool handFlip = true;  // turned half round so the screen faces CJ
    bool model3d = true;   // the phone on screen drawn as its 3D model, ringed in ink
    float screenEffect = 0.5f;  // how strongly the 3D phone's display shows its LCD pixels
    // [Phone] Skin: the iFruit as it ships, or Harry's phone from Silent Hill:
    // Shattered Memories - black, three keys ringed in teal, a menu of tiles.
    bool shattered = false;
    bool silentHill = false;  // [Phone] Profile=SilentHill: content and render integration

    // [Look]: the phone's colours, as 0xAARRGGBB.
    struct Look {
        uint32_t accent = 0xFFACCBF1, accentBar = 0x5AACCBF1, text = 0xFFFFFFFF, grey = 0xFFB4B4B4,
                 dimGrey = 0xFF8A8A8A, panel = 0xAA000000, panelDark = 0xDC000000, separator = 0x30FFFFFF,
                 band = 0xFF4F86C6, bandTop = 0xFF7AA9DE, green = 0xFF36682C, red = 0xFFB4191D, ink = 0xFF000000;
        float inkWidth = 1.0f;
    } look;
    // [Features]: what the phone does, each on or off.
    struct Features {
        bool rain = true, cracks = true, startup = true, outline = true, motion = true, vibrate = true,
             menuCursor = true, arrange = true, glass = true, subtitles = false, scratches = true,
             maps = true,  // the Maps app, and the map it draws from Valkyrie-radar-tiles
             blood = true,      // blood on the glass after a hit with the phone
             slowPages = true,  // web pages load at EDGE speed
             signal = true;     // the signal follows the map's masts
        float rainAmount = 1.0f;     // how hard rain gathers on the glass
        int crackTaps = 10;          // quick taps in one spot that break the glass
        float crackSeconds = 60.0f;  // how long a crack lasts
        float startupSeconds = 14.0f;
        float scratchAmount = 1.0f;  // how worn the glass is
        bool scratchStrong = true;   // [Features] ScratchLook: Strong or Subtle
        float bloodSeconds = 60.0f;  // how long blood stays on the glass
    } features;
    int calendarYear = 2007;  // [Calendar] the year the phone's calendar starts in
    // [Stocks]: BAWSAQ's companies and its trading hours.
    struct Company {
        std::string ticker, name;
        float base, swing;
    };
    std::vector<Company> companies;
    float marketOpen = 9.5f, marketClose = 16.0f;
    std::vector<int> radioStations;  // [Radio] the stations listed, by the game's numbers

    std::vector<App> apps;  // the home screen, in order
    std::vector<App> dock;  // the four along the bottom
    std::vector<Contact> contacts;
    std::vector<Tone> ringtones;
    std::vector<Tone> textTones;
    std::string ringback;  // heard while a call rings out
    std::string keyTone;   // a key on the keypad
    std::string sentTone;  // a text going out

    // The numbers that answer.
    std::string emergency = "911", nonEmergency = "311", hotline = "726", hotel = "666";
    std::string save = "100";  // saves the game where CJ stands
    // Opens valkyrie-trainer, when it is installed: *#TRAIN# on the keypad.
    std::string trainer = "*#87246#";
    // The restaurants that deliver: Well Stacked Pizza, Burger Shot and
    // Cluckin' Bell. Also in Contacts.
    std::string pizza = "5557492", burger = "5552874", chicken = "5552582";

    // How fast the mouse aims the camera, and whether a photo keeps the
    // viewfinder's upright shape or the whole screen.
    float cameraSensitivity = 1.0f;
    bool photoFlash = false;  // [Photos] Flash: white over the screen after a picture
    bool portraitPhotos = true;
    // The live picture on the phone's screen (viewfinder.h). Off, the
    // viewfinder stays dark; pictures are still taken.
    bool liveView = true;
    // What CJ does with the phone: looking at it while it is up, holding it
    // up for a picture, holding it out for a selfie.
    Anim useAnim{"betslp_loop", "otb", true};
    Anim cameraAnim{"picstnd_in", "camera", false};
    Anim selfieAnim{"ARRESTgun", "ped", false};
    // Where the lens is in the two camera poses.
    Lens cameraLens{0.05f, 0.60f, 0.65f};
    Lens selfieLens{0.20f, 1.10f, 0.66f};
    std::string homePage = "www.eyefind.info";
};

void Load(const std::string& gameDir);
const Config& Get();

// Every app the phone has, whether the file shows it or not.
const std::vector<std::string>& AllApps();

}  // namespace config
