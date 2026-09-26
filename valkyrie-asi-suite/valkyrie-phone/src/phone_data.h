// What the phone remembers: contacts, text messages, recent calls, and the
// settings. The server keeps these in its SQLite database per character; here
// there is one player, so they live in one file in the game's user folder,
// beside the save games.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace phone_data {

// The server's limits, so the phone fills up where it always did.
constexpr size_t kMaxContacts = 100;
constexpr size_t kMaxMessages = 128;  // "You have too many inbox messages (128)"
constexpr size_t kMaxRecents = 40;
constexpr size_t kMaxMessageLength = 128;  // "can't be longer than 128 characters"
constexpr size_t kMaxNameLength = 24;
constexpr size_t kMaxNumberLength = 12;

struct Contact {
    std::string name;
    std::string number;
    std::string picture;  // an icon as the ini names one, or a photo's path; empty for none
};

// When something happened, on the game's clock. San Andreas has a weekday and
// a time of day but no calendar, so that is what is kept.
struct Stamp {
    int day = 1;  // CClock::CurrentDay, 1 is Sunday
    int hour = 0;
    int minute = 0;
};

struct Message {
    std::string number;  // the other end
    std::string text;
    bool outgoing = true;
    bool delivered = false;
    bool read = true;
    Stamp at;
};

struct Recent {
    std::string number;
    bool outgoing = true;
    Stamp at;
};

struct Settings {
    int wallpaper = 3;  // "Police Car", the server's default
    bool slideToUnlock = true;
    bool poweredOn = true;
    int volume = 7;       // 0 to 10, the side buttons
    bool silent = false;  // the ring switch: ringtones and text tones held back
    std::string ownNumber;  // made up once, the first time the phone is used
};

struct Data {
    std::vector<Contact> contacts;
    // CJ's own people, from the ini: shown with the contacts, never saved.
    std::vector<Contact> builtIn;
    std::vector<Message> messages;  // oldest first
    std::vector<Recent> recents;    // newest first
    Settings settings;
    // Anything else an app keeps: the games' high-score tables.
    std::map<std::string, std::string> store;
};

Data& Get();

// Where the file is. Call once the game has set its user folder up.
void Init(const std::string& path);

void Load();
void Save();

// Helpers the screens share.
std::string NameFor(const std::string& number);  // contact name, or the number itself
int FindContact(const std::string& number);       // index, or -1
void SortContacts();
Stamp Now();
std::string FormatTime(const Stamp& s);      // "3:45 PM"
std::string FormatDayTime(const Stamp& s);   // "Tuesday 3:45 PM"
std::string DayName(int day);
std::string FormatNumber(const std::string& digits);  // 5550123 -> 555-0123

}  // namespace phone_data
