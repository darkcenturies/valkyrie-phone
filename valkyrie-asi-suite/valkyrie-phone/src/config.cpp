#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "bundle.h"
#include "log.h"

namespace config {
namespace {

Config g_config;

const std::vector<std::string> kAllApps = {"Phone",    "Text",  "Contacts", "Camera",     "Photos", "Maps",
                                           "Weather",  "Stocks", "Internet", "Games",   "Radio",  "Clock",
                                           "Calendar", "Calculator", "Notes", "Flashlight", "Settings"};
// Apps added after the first release, and the app each goes after when an
// older file's home screen is given it (once - see Load).
const std::vector<std::pair<std::string, std::string>> kAddedApps = {
    {"Maps", "Photos"}, {"Weather", "Maps"}, {"Stocks", "Weather"}, {"Radio", "Games"}, {"Calendar", "Clock"},
    {"Flashlight", "Notes"}};

// The file as it ships, one section at a time so a missing one can be added
// to an older file on its own.
struct Section {
    const char* name;
    const char* text;
};

const Section kSections[] = {
    {"Phone",
     "; Valkyrie Phone. Everything here can be changed; the phone reads this file\r\n"
     "; each time the game starts. Take a section out and the phone uses what it\r\n"
     "; ships with. Lines starting with ; are notes.\r\n"
     "\r\n"
     "[Phone]\r\n"
     "; The key that takes the phone out and puts it away: a letter, or a\r\n"
     "; Windows virtual-key code such as 0x70 for F1.\r\n"
     "; None to have no key (the inventory's Phone still takes it out).\r\n"
     "Key=P\r\n"
     "; How tall the phone stands, as a share of the screen's height.\r\n"
     "Height=0.78\r\n"
     "; Which side of the screen it sits on: right or left.\r\n"
     "Side=right\r\n"
     "; 1: the phone on screen is its 3D model, coming in from the corner with the\r\n"
     "; Valkyrie ink round it. 0: the flat handset rising from the bottom.\r\n"
     "Model3D=1\r\n"
     "; How it looks: iFruit, or Keypad - Valkyrie's keypad handset:\r\n"
     "; keypad, black with three keys ringed in teal under a menu of\r\n"
     "; tiles. [Look]'s colours left as they ship take the skin's own.\r\n"
     "Skin=iFruit\r\n"
     "; How strongly its display shows its pixels' red, green and blue stripes, from\r\n"
     "; 0 (none) to 1.\r\n"
     "ScreenEffect=0.5\r\n"
     "; The model CJ holds while this phone is in use. valkyrie-phone-model.dff is\r\n"
     "; the phone's own, built into it; any other name is a model from the game\r\n"
     "; folder, such as an SA replacement for cellphone.dff, with its .txd. The\r\n"
     "; game's own phone stays for the game's own calls.\r\n"
     "Model=valkyrie-phone-model.dff\r\n"
     "ModelTextures=valkyrie-phone-model.txd\r\n"},
    {"Look",
     "\r\n[Look]\r\n"
     "; The phone's colours, as AARRGGBB (hex, alpha first): 00 clear to FF solid.\r\n"
     "; Accent: titles, links and the Settings values. AccentBar: a row being\r\n"
     "; pressed. Band / BandTop: the title band's two ends.\r\n"
     "Accent=FFACCBF1\r\n"
     "AccentBar=5AACCBF1\r\n"
     "Text=FFFFFFFF\r\n"
     "Grey=FFB4B4B4\r\n"
     "DimGrey=FF8A8A8A\r\n"
     "Panel=AA000000\r\n"
     "PanelDark=DC000000\r\n"
     "Separator=30FFFFFF\r\n"
     "Band=FF4F86C6\r\n"
     "BandTop=FF7AA9DE\r\n"
     "Green=FF36682C\r\n"
     "Red=FFB4191D\r\n"
     "; The Valkyrie ink round the 3D phone, and how wide it is (1 as shipped).\r\n"
     "Ink=FF000000\r\n"
     "InkWidth=1.0\r\n"},
    {"Features",
     "\r\n[Features]\r\n"
     "; 1 on, 0 off.\r\n"
     "; Rain gathering on the glass out in the rain, and how hard (1 as shipped).\r\n"
     "Rain=1\r\n"
     "RainAmount=1.0\r\n"
     "; Quick taps in one spot crack the glass; how many, and how long it lasts.\r\n"
     "Cracks=1\r\n"
     "CrackTaps=10\r\n"
     "CrackSeconds=60\r\n"
     "; Switched on, the phone starts up (the mark and a bar) for this long.\r\n"
     "Startup=1\r\n"
     "StartupSeconds=14\r\n"
     "; The ink outline round the 3D phone.\r\n"
     "Outline=1\r\n"
     "; Screens sliding and apps growing from their icons.\r\n"
     "Motion=1\r\n"
     "; The phone shaking when it buzzes.\r\n"
     "Vibrate=1\r\n"
     "; The pause menu's cursor (0: the phone's own arrow).\r\n"
     "MenuCursor=1\r\n"
     "; Holding an icon on the home screen to move the icons about.\r\n"
     "ArrangeIcons=1\r\n"
     "; The glass's shine over the screen.\r\n"
     "GlassShine=1\r\n"
     "; Fine scratches from use on the front glass, most round its corners; they\r\n"
     "; only show where light catches them. How many (1 as shipped, up to 3).\r\n"
     "Scratches=1\r\n"
     "ScratchAmount=1.0\r\n"
     "; Strong: plain to see in the sun. Subtle: fewer and fainter, only just there.\r\n"
     "ScratchLook=Strong\r\n"
     "; Blood on the glass after hitting someone with the phone, and how many\r\n"
     "; seconds it stays (fading over the last quarter).\r\n"
     "Blood=1\r\n"
     "BloodSeconds=60\r\n"
     "; Web pages load as slowly as 2007's EDGE network did. 0: at once.\r\n"
     "SlowPages=1\r\n"
     "; The signal comes from the radio masts on the map: fewer bars indoors and\r\n"
     "; far out, no service in the dead spots, and calls and web pages go with\r\n"
     "; it. 0: full signal everywhere.\r\n"
     "Signal=1\r\n"
     "; The game's subtitles for what is said on a call (it is on the phone too).\r\n"
     "Subtitles=0\r\n"
     "; The Maps app: San Andreas in 3D, drawn from the Valkyrie-radar-tiles folder\r\n"
     "; beside the game. 0 leaves the app off the phone and draws nothing for it.\r\n"
     "Maps=1\r\n"},
    {"Calendar",
     "\r\n[Calendar]\r\n"
     "; The year the calendar starts in; it turns over with the game's December.\r\n"
     "; A calendar already running keeps its year until the phone's data is reset.\r\n"
     "Year=2007\r\n"},
    {"Stocks",
     "\r\n[Stocks]\r\n"
     "; BAWSAQ's trading hours, as hours of the game's day (9.5 is half past nine).\r\n"
     "Open=9.5\r\n"
     "Close=16\r\n"
     "; The companies: TICKER=Name, the price it moves about, how far it wanders.\r\n"
     "CLKB=Cluckin' Bell, 42.10, 0.22\r\n"
     "BRGS=Burger Shot, 37.60, 0.20\r\n"
     "WSPZ=Well Stacked Pizza, 21.35, 0.26\r\n"
     "ECLA=eCola, 66.80, 0.14\r\n"
     "SPNK=Sprunk, 71.25, 0.13\r\n"
     "AMNU=Ammu-Nation, 54.90, 0.30\r\n"
     "BNCO=Binco, 8.75, 0.35\r\n"
     "ZIPC=Zip, 24.40, 0.24\r\n"
     "DSCH=Didier Sachs, 118.50, 0.18\r\n"
     "PRLP=ProLaps, 16.90, 0.32\r\n"
     "VICT=Victim, 31.20, 0.25\r\n"
     "SUBU=SubUrban, 12.60, 0.33\r\n"},
    {"Radio",
     "\r\n[Radio]\r\n"
     "; The stations the Radio app lists, by the game's numbers: 1 Playback FM,\r\n"
     "; 2 K-Rose, 3 K-DST, 4 Bounce FM, 5 SF-UR, 6 Radio Los Santos, 7 Radio X,\r\n"
     "; 8 CSR 103.9, 9 K-Jah West, 10 Master Sounds 98.3, 11 WCTR, 12 User Track\r\n"
     "; Player.\r\n"
     "Stations=1,2,3,4,5,6,7,8,9,10,11,12\r\n"},
    {"Model",
     "\r\n[Model]\r\n"
     "; How much the phone in CJ's hand reflects, as the game's cars do: 0 matt,\r\n"
     "; 1 as shiny as it goes. The phone on the screen shines with the light CJ\r\n"
     "; stands in either way.\r\n"
     "Shine=1.0\r\n"
     "; 1: reflect a copy of the current game frame without a second world\r\n"
     "; render. 0: use the bundled studio environment picture instead.\r\n"
     "Reflections=1\r\n"
     "; How the phone sits in CJ's left hand: turned by HandTurn (degrees about the\r\n"
     "; hand's x, y and z) and moved by HandOffset (metres, along the hand's x, y\r\n"
     "; and z). The defaults are the game's own for the second pistol of a pair.\r\n"
     "HandTurn=180,0,0\r\n"
     "HandOffset=0.04,0.05,0\r\n"
     "; 1: the phone is turned half round about its own length, so its screen faces\r\n"
     "; CJ. 0 if a model of your own already faces him.\r\n"
     "HandFlip=1\r\n"},
    {"Apps",
     "\r\n[Apps]\r\n"
     "; The home screen, in order, four to a row. Leave an app out to hide it.\r\n"
     "; The apps: Phone, Text, Contacts, Camera, Photos, Maps, Internet, Games,\r\n"
     "; Weather, Stocks, Radio, Calendar, Clock, Calculator, Notes, Flashlight,\r\n"
     "; Settings.\r\n"
     "Order=Phone,Text,Contacts,Camera,Photos,Maps,Weather,Stocks,Internet,Games,Radio,Clock,Calendar,Calculator,Notes,Flashlight,Settings\r\n"
     "; The dock along the bottom of the home screen, up to four; they are not\r\n"
     "; shown again above it.\r\n"
     "Dock=Phone,Text,Internet,Games\r\n"
     "\r\n"
     "[Icons]\r\n"
     "; Each app's icon. One of:\r\n"
     ";   a picture in valkyrie-phone.txd (app_phone, app_text, app_contacts,\r\n"
     ";   app_photos, app_clock, app_internet, app_settings, app_games,\r\n"
     ";   app_calculator, app_notes, app_maps, app_weather, app_stocks, app_radio,\r\n"
     ";   app_calendar, app_flashlight);\r\n"
     ";   hud:NAME for one of the game's radar icons in models\\hud.txd, such as\r\n"
     ";   hud:radar_1hourphoto;\r\n"
     ";   a .png file, from the game folder, such as icons\\myphone.png.\r\n"
     "Phone=app_phone\r\n"
     "Text=app_text\r\n"
     "Contacts=app_contacts\r\n"
     "Camera=app_photos\r\n"
     "Photos=hud:radar_1hourphoto\r\n"
     "Maps=app_maps\r\n"
     "Weather=app_weather\r\n"
     "Stocks=app_stocks\r\n"
     "Radio=app_radio\r\n"
     "Calendar=app_calendar\r\n"
     "Internet=app_internet\r\n"
     "Games=app_games\r\n"
     "Clock=app_clock\r\n"
     "Calculator=app_calculator\r\n"
     "Notes=app_notes\r\n"
     "Flashlight=app_flashlight\r\n"
     "Settings=hud:radar_modGarage\r\n"
     "\r\n"
     "[Labels]\r\n"
     "; The name under each icon.\r\n"
     "Phone=Phone\r\n"
     "Text=Text\r\n"
     "Contacts=Contacts\r\n"
     "Camera=Camera\r\n"
     "Photos=Photos\r\n"
     "Maps=Maps\r\n"
     "Weather=Weather\r\n"
     "Stocks=Stocks\r\n"
     "Radio=Radio\r\n"
     "Calendar=Calendar\r\n"
     "Internet=Internet\r\n"
     "Games=Games\r\n"
     "Clock=Clock\r\n"
     "Calculator=Calculator\r\n"
     "Notes=Notes\r\n"
     "Flashlight=Flashlight\r\n"
     "Settings=Settings\r\n"},
    {"Contacts",
     "\r\n[Contacts]\r\n"
     "; The people already in CJ's phone: Name=Number (digits, * and #), and after a\r\n"
     "; comma, if you like, a picture - the same kinds [Icons] takes. They are\r\n"
     "; listed in Contacts beside the ones you add, and are changed here, not\r\n"
     "; on the phone. The game has no phone numbers for them; these are made up.\r\n"
     "; None of them picks up.\r\n"
     "Sweet=5550101, hud:radar_SWEET\r\n"
     "Kendl=5550102\r\n"
     "Big Smoke=5550103, hud:radar_BIGSMOKE\r\n"
     "Ryder=5550104, hud:radar_RYDER\r\n"
     "OG Loc=5550105, hud:radar_OGLOC\r\n"
     "Cesar=5550106, hud:radar_CESARVIAPANDO\r\n"
     "Catalina=5550107, hud:radar_CATALINAPINK\r\n"
     "The Truth=5550108, hud:radar_THETRUTH\r\n"
     "Woozie=5550109, hud:radar_WOOZIE\r\n"
     "Mike Toreno=5550110, hud:radar_TORENO\r\n"
     "Zero=5550111, hud:radar_ZERO\r\n"
     "Jethro=5550112\r\n"
     "Dwaine=5550113\r\n"
     "Kent Paul=5550114\r\n"
     "Maccer=5550115\r\n"
     "Ken Rosenberg=5550116\r\n"
     "Madd Dogg=5550117, hud:radar_MADDOG\r\n"
     "Salvatore Leone=5550118, hud:radar_mafiaCasino\r\n"
     "Emmet=5550119, hud:radar_emmetGun\r\n"
     "Denise=5550120, hud:radar_girlfriend\r\n"
     "Michelle=5550121, hud:radar_girlfriend\r\n"
     "Helena=5550122, hud:radar_girlfriend\r\n"
     "Katie=5550123, hud:radar_girlfriend\r\n"
     "Barbara=5550124, hud:radar_girlfriend\r\n"
     "Millie=5550125, hud:radar_girlfriend\r\n"},
    {"Ringtones",
     "\r\n[Ringtones]\r\n"
     "; The ringtones to pick from in Settings: Name=Sound. A sound is one of the\r\n"
     "; game's own mission-audio sounds by number (the numbers SA-MP's\r\n"
     "; PlayerPlaySound takes - Valkyrie, the phone's own, is 20600), or a .wav file\r\n"
     "; in the game folder, such as ringtones\\mine.wav. Every .wav file in\r\n"
     "; valkyrie-phone-tones\\ringtones is listed after these, named after the\r\n"
     "; file, so a ringtone can also be added by putting it in there.\r\n"
     "Valkyrie=20600\r\n"
     "\r\n"
     "[TextTones]\r\n"
     "; The same, for a text arriving. The .wav files in\r\n"
     "; valkyrie-phone-tones\\texttones are listed after these.\r\n"
     "Valkyrie=20600\r\n"
     "\r\n"
     "[Sounds]\r\n"
     "; Heard while a call rings out; SP-RP's dial tone is 3600.\r\n"
     "Ringback=3600\r\n"
     "; A keypad key and a sent text. Empty is silent. Numbers from 1000 to 1999\r\n"
     "; are the game's short sound effects.\r\n"
     "Key=\r\n"
     "Sent=\r\n"},
    {"Services",
     "\r\n[Services]\r\n"
     "; The numbers that answer, with the SP-RP lines. 911 sends the police (who\r\n"
     "; guard you unless you are wanted), a fire engine (whose crew put out the\r\n"
     "; fires round you) or an ambulance (whose paramedic treats you).\r\n"
     "Emergency=911\r\n"
     "NonEmergency=311\r\n"
     "Hotline=726\r\n"
     "Hotel=666\r\n"
     "; Dialling this saves the game where CJ is standing, after asking.\r\n"
     "Save=100\r\n"
     "; Dialling this opens valkyrie-trainer, when it is installed (*#TRAIN#).\r\n"
     "Trainer=*#87246#\r\n"
     "; Restaurants that deliver to where you are, at their shops' prices.\r\n"
     "Pizza=5557492\r\n"
     "Burger=5552874\r\n"
     "Chicken=5552582\r\n"},
    {"Camera",
     "\r\n[Camera]\r\n"
     "; The Camera app shows what the phone sees on its own screen. The mouse\r\n"
     "; aims, a click takes the picture, the right button or Tab turns the phone\r\n"
     "; round for a selfie, the wheel zooms, Esc goes back. How fast the mouse\r\n"
     "; aims:\r\n"
     "Sensitivity=1.0\r\n"},
    {"LiveView",
     "\r\n[LiveView]\r\n"
     "; 1 draws what the phone's lens sees on its screen, through the game's\r\n"
     "; mirror renderer. 0 turns that off if it causes trouble; photos still work.\r\n"
     "On=1\r\n"},
    {"Photos",
     "\r\n[Photos]\r\n"
     "; Photos are saved where the game's own camera saves them, in the Gallery\r\n"
     "; folder of the game's User Files. Portrait keeps the upright picture the\r\n"
     "; viewfinder showed; Screen keeps the whole screen, as the game's camera does.\r\n"
     "Shape=Portrait\r\n"
     "; A white flash over the screen after each picture (1), or none (0).\r\n"
     "Flash=0\r\n"},
    {"Animations",
     "\r\n[Animations]\r\n"
     "; What CJ does with the phone: an animation, the .ifp it is in (ped is\r\n"
     "; always loaded; any other is loaded when needed), and whether it loops\r\n"
     "; (1) or holds its last frame (0).\r\n"
     "; Looking at the phone while it is up:\r\n"
     "Use=betslp_loop\r\n"
     "UseFile=otb\r\n"
     "UseLoop=1\r\n"
     "; Holding it up to take a picture:\r\n"
     "Camera=picstnd_in\r\n"
     "CameraFile=camera\r\n"
     "CameraLoop=0\r\n"
     "; Holding it out for a selfie:\r\n"
     "Selfie=ARRESTgun\r\n"
     "SelfieFile=ped\r\n"
     "SelfieLoop=0\r\n"
     "; Where the phone's lens is in each pose, in metres from CJ's middle:\r\n"
     "; to his right, in front of him, up. Match these to the animations above.\r\n"
     "CameraLens=0.05,0.60,0.65\r\n"
     "SelfieLens=0.20,1.10,0.66\r\n"},
    {"Internet",
     "\r\n[Internet]\r\n"
     "; The page the browser opens on, and Home goes to.\r\n"
     "Home=www.eyefind.info\r\n"},
};

std::string Read(const std::string& ini, const char* section, const char* key, const char* fallback) {
    char buf[512] = {};
    GetPrivateProfileStringA(section, key, fallback, buf, sizeof buf, ini.c_str());
    std::string s = buf;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    return s;
}

// Name=Value lines of a section, in file order.
std::vector<std::pair<std::string, std::string>> Pairs(const std::string& ini, const char* section) {
    std::vector<char> buf(65536);
    const DWORD n = GetPrivateProfileSectionA(section, buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    std::vector<std::pair<std::string, std::string>> out;
    for (DWORD i = 0; i < n;) {
        std::string line(buf.data() + i);
        i += static_cast<DWORD>(line.size()) + 1;
        if (line.empty() || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            return s;
        };
        out.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
    }
    return out;
}

bool HasSection(const std::string& ini, const char* section) {
    char buf[8] = {};
    return GetPrivateProfileSectionA(section, buf, sizeof buf, ini.c_str()) > 0;
}

// Write the file as it ships, or add the sections an older one is missing.
void WriteDefaults(const std::string& ini) {
    const bool exists = GetFileAttributesA(ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::string add;
    for (const Section& s : kSections) {
        if (!exists || !HasSection(ini, s.name)) add += s.text;
    }
    // Keys added to a section after it first shipped.
    if (exists && HasSection(ini, "Phone")) {
        char v[32] = {};
        if (!GetPrivateProfileStringA("Phone", "Skin", "", v, sizeof v, ini.c_str()))
            WritePrivateProfileStringA("Phone", "Skin", "iFruit", ini.c_str());
    }
    if (exists && HasSection(ini, "Features")) {
        char v[16] = {};
        if (!GetPrivateProfileStringA("Features", "Scratches", "", v, sizeof v, ini.c_str())) {
            WritePrivateProfileStringA("Features", "Scratches", "1", ini.c_str());
            WritePrivateProfileStringA("Features", "ScratchAmount", "1.0", ini.c_str());
        }
        if (!GetPrivateProfileStringA("Features", "ScratchLook", "", v, sizeof v, ini.c_str()))
            WritePrivateProfileStringA("Features", "ScratchLook", "Strong", ini.c_str());
        for (const auto& [key, value] : {std::pair<const char*, const char*>{"Blood", "1"}, {"BloodSeconds", "60"},
                                         {"SlowPages", "1"}, {"Signal", "1"}}) {
            if (!GetPrivateProfileStringA("Features", key, "", v, sizeof v, ini.c_str()))
                WritePrivateProfileStringA("Features", key, value, ini.c_str());
        }
    }
    if (add.empty()) return;
    std::ofstream out(ini, std::ios::binary | std::ios::app);
    if (exists) out << "\r\n";
    out << add;
    logfile::Line("config: %s %s", exists ? "added missing sections to" : "wrote", ini.c_str());
}

std::vector<Tone> Tones(const std::string& ini, const char* section, const char* fallbackName,
                        const char* fallbackSound) {
    std::vector<Tone> out;
    for (const auto& kv : Pairs(ini, section)) {
        if (kv.second.empty()) continue;
        // The phone's own tone was called SP-RP, and as a text tone (40405)
        // it made no sound; an ini written then gets Valkyrie's.
        if (kv.first == "SP-RP") out.push_back({"Valkyrie", "20600"});
        else out.push_back({kv.first, kv.second});
    }
    if (out.empty()) out.push_back({fallbackName, fallbackSound});
    return out;
}

// The .wav files in one of valkyrie-phone-tones' folders, added after the
// ini's own tones under their file names. `absolute`: kept by their full
// path (the phone's own, unpacked from the ASI) rather than the game folder's.
void AddToneFiles(const std::string& gameDir, const char* folder, std::vector<Tone>& tones, bool absolute = false) {
    const std::string relative = std::string("valkyrie-phone-tones\\") + folder + "\\";
    std::vector<std::string> files;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((gameDir + relative + "*.wav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end(),
              [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const std::string& file : files) {
        const std::string name = file.substr(0, file.size() - 4);
        const bool taken = std::any_of(tones.begin(), tones.end(), [&](const Tone& t) {
            return _stricmp(t.name.c_str(), name.c_str()) == 0;
        });
        if (!taken) tones.push_back({name, (absolute ? gameDir : std::string()) + relative + file});
    }
}

}  // namespace

const std::vector<std::string>& AllApps() { return kAllApps; }

void Load(const std::string& gameDir) {
    const std::string ini = gameDir + "valkyrie-phone.ini";
    WriteDefaults(ini);
    Config c;

    const std::string key = Read(ini, "Phone", "Key", "P");
    // None, Off or 0: no key; the phone is taken out another way (the
    // inventory's Phone).
    if (_stricmp(key.c_str(), "none") == 0 || _stricmp(key.c_str(), "off") == 0 || key == "0") c.key = 0;
    else if (key.size() == 1) c.key = toupper(static_cast<unsigned char>(key[0]));
    else if (!key.empty()) c.key = static_cast<int>(strtol(key.c_str(), nullptr, 0));
    c.height = std::clamp(static_cast<float>(atof(Read(ini, "Phone", "Height", "0.78").c_str())), 0.4f, 0.95f);
    c.right = _stricmp(Read(ini, "Phone", "Side", "right").c_str(), "left") != 0;
    c.model = Read(ini, "Phone", "Model", c.model.c_str());
    c.modelTextures = Read(ini, "Phone", "ModelTextures", c.modelTextures.c_str());
    c.reflections = atoi(Read(ini, "Model", "Reflections", "1").c_str()) != 0;
    auto three = [&](const char* key, const char* fallback, float* out) {
        const std::string v = Read(ini, "Model", key, fallback);
        float t[3] = {0, 0, 0};
        if (sscanf(v.c_str(), "%f , %f , %f", &t[0], &t[1], &t[2]) != 3) sscanf(fallback, "%f,%f,%f", &t[0], &t[1], &t[2]);
        for (int i = 0; i < 3; ++i) out[i] = t[i];
    };
    three("HandTurn", "180,0,0", c.handTurn);
    three("HandOffset", "0.04,0.05,0", c.handOffset);
    c.handFlip = atoi(Read(ini, "Model", "HandFlip", "1").c_str()) != 0;
    c.model3d = atoi(Read(ini, "Phone", "Model3D", "1").c_str()) != 0;
    {
        const std::string skin = Read(ini, "Phone", "Skin", "iFruit");
        c.keypad = _stricmp(skin.c_str(), "Keypad") == 0;
    }
    // [Look]. Under the keypad skin, its own colours stand in for
    // any the file leaves as they ship.
    const Config::Look ifruit{};
    if (c.keypad) {
        auto& l = c.look;
        l.accent = 0xFF56D6EC;
        l.accentBar = 0x6038A8C8;
        l.dimGrey = 0xFF7C8C94;
        l.panel = 0xB4000000;
        l.panelDark = 0xE6000000;
        l.separator = 0x3856D6EC;
        l.band = 0xFF071018;
        l.bandTop = 0xFF0E2330;
    }
    auto colour = [&](const char* key, uint32_t& out, uint32_t shipped) {
        const std::string v = Read(ini, "Look", key, "");
        if (v.empty()) return;
        const uint32_t value = static_cast<uint32_t>(strtoul(v.c_str(), nullptr, 16));
        if (!c.keypad || value != shipped) out = value;
    };
    colour("Accent", c.look.accent, ifruit.accent);
    colour("AccentBar", c.look.accentBar, ifruit.accentBar);
    colour("Text", c.look.text, ifruit.text);
    colour("Grey", c.look.grey, ifruit.grey);
    colour("DimGrey", c.look.dimGrey, ifruit.dimGrey);
    colour("Panel", c.look.panel, ifruit.panel);
    colour("PanelDark", c.look.panelDark, ifruit.panelDark);
    colour("Separator", c.look.separator, ifruit.separator);
    colour("Band", c.look.band, ifruit.band);
    colour("BandTop", c.look.bandTop, ifruit.bandTop);
    colour("Green", c.look.green, ifruit.green);
    colour("Red", c.look.red, ifruit.red);
    colour("Ink", c.look.ink, ifruit.ink);
    c.look.inkWidth = std::clamp(static_cast<float>(atof(Read(ini, "Look", "InkWidth", "1.0").c_str())), 0.0f, 4.0f);
    // [Features]
    auto flag = [&](const char* key, bool fallback) {
        return atoi(Read(ini, "Features", key, fallback ? "1" : "0").c_str()) != 0;
    };
    auto& f = c.features;
    f.rain = flag("Rain", true);
    f.cracks = flag("Cracks", true);
    f.startup = flag("Startup", true);
    f.outline = flag("Outline", true);
    f.motion = flag("Motion", true);
    f.vibrate = flag("Vibrate", true);
    f.menuCursor = flag("MenuCursor", true);
    f.arrange = flag("ArrangeIcons", true);
    f.glass = flag("GlassShine", true);
    f.subtitles = flag("Subtitles", false);
    f.maps = flag("Maps", true);
    f.scratches = flag("Scratches", true);
    f.blood = flag("Blood", true);
    f.slowPages = flag("SlowPages", true);
    f.signal = flag("Signal", true);
    f.bloodSeconds = std::clamp(static_cast<float>(atof(Read(ini, "Features", "BloodSeconds", "60").c_str())), 5.0f, 3600.0f);
    f.scratchStrong = _stricmp(Read(ini, "Features", "ScratchLook", "Strong").c_str(), "Subtle") != 0;
    f.scratchAmount = std::clamp(static_cast<float>(atof(Read(ini, "Features", "ScratchAmount", "1.0").c_str())), 0.0f, 3.0f);
    f.rainAmount = std::clamp(static_cast<float>(atof(Read(ini, "Features", "RainAmount", "1.0").c_str())), 0.0f, 4.0f);
    f.crackTaps = std::clamp(atoi(Read(ini, "Features", "CrackTaps", "10").c_str()), 2, 50);
    f.crackSeconds = std::clamp(static_cast<float>(atof(Read(ini, "Features", "CrackSeconds", "60").c_str())), 5.0f, 3600.0f);
    f.startupSeconds = std::clamp(static_cast<float>(atof(Read(ini, "Features", "StartupSeconds", "14").c_str())), 1.0f, 120.0f);
    // [Calendar]
    c.calendarYear = std::clamp(atoi(Read(ini, "Calendar", "Year", "2007").c_str()), 1, 9999);
    // [Stocks]
    c.marketOpen = std::clamp(static_cast<float>(atof(Read(ini, "Stocks", "Open", "9.5").c_str())), 0.0f, 23.0f);
    c.marketClose = std::clamp(static_cast<float>(atof(Read(ini, "Stocks", "Close", "16").c_str())), c.marketOpen + 0.5f, 24.0f);
    for (const auto& kv : Pairs(ini, "Stocks")) {
        if (_stricmp(kv.first.c_str(), "Open") == 0 || _stricmp(kv.first.c_str(), "Close") == 0) continue;
        const std::string& v = kv.second;
        const size_t a = v.find(','), b = a == std::string::npos ? a : v.find(',', a + 1);
        if (b == std::string::npos) continue;
        auto trim = [](std::string t) {
            while (!t.empty() && isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
            while (!t.empty() && isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
            return t;
        };
        const float base = static_cast<float>(atof(v.substr(a + 1, b - a - 1).c_str()));
        const float swing = static_cast<float>(atof(v.substr(b + 1).c_str()));
        if (base > 0.0f) c.companies.push_back({kv.first, trim(v.substr(0, a)), base, std::clamp(swing, 0.0f, 1.0f)});
    }
    if (c.companies.empty()) c.companies.push_back({"CLKB", "Cluckin' Bell", 42.10f, 0.22f});
    // [Radio]
    {
        const std::string list = Read(ini, "Radio", "Stations", "1,2,3,4,5,6,7,8,9,10,11,12");
        size_t p = 0;
        while (p < list.size()) {
            const size_t comma = std::min(list.find(',', p), list.size());
            const int id = atoi(list.substr(p, comma - p).c_str());
            if (id >= 1 && id <= 12) c.radioStations.push_back(id);
            p = comma + 1;
        }
    }
    c.screenEffect = std::clamp(static_cast<float>(atof(Read(ini, "Phone", "ScreenEffect", "0.5").c_str())), 0.0f, 1.0f);
    c.shine = std::clamp(static_cast<float>(atof(Read(ini, "Model", "Shine", "1.0").c_str())), 0.0f, 1.0f);

    // The apps, in the file's order; anything it names that the phone does
    // not have is left out, and an empty list means all of them.
    auto list = [&](const char* key, const char* fallback) {
        std::vector<App> out;
        std::stringstream in(Read(ini, "Apps", key, fallback));
        std::string id;
        while (std::getline(in, id, ',')) {
            while (!id.empty() && id.front() == ' ') id.erase(0, 1);
            while (!id.empty() && id.back() == ' ') id.pop_back();
            auto it = std::find_if(kAllApps.begin(), kAllApps.end(),
                                   [&](const std::string& a) { return _stricmp(a.c_str(), id.c_str()) == 0; });
            if (it == kAllApps.end()) {
                if (!id.empty()) logfile::Line("config: no app called \"%s\"", id.c_str());
                continue;
            }
            if (std::none_of(out.begin(), out.end(), [&](const App& a) { return a.id == *it; })) {
                out.push_back({*it, *it, ""});
            }
        }
        return out;
    };
    // A file written before an app existed does not list it, and would keep it
    // off the home screen for good. Each new app is put into its Order once,
    // after the app it belongs beside; taken out again, it stays out.
    {
        std::string order = Read(ini, "Apps", "Order", "");
        std::string added = Read(ini, "Apps", "Added", "");
        bool changed = false;
        for (const auto& [app, after] : kAddedApps) {
            if (("," + added + ",").find("," + app + ",") != std::string::npos) continue;
            added += (added.empty() ? "" : ",") + app;
            changed = true;
            if (order.empty() || ("," + order + ",").find("," + app + ",") != std::string::npos) continue;
            const size_t at = ("," + order + ",").find("," + after + ",");
            if (at == std::string::npos) order += "," + app;
            else order.insert(at + after.size(), "," + app);
        }
        if (changed) {
            if (!order.empty()) WritePrivateProfileStringA("Apps", "Order", order.c_str(), ini.c_str());
            WritePrivateProfileStringA("Apps", "Added", added.c_str(), ini.c_str());
        }
    }
    c.apps = list("Order", "");
    if (c.apps.empty()) {
        for (const auto& a : kAllApps) c.apps.push_back({a, a, ""});
    }
    c.dock = list("Dock", "Phone,Text,Internet,Games");
    if (c.dock.size() > 4) c.dock.resize(4);
    static const std::map<std::string, std::string> kIcons = {
        {"Phone", "app_phone"}, {"Text", "app_text"}, {"Contacts", "app_contacts"}, {"Camera", "app_photos"},
        {"Photos", "hud:radar_1hourphoto"}, {"Maps", "app_maps"}, {"Internet", "app_internet"}, {"Games", "app_games"},
        {"Clock", "app_clock"}, {"Calculator", "app_calculator"}, {"Notes", "app_notes"},
        {"Settings", "hud:radar_modGarage"}, {"Weather", "app_weather"}, {"Stocks", "app_stocks"},
        {"Radio", "app_radio"}, {"Calendar", "app_calendar"}, {"Flashlight", "app_flashlight"}};
    for (auto* apps : {&c.apps, &c.dock}) {
        for (App& a : *apps) {
            a.icon = Read(ini, "Icons", a.id.c_str(), kIcons.at(a.id).c_str());
            a.label = Read(ini, "Labels", a.id.c_str(), a.id.c_str());
        }
        // [Features] Maps=0: no Maps app, wherever the lists or a saved home
        // screen had it.
        if (!c.features.maps) {
            apps->erase(std::remove_if(apps->begin(), apps->end(), [](const App& a) { return a.id == "Maps"; }),
                        apps->end());
        }
    }

    // The story's people have radar icons of their own; a file written before
    // contacts took pictures gets them by name.
    static const std::map<std::string, std::string> kPictures = {
        {"Sweet", "hud:radar_SWEET"},          {"Big Smoke", "hud:radar_BIGSMOKE"},
        {"Ryder", "hud:radar_RYDER"},          {"OG Loc", "hud:radar_OGLOC"},
        {"Cesar", "hud:radar_CESARVIAPANDO"},  {"Catalina", "hud:radar_CATALINAPINK"},
        {"The Truth", "hud:radar_THETRUTH"},   {"Woozie", "hud:radar_WOOZIE"},
        {"Mike Toreno", "hud:radar_TORENO"},   {"Zero", "hud:radar_ZERO"},
        {"Madd Dogg", "hud:radar_MADDOG"},     {"Salvatore Leone", "hud:radar_mafiaCasino"},
        {"Emmet", "hud:radar_emmetGun"},       {"Denise", "hud:radar_girlfriend"},
        {"Michelle", "hud:radar_girlfriend"},  {"Helena", "hud:radar_girlfriend"},
        {"Katie", "hud:radar_girlfriend"},     {"Barbara", "hud:radar_girlfriend"},
        {"Millie", "hud:radar_girlfriend"}};
    for (const auto& kv : Pairs(ini, "Contacts")) {
        const size_t comma = kv.second.find(',');
        std::string digits;
        for (char ch : kv.second.substr(0, comma)) {
            // * and # too, for codes like the trainer's.
            if (isdigit(static_cast<unsigned char>(ch)) || ch == '*' || ch == '#') digits += ch;
        }
        std::string picture = comma == std::string::npos ? std::string() : kv.second.substr(comma + 1);
        while (!picture.empty() && picture.front() == ' ') picture.erase(0, 1);
        if (comma == std::string::npos) {
            const auto it = kPictures.find(kv.first);
            if (it != kPictures.end()) picture = it->second;
        }
        if (!kv.first.empty() && !digits.empty()) c.contacts.push_back({kv.first, digits, picture});
    }

    c.ringtones = Tones(ini, "Ringtones", "Valkyrie", "20600");
    c.textTones = Tones(ini, "TextTones", "Valkyrie", "20600");
    // A player's own in the game folder, then the phone's own from the ASI.
    AddToneFiles(gameDir, "ringtones", c.ringtones);
    AddToneFiles(gameDir, "texttones", c.textTones);
    if (!bundle::Dir().empty()) {
        AddToneFiles(bundle::Dir(), "ringtones", c.ringtones, true);
        AddToneFiles(bundle::Dir(), "texttones", c.textTones, true);
    }
    c.ringback = Read(ini, "Sounds", "Ringback", "3600");
    c.keyTone = Read(ini, "Sounds", "Key", "");
    c.sentTone = Read(ini, "Sounds", "Sent", "");

    c.emergency = Read(ini, "Services", "Emergency", "911");
    c.nonEmergency = Read(ini, "Services", "NonEmergency", "311");
    c.hotline = Read(ini, "Services", "Hotline", "726");
    c.hotel = Read(ini, "Services", "Hotel", "666");

    c.save = Read(ini, "Services", "Save", "100");
    c.trainer = Read(ini, "Services", "Trainer", "*#87246#");
    // The trainer in the contacts, at whatever number [Services] gives it, so
    // nobody has to know the code. Deleted on the phone like any other.
    if (!c.trainer.empty()) {
        bool listed = false;
        for (const auto& contact : c.contacts) listed = listed || contact.number == c.trainer;
        if (!listed) c.contacts.push_back({"Trainer", c.trainer, ""});
    }
    c.pizza = Read(ini, "Services", "Pizza", "5557492");
    c.burger = Read(ini, "Services", "Burger", "5552874");
    c.chicken = Read(ini, "Services", "Chicken", "5552582");
    // In the contacts too, with their radar icons.
    const std::pair<const char*, const std::string*> restaurants[] = {
        {"Well Stacked Pizza", &c.pizza}, {"Burger Shot", &c.burger}, {"Cluckin' Bell", &c.chicken}};
    const char* const restaurantIcons[] = {"hud:radar_pizza", "hud:radar_burgerShot", "hud:radar_chicken"};
    for (int i = 0; i < 3; ++i) {
        const std::string& number = *restaurants[i].second;
        if (number.empty()) continue;
        bool listed = false;
        for (const auto& contact : c.contacts) listed = listed || contact.number == number;
        if (!listed) c.contacts.push_back({restaurants[i].first, number, restaurantIcons[i]});
    }
    c.cameraSensitivity =
        std::clamp(static_cast<float>(atof(Read(ini, "Camera", "Sensitivity", "1.0").c_str())), 0.1f, 5.0f);
    c.liveView = atoi(Read(ini, "LiveView", "On", "1").c_str()) != 0;
    c.portraitPhotos = _stricmp(Read(ini, "Photos", "Shape", "Portrait").c_str(), "Screen") != 0;
    c.photoFlash = atoi(Read(ini, "Photos", "Flash", "0").c_str()) != 0;
    auto anim = [&](const char* key, const char* name, const char* file, bool loop) {
        const std::string k = key;
        Anim a;
        a.name = Read(ini, "Animations", k.c_str(), name);
        a.file = Read(ini, "Animations", (k + "File").c_str(), file);
        a.loop = atoi(Read(ini, "Animations", (k + "Loop").c_str(), loop ? "1" : "0").c_str()) != 0;
        if (a.file.empty()) a.file = "ped";
        return a;
    };
    c.useAnim = anim("Use", "betslp_loop", "otb", true);
    c.cameraAnim = anim("Camera", "picstnd_in", "camera", false);
    // An older file kept the selfie pose under [Camera].
    const std::string oldSelfie = Read(ini, "Camera", "SelfieAnim", "ARRESTgun");
    const std::string oldSelfieFile = Read(ini, "Camera", "SelfieAnimFile", "ped");
    c.selfieAnim = anim("Selfie", oldSelfie.c_str(), oldSelfieFile.c_str(), false);
    auto lens = [&](const char* key, Lens fallback) {
        const std::string text = Read(ini, "Animations", key, "");
        Lens l{};
        if (sscanf_s(text.c_str(), "%f , %f , %f", &l.right, &l.forward, &l.up) != 3) return fallback;
        auto sane = [](float v) { return std::clamp(v, -1.5f, 1.5f); };
        return Lens{sane(l.right), sane(l.forward), sane(l.up)};
    };
    c.cameraLens = lens("CameraLens", c.cameraLens);
    c.selfieLens = lens("SelfieLens", c.selfieLens);
    c.homePage = Read(ini, "Internet", "Home", "www.eyefind.info");

    g_config = c;
    logfile::Line("config: key %d, %zu apps, %zu contacts, %zu ringtones", c.key, c.apps.size(), c.contacts.size(),
                  c.ringtones.size());
}

const Config& Get() { return g_config; }

}  // namespace config
