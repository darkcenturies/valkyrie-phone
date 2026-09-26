#include "phone_data.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

#include "log.h"

namespace phone_data {
namespace {

// CClock, v1.0 US.
constexpr uintptr_t kClockDay = 0xB7014E;
constexpr uintptr_t kClockMinutes = 0xB70152;
constexpr uintptr_t kClockHours = 0xB70153;

Data g_data;
std::string g_path;

// One record per line, fields split by tabs. Text can hold neither, so tabs,
// newlines and backslashes are escaped.
std::string Escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out.push_back(n == 't' ? '\t' : n == 'n' ? '\n' : n);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            field.push_back(line[i]);
            field.push_back(line[++i]);
        } else if (line[i] == '\t') {
            fields.push_back(Unescape(field));
            field.clear();
        } else {
            field.push_back(line[i]);
        }
    }
    fields.push_back(Unescape(field));
    return fields;
}

int ToInt(const std::string& s, int fallback = 0) {
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

std::string StampText(const Stamp& s) {
    return std::to_string(s.day) + "\t" + std::to_string(s.hour) + "\t" + std::to_string(s.minute);
}

Stamp ReadStamp(const std::vector<std::string>& f, size_t from) {
    Stamp s;
    if (f.size() >= from + 3) {
        s.day = std::clamp(ToInt(f[from], 1), 1, 7);
        s.hour = std::clamp(ToInt(f[from + 1]), 0, 23);
        s.minute = std::clamp(ToInt(f[from + 2]), 0, 59);
    }
    return s;
}

std::string MakeNumber() {
    // The server's rule: 160000 plus up to 9999.
    std::random_device rd;
    std::uniform_int_distribution<int> pick(0, 9998);
    return std::to_string(160000 + pick(rd));
}

}  // namespace

Data& Get() {
    return g_data;
}

void Init(const std::string& path) {
    g_path = path;
}

void Load() {
    g_data = Data{};
    std::ifstream in(g_path, std::ios::binary);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto f = Split(line);
            if (f.empty()) continue;
            const std::string& kind = f[0];
            if (kind == "contact" && f.size() >= 3) {
                g_data.contacts.push_back({f[1], f[2], f.size() >= 4 ? f[3] : std::string()});
            } else if (kind == "message" && f.size() >= 9) {
                Message m;
                m.number = f[1];
                m.outgoing = f[2] == "1";
                m.delivered = f[3] == "1";
                m.read = f[4] == "1";
                m.at = ReadStamp(f, 5);
                m.text = f[8];
                g_data.messages.push_back(m);
            } else if (kind == "recent" && f.size() >= 6) {
                Recent r;
                r.number = f[1];
                r.outgoing = f[2] == "1";
                r.at = ReadStamp(f, 3);
                g_data.recents.push_back(r);
            } else if (kind == "wallpaper" && f.size() >= 2) {
                g_data.settings.wallpaper = std::clamp(ToInt(f[1], 3), 0, 26);
            } else if (kind == "unlock" && f.size() >= 2) {
                g_data.settings.slideToUnlock = f[1] == "1";
            } else if (kind == "power" && f.size() >= 2) {
                g_data.settings.poweredOn = f[1] == "1";
            } else if (kind == "volume" && f.size() >= 2) {
                g_data.settings.volume = std::clamp(ToInt(f[1], 7), 0, 10);
            } else if (kind == "silent" && f.size() >= 2) {
                g_data.settings.silent = f[1] == "1";
            } else if (kind == "store" && f.size() >= 3) {
                g_data.store[f[1]] = f[2];
            } else if (kind == "number" && f.size() >= 2) {
                g_data.settings.ownNumber = f[1];
            }
        }
        logfile::Line("phone: loaded %u contacts, %u messages, %u recent calls",
                      static_cast<unsigned>(g_data.contacts.size()),
                      static_cast<unsigned>(g_data.messages.size()),
                      static_cast<unsigned>(g_data.recents.size()));
    }
    if (g_data.settings.ownNumber.empty()) {
        g_data.settings.ownNumber = MakeNumber();
        Save();
    }
    SortContacts();
}

void Save() {
    if (g_path.empty()) {
        return;
    }
    // Write beside the file and swap it in, so a crash mid-write leaves the
    // old phone rather than half of a new one.
    const std::string temp = g_path + ".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            logfile::Line("phone: cannot write %s", temp.c_str());
            return;
        }
        const Settings& s = g_data.settings;
        out << "number\t" << Escape(s.ownNumber) << "\n";
        out << "wallpaper\t" << s.wallpaper << "\n";
        out << "unlock\t" << (s.slideToUnlock ? 1 : 0) << "\n";
        out << "power\t" << (s.poweredOn ? 1 : 0) << "\n";
        out << "volume\t" << s.volume << "\n";
        out << "silent\t" << (s.silent ? 1 : 0) << "\n";
        for (const Contact& c : g_data.contacts) {
            out << "contact\t" << Escape(c.name) << "\t" << Escape(c.number) << "\t" << Escape(c.picture) << "\n";
        }
        for (const Message& m : g_data.messages) {
            out << "message\t" << Escape(m.number) << "\t" << (m.outgoing ? 1 : 0) << "\t"
                << (m.delivered ? 1 : 0) << "\t" << (m.read ? 1 : 0) << "\t" << StampText(m.at)
                << "\t" << Escape(m.text) << "\n";
        }
        for (const auto& kv : g_data.store) {
            out << "store\t" << Escape(kv.first) << "\t" << Escape(kv.second) << "\n";
        }
        for (const Recent& r : g_data.recents) {
            out << "recent\t" << Escape(r.number) << "\t" << (r.outgoing ? 1 : 0) << "\t"
                << StampText(r.at) << "\n";
        }
    }
    if (!MoveFileExA(temp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logfile::Line("phone: cannot replace %s (%lu)", g_path.c_str(), GetLastError());
    }
}

int FindContact(const std::string& number) {
    for (size_t i = 0; i < g_data.contacts.size(); ++i) {
        if (g_data.contacts[i].number == number) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string NameFor(const std::string& number) {
    const int i = FindContact(number);
    if (i >= 0) return g_data.contacts[i].name;
    for (const Contact& c : g_data.builtIn) {
        if (c.number == number) return c.name;
    }
    return FormatNumber(number);
}

void SortContacts() {
    std::stable_sort(g_data.contacts.begin(), g_data.contacts.end(),
                     [](const Contact& a, const Contact& b) {
                         return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
                     });
}

Stamp Now() {
    Stamp s;
    s.day = std::clamp(static_cast<int>(*reinterpret_cast<const uint8_t*>(kClockDay)), 1, 7);
    s.hour = std::clamp(static_cast<int>(*reinterpret_cast<const uint8_t*>(kClockHours)), 0, 23);
    s.minute = std::clamp(static_cast<int>(*reinterpret_cast<const uint8_t*>(kClockMinutes)), 0, 59);
    return s;
}

std::string FormatTime(const Stamp& s) {
    const int h12 = s.hour % 12 == 0 ? 12 : s.hour % 12;
    char buf[16];
    snprintf(buf, sizeof buf, "%d:%02d %s", h12, s.minute, s.hour < 12 ? "AM" : "PM");
    return buf;
}

std::string DayName(int day) {
    static const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                        "Thursday", "Friday", "Saturday"};
    return kDays[std::clamp(day, 1, 7) - 1];
}

std::string FormatDayTime(const Stamp& s) {
    return DayName(s.day) + " " + FormatTime(s);
}

std::string FormatNumber(const std::string& digits) {
    return digits;
}

}  // namespace phone_data
