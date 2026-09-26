// Let's Get Ready To Bumble, ported from the game's GRAV mission script
// (main.scm mission 6).
//
// The bee starts at the bottom of a tall column of sky and climbs it: hold
// Space to flap, left and right to drift. Every flower is worth ten; the last
// one on a level ends it, adding the flowers collected and a hundredth of the
// time left. Thorns kill, and so does running out of the two minutes. Leaves
// are for landing on. A dead bee comes back as a ghost that floats to the
// nearest leaf, while lives last; every 665 points or so adds one.
//
// Every rule and number of play is the script's; the level's columns and
// where things sit on screen are laid out again for the upright screen.
// "Timed" steps scale by the game's time step
// (1.0 at 50 frames a second).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "arcade.h"
#include "games.h"
#include "phone_data.h"

namespace games {
namespace {

constexpr uint32_t kDim = 0xFF969696;
constexpr float kMid = arcade::kWidth * 0.5f;
constexpr float kScrollTop = 312.0f, kScrollBottom = 412.0f;
constexpr float kTableTop = 96.0f + 112.0f;
constexpr uint32_t kIcon = 0x96969696;  // (150, 150, 150, 150)
constexpr uint32_t kGold = 0xFFA88E33;  // (168, 142, 51), the script's text colour
const char kLetters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.";
constexpr float kNone = 999.0f;

// The level on the upright screen. The script's five columns, 80 apart from
// 160 to 480, are drawn in 72 apart from 136, so a flower and the leaf to its
// left fit across 448; the view is 672 tall instead of 448.
constexpr float kViewH = arcade::kHeight;
float Column(int i) { return 136.0f + i * 72.0f; }
// The rows of thorns along the floor and the ceiling.
const float kThornRow[4] = {56.0f, 168.0f, 280.0f, 392.0f};

struct Entry {
    std::string name;
    int score;
    int level;
};

std::vector<Entry> DefaultTable(std::mt19937& rng) {
    // The first slot is always "C..", the rest nine different initials drawn
    // from the staff the script picks from.
    static const char* const kPool[] = {"NF.", "IMY", "WIL", "CKR", "DBP", "DAV", "DOD", "SJL",
                                        "STE", "JUD", "KMB", "GSW", "DSW", "WDY", "GAZ", "WAZ",
                                        "KIN", "BEA", "BAX", "LOU", "JNO", "MYT", "DEF", "KHZ",
                                        "MEO", "GFW", "WRM", "A.W", "RIC"};
    std::vector<int> order(29);
    for (int i = 0; i < 29; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    std::vector<Entry> t{{"C..", 1000, 10}};
    for (int i = 1; i < 10; ++i) t.push_back({kPool[order[i - 1]], 1000 - i * 100, 10 - i});
    return t;
}

std::vector<Entry> LoadTable(std::mt19937& rng) {
    auto& store = phone_data::Get().store;
    auto it = store.find("bumble");
    std::vector<Entry> t;
    if (it != store.end()) {
        size_t p = 0;
        const std::string& s = it->second;
        while (p < s.size() && t.size() < 10) {
            const size_t a = s.find(':', p), b = s.find(':', a + 1), c = s.find(',', p);
            if (a == std::string::npos || b == std::string::npos) break;
            t.push_back({s.substr(p, 3), atoi(s.substr(a + 1, b - a - 1).c_str()),
                         atoi(s.substr(b + 1, c - b - 1).c_str())});
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    if (t.size() == 10) return t;
    t = DefaultTable(rng);
    return t;
}

void SaveTable(const std::vector<Entry>& t) {
    std::string s;
    for (const auto& e : t) {
        if (!s.empty()) s += ",";
        s += e.name + ":" + std::to_string(e.score) + ":" + std::to_string(e.level);
    }
    phone_data::Get().store["bumble"] = s;
    phone_data::Save();
}

bool Collide(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return std::fabs(ax - bx) * 2.0f < aw + bw && std::fabs(ay - by) * 2.0f < ah + bh;
}

class Bumble : public Game {
public:
    Bumble() : rng_(GetTickCount()) {
        const int d = arcade::Dictionary("ld_grav");
        auto t = [&](const char* n) { return arcade::Texture(d, n); };
        sky_ = t("sky"); beea_ = t("beea"); flwra_ = t("flwra"); ghost_ = t("ghost"); leaf_ = t("leaf");
        flwr_ = t("flwr"); thorn_ = t("thorn"); bee2_ = t("bee2"); bee1_ = t("bee1");
        hive_ = t("hive"); timer_ = t("timer"); logo_ = t("bumble"); playw_ = t("playw"); playy_ = t("playy");
        hiw_ = t("hiscorew"); hiy_ = t("hiscorey"); exitw_ = t("exitw"); exity_ = t("exity");
        table_ = LoadTable(rng_);
        if (phone_data::Get().store.find("bumble") == phone_data::Get().store.end()) SaveTable(table_);
        Reset();
    }

    bool Frame(const arcade::Pad& pad, unsigned now, float dtMs) override {
        now_ = now;
        step_ = dtMs / 20.0f;
        Sky();
        bool keep = true;
        if (state_ == State::Menu) keep = Menu(pad);
        else if (state_ == State::Playing) Play(pad);
        else Table(pad);
        prev_ = pad;
        return keep;
    }

private:
    enum class State { Menu, Playing, Table };
    enum class Bee { Dead = 0, Alive = 1, Ghost = 2 };
    enum class Menu3 { Play = 1, Scores = 2, Exit = 3 };
    enum class TableStage { GameOver, Check, Entry, Show };

    int RandomInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi - 1)(rng_); }

    arcade::TextStyle Gold(float sx = 1.0f, float sy = 2.0f) const {
        arcade::TextStyle s;
        s.face = sprite::Face::Menu;
        s.scaleX = sx;
        s.scaleY = sy;
        s.argb = kGold;
        s.shadow = false;
        return s;
    }

    void Sky() {
        for (float x : {128.0f, 384.0f}) {
            for (float y : {128.0f, 384.0f, 640.0f}) arcade::Sprite(sky_, x, y, 256, 256, kDim);
        }
    }

    // The script's angles have 180 as upright.
    void Rotated(uintptr_t tex, float x, float y, float w, float h, float angle, uint32_t argb) {
        arcade::SpriteRotated(tex, x, y, w, h, angle - 180.0f, argb);
    }

    // What GRAV_2062 sets up: a fresh game at the title screen.
    void Reset() {
        state_ = State::Menu;
        menu_ = Menu3::Play;
        nextLevel_ = true;
        score_ = 0;
        level_ = 0;
        lives_ = 2;
        bee_ = Bee::Alive;
        tilt_ = 180.0f;
        screenY_ = 4096.0f - kViewH;
        pickups_ = 0;
        speedX_ = speedY_ = 0.0f;
        lastBonus_ = 0;
        ground_ = false;
        menuHeld_ = true;
    }

    bool Menu(const arcade::Pad& pad) {
        if (menuHeld_ && !pad.up && !pad.down) menuHeld_ = false;
        arcade::Sprite(logo_, kMid, 260.0f, 256, 128, kDim);
        arcade::Sprite(menu_ == Menu3::Play ? playy_ : playw_, kMid, 416.0f, 64, 32, kDim);
        arcade::Sprite(menu_ == Menu3::Scores ? hiy_ : hiw_, kMid, 448.0f, 128, 32, kDim);
        arcade::Sprite(menu_ == Menu3::Exit ? exity_ : exitw_, kMid, 480.0f, 64, 32, kDim);
        if (!menuHeld_) {
            if (pad.up) { menu_ = menu_ == Menu3::Play ? Menu3::Exit : Menu3(int(menu_) - 1); menuHeld_ = true; }
            else if (pad.down) { menu_ = menu_ == Menu3::Exit ? Menu3::Play : Menu3(int(menu_) + 1); menuHeld_ = true; }
        }
        if (pad.cross && !prev_.cross) {
            if (menu_ == Menu3::Play) {
                state_ = State::Playing;
                nextLevel_ = true;
            } else if (menu_ == Menu3::Scores) {
                state_ = State::Table;
                stage_ = TableStage::Show;
                highlight_ = false;
                helpStart_ = now_;
            } else {
                return false;
            }
        }
        return true;
    }

    // --- A level --------------------------------------------------------------

    struct Thing { float x, y; };

    void BuildLevel() {
        const int count = std::min(50, (level_ + 1) * 10);
        thorns_.clear();
        flowers_.clear();
        leaves_.clear();
        for (int i = 0; i < count; ++i) {
            thorns_.push_back({Column(RandomInt(0, 5)), static_cast<float>(RandomInt(0, 30) * 128)});
        }
        for (int i = 0; i < count; ++i) {
            const float x = Column(RandomInt(0, 5));
            const float y = static_cast<float>(RandomInt(0, 32) * 128 + 64);
            flowers_.push_back({x, y});
            leaves_.push_back({x - 48.0f, y + 32.0f});
        }
        // Rows of thorns along the floor and the ceiling.
        for (float x : kThornRow) thorns_.push_back({x, 4096.0f});
        for (float x : kThornRow) thorns_.push_back({x, 0.0f});
        totalPickups_ = count;
        pickupsLeft_ = count;
    }

    // Put the bee on the leaf nearest the bottom of the level.
    void StartLevel() {
        BuildLevel();
        float best = 99999.9f;
        beeX_ = kMid;
        beeY_ = 4096.0f;
        float tx = beeX_, ty = beeY_;
        for (const auto& l : leaves_) {
            if (beeY_ > 3000.0f && 4100.0f > beeY_) {
                const float d = std::hypot(beeX_ - l.x, beeY_ - l.y);
                if (best > d) { best = d; tx = l.x; ty = l.y - 32.0f; }
            }
        }
        beeX_ = tx;
        beeY_ = ty;
        nextLevel_ = false;
        levelShownUntil_ = now_ + 1000;
        countdownStart_ = now_ + 1000;
    }

    // The ghost's way back: the nearest leaf on screen. With so few leaves
    // up the tall column there may be none in view; then the nearest of all,
    // rather than the ghost's old target (the top of the level, where it
    // floated off for good).
    void AimGhost() {
        float best = 99999.9f;
        for (int pass = 0; pass < 2 && best == 99999.9f; ++pass) {
            for (const auto& l : leaves_) {
                const float sy = l.y - screenY_;
                if (pass == 1 || (sy > -200.0f && kViewH + 152.0f > sy)) {
                    const float d = std::hypot(beeX_ - l.x, beeY_ - l.y);
                    if (best > d) { best = d; ghostX_ = l.x; ghostY_ = l.y - 32.0f; }
                }
            }
        }
        if (best == 99999.9f) { ghostX_ = beeX_; ghostY_ = beeY_; }
        ghostAt_ = now_;
        const float vx = ghostX_ - beeX_, vy = ghostY_ - beeY_;
        const float mag = std::max(0.001f, std::sqrt(vx * vx + vy * vy));
        speedX_ = 2.5f * vx / mag;
        speedY_ = 2.5f * vy / mag;
    }

    void Collisions(const arcade::Pad& pad) {
        for (auto& f : flowers_) {
            if (f.x == kNone || f.x == -kNone) continue;
            const float sy = f.y - screenY_;
            if (!(sy > -64.0f && kViewH + 32.0f > sy)) continue;
            if (!Collide(beeX_, beeY_, 32, 44, f.x, f.y, 32, 32)) continue;
            if (pickupsLeft_ != 1) {
                score_ += 10;
                ++pickups_;
                --pickupsLeft_;
            } else {
                // The last flower: the level is done.
                score_ += pickups_;
                score_ += countdown_ / 100;
                pickups_ = 0;
                ++level_;
                const int gained = score_ - lastBonus_;
                lastBonus_ = gained;
                if (gained > 665) ++lives_;
                nextLevel_ = true;
            }
            f.x = -kNone;
            return;
        }
        for (const auto& l : leaves_) {
            const float sy = l.y - screenY_;
            if (!(sy > -64.0f && kViewH + 32.0f > sy)) continue;
            if (Collide(beeX_, beeY_, 32, 44, l.x, l.y, 96, 32)) {
                if (l.y > beeY_ + 12.0f) {
                    if (!ground_ && (!pad.cross || 0.0f > speedY_) && !(l.x - 52.0f > beeX_) &&
                        !(beeX_ > l.x + 52.0f)) {
                        speedY_ *= -0.1f;
                        beeY_ = l.y - 38.0f;
                        ground_ = true;
                    }
                } else {
                    speedX_ *= -0.5f;
                    speedY_ *= -0.5f;
                    beeY_ += 4.0f;
                }
            } else {
                ground_ = false;
            }
        }
        for (const auto& t : thorns_) {
            const float sy = t.y - screenY_;
            if (!(sy > -64.0f && kViewH + 32.0f > sy)) continue;
            if (Collide(beeX_, beeY_, 32, 44, t.x, t.y, 128, 40)) bee_ = Bee::Dead;
        }
    }

    void Move(const arcade::Pad& pad) {
        beeX_ = std::clamp(beeX_, 50.0f, arcade::kWidth - 50.0f);
        if (pad.cross) {
            if (ground_) { ground_ = false; speedY_ += 1.0f * step_; }
            if (20.0f > speedY_) speedY_ += 0.1f * step_;
        }
        if (pad.square && !ground_ && speedY_ > 0.0f) speedY_ -= 0.05f * step_;
        speedX_ -= speedX_ / 16.0f * step_;
        float stick = 0.0f;
        if (pad.left) stick = -127.0f;
        if (pad.right) stick = 127.0f;
        speedX_ += stick / 512.0f * step_;
        beeX_ += speedX_ * step_;
        if (!ground_ && speedY_ > -20.0f) speedY_ -= 0.05f * step_;
        beeY_ -= speedY_ * step_;
    }

    void Tilt(const arcade::Pad& pad) {
        if (!pad.left && !pad.right) {
            if (tilt_ > 180.0f) tilt_ -= 2.0f;
            if (180.0f > tilt_) tilt_ += 2.0f;
        }
        if (pad.right && 210.0f > tilt_) { tilt_ += 4.0f; if (180.0f > tilt_) tilt_ += 4.0f; }
        if (pad.left && tilt_ > 150.0f) { tilt_ -= 4.0f; if (tilt_ > 180.0f) tilt_ -= 4.0f; }
    }

    void Scroll() {
        const float sy = beeY_ - screenY_;
        // The script keeps the bee between 200 and 300 of 448; the taller
        // view keeps it the same way round the middle.
        if (kScrollTop > sy) screenY_ -= kScrollTop - sy;
        if (sy > kScrollBottom) screenY_ += sy - kScrollBottom;
    }

    void DrawLevel(const arcade::Pad& pad) {
        for (const auto& t : thorns_) {
            const float y = t.y - screenY_;
            if (y > -64.0f && kViewH + 32.0f > y) arcade::Sprite(thorn_, t.x, y, 128, 64, kDim);
        }
        for (const auto& f : flowers_) {
            if (f.x == -kNone) continue;
            const float y = f.y - screenY_;
            if (y > -64.0f && kViewH + 32.0f > y) arcade::Sprite(flwr_, f.x, y, 32, 32, kDim);
        }
        for (const auto& l : leaves_) {
            const float y = l.y - screenY_;
            if (y > -64.0f && kViewH + 32.0f > y) arcade::Sprite(leaf_, l.x, y, 128, 32, kDim);
        }
        const float y = beeY_ - screenY_;
        if (bee_ == Bee::Alive) {
            if (!pad.cross) {
                Rotated(bee1_, beeX_, y, 48, 48, tilt_, kDim);
            } else {
                // Flapping: the two wing frames swap every few milliseconds.
                if (now_ - buzzAt_ > 10) { buzz_ = !buzz_; buzzAt_ = now_; }
                Rotated(buzz_ ? bee2_ : bee1_, beeX_, y, 48, 48, tilt_, kDim);
            }
        } else if (bee_ == Bee::Ghost) {
            Rotated(ghost_, beeX_, y, 48, 48, 180.0f, kDim);
            beeX_ += speedX_ * step_;
            beeY_ += speedY_ * step_;
        }
    }

    void Play(const arcade::Pad& pad) {
        if (bee_ == Bee::Alive) Tilt(pad);
        if (nextLevel_) StartLevel();
        if (levelShownUntil_ > now_) {
            arcade::Text(kMid, kViewH * 0.5f, "LEVEL " + std::to_string(level_ + 1), Gold());
            if (level_ == 0) arcade::Help("Space Flap~n~Left Right Drift~n~Shift Brake~n~Backspace Exit");
            return;
        }

        Scroll();
        if (bee_ == Bee::Alive) {
            Collisions(pad);
            if (nextLevel_) return;
            Move(pad);
        }
        DrawLevel(pad);

        if (bee_ == Bee::Dead && lives_ > 0) {
            --lives_;
            bee_ = Bee::Ghost;
            AimGhost();
        }
        // There, or near enough: a slow frame can step the ghost past its
        // little target box, so it is caught within a step, and never left
        // drifting longer than the trip could take.
        const float ghostStep = 2.5f * step_ + 4.0f;
        const bool ghostHome = std::fabs(beeX_ - ghostX_) <= ghostStep && std::fabs(beeY_ - ghostY_) <= ghostStep;
        if (bee_ == Bee::Ghost && (ghostHome || now_ - ghostAt_ > 15000)) {
            bee_ = Bee::Alive;
            tilt_ = 180.0f;
            beeX_ = ghostX_;
            beeY_ = ghostY_;
        }
        if (bee_ == Bee::Dead && lives_ == 0) {
            GameOver();
            return;
        }

        // The panel along the bottom: lives, flowers left, time; the score at
        // the top.
        constexpr float kPanelY = kViewH - 58.0f;
        arcade::Text(376.0f, kPanelY, std::to_string(lives_), Gold());
        Rotated(beea_, 376.0f, kPanelY, 44, 44, 180.0f, kIcon);
        Rotated(bee1_, 376.0f, kPanelY, 36, 36, 180.0f, kIcon);
        arcade::Text(72.0f, kPanelY, std::to_string(pickupsLeft_), Gold());
        Rotated(flwra_, 72.0f, kPanelY, 40, 40, 180.0f, kIcon);
        Rotated(flwr_, 72.0f, kPanelY, 32, 32, 180.0f, kIcon);
        const int secs = std::max(0, countdown_) / 1000;
        char time[16];
        snprintf(time, sizeof time, "%d:%02d", secs / 60, secs % 60);
        arcade::Text(kMid, kPanelY, time, Gold());
        Rotated(timer_, kMid, kPanelY, 40, 40, 180.0f, kIcon);
        arcade::Text(kMid, 70.0f, std::to_string(score_), Gold());
        Rotated(hive_, kMid, 70.0f, 40, 40, 180.0f, kIcon);

        const int elapsed = static_cast<int>(now_ - countdownStart_);
        if (120000 > elapsed) {
            countdown_ = 120000 - elapsed;
        } else if (bee_ == Bee::Alive) {
            if (lives_ > 0) {
                --lives_;
                nextLevel_ = true;
            } else {
                GameOver();
            }
        }
    }

    void GameOver() {
        state_ = State::Table;
        stage_ = TableStage::GameOver;
        gameOverAt_ = now_;
        bee_ = Bee::Dead;
    }

    // --- The table ------------------------------------------------------------

    void Table(const arcade::Pad& pad) {
        if (pad.triangle && stage_ != TableStage::GameOver) {
            Reset();
            return;
        }
        if (stage_ == TableStage::GameOver) {
            arcade::Text(kMid, 290.0f, "GAME OVER!", Gold(2.0f, 4.0f));
            if (now_ - gameOverAt_ > 5000) stage_ = TableStage::Check;
            return;
        }
        if (stage_ == TableStage::Check) {
            if (table_[9].score > score_) {
                stage_ = TableStage::Show;
                helpStart_ = now_;
            } else {
                int rank = 9;
                while (rank >= 0 && score_ > table_[rank].score) --rank;
                ++rank;
                for (int i = 9; i > rank; --i) table_[i] = table_[i - 1];
                table_[rank] = {"A..", score_, level_};
                rank_ = rank;
                letter_ = 10;
                write_ = 0;
                pressed_ = true;
                highlight_ = true;
                stage_ = TableStage::Entry;
            }
        }
        if (stage_ == TableStage::Entry) EnterName(pad);

        for (int i = 0; i < 10; ++i) {
            const float y = kTableTop + i * 32.0f;
            const bool mine = highlight_ && i == rank_;
            for (int j = 0; j < 3; ++j) {
                arcade::TextStyle s = Gold();
                if (mine) s.argb = (stage_ == TableStage::Entry && j == write_) ? 0xFFFF4000 : 0xFFFF8000;
                arcade::Text(kMid - 144.0f + j * 48.0f, y, std::string(1, table_[i].name[j]), s);
            }
            arcade::TextStyle s = Gold();
            s.align = sprite::Align::Right;
            if (mine) s.argb = 0xFFFF8000;
            arcade::Text(kMid + 144.0f, y, std::to_string(table_[i].score), s);
        }
        arcade::Text(kMid, kTableTop - 32.0f, "HI-SCORE", Gold());
        if (stage_ == TableStage::Show && 10000 > now_ - helpStart_) {
            arcade::Window(35.0f, 15.0f, 200.0f, 45.0f);
            arcade::TextStyle h;
            h.face = sprite::Face::Subtitles;
            h.scaleX = 0.5f;
            h.scaleY = 1.8f;
            h.argb = 0xFFE1E1E1;
            h.align = sprite::Align::Left;
            h.shadow = false;
            arcade::Text(40.0f, 20.0f, "Backspace Back", h);
        }
    }

    void EnterName(const arcade::Pad& pad) {
        if (!pad.up && !pad.down && !pad.left && !pad.right && !pad.cross) pressed_ = false;
        std::string& name = table_[rank_].name;
        name[write_] = kLetters[letter_];
        for (int j = write_ + 1; j < 3; ++j) name[j] = 'A';
        if (pressed_) return;
        if (pad.up || pad.right) { letter_ = 36 > letter_ ? letter_ + 1 : 0; pressed_ = true; }
        if (pad.down || pad.left) { letter_ = letter_ > 0 ? letter_ - 1 : 36; pressed_ = true; }
        if (pad.cross) {
            pressed_ = true;
            letter_ = 10;
            if (++write_ == 3) {
                SaveTable(table_);
                stage_ = TableStage::Show;
                helpStart_ = now_;
            }
        }
    }

    std::mt19937 rng_;
    uintptr_t sky_ = 0, beea_ = 0, flwra_ = 0, ghost_ = 0, leaf_ = 0, flwr_ = 0, thorn_ = 0,
              bee2_ = 0, bee1_ = 0, hive_ = 0, timer_ = 0, logo_ = 0, playw_ = 0, playy_ = 0, hiw_ = 0,
              hiy_ = 0, exitw_ = 0, exity_ = 0;

    unsigned now_ = 0;
    float step_ = 1.0f;
    arcade::Pad prev_{};

    State state_ = State::Menu;
    Menu3 menu_ = Menu3::Play;
    bool menuHeld_ = true;

    bool nextLevel_ = true;
    int score_ = 0, level_ = 0, lives_ = 2, lastBonus_ = 0;
    Bee bee_ = Bee::Alive;
    float tilt_ = 180.0f, screenY_ = 4096.0f - kViewH;
    float beeX_ = kMid, beeY_ = 4096.0f, speedX_ = 0.0f, speedY_ = 0.0f, ghostX_ = 0.0f, ghostY_ = 0.0f;
    bool ground_ = false, buzz_ = false;
    unsigned buzzAt_ = 0, ghostAt_ = 0;
    int pickups_ = 0, totalPickups_ = 0, pickupsLeft_ = 0, countdown_ = 120000;
    unsigned countdownStart_ = 0, levelShownUntil_ = 0;
    std::vector<Thing> thorns_, flowers_, leaves_;

    std::vector<Entry> table_;
    TableStage stage_ = TableStage::Show;
    unsigned gameOverAt_ = 0, helpStart_ = 0;
    int rank_ = -1, letter_ = 10, write_ = 0;
    bool pressed_ = false, highlight_ = false;
};

}  // namespace

std::unique_ptr<Game> MakeBumble() {
    return std::make_unique<Bumble>();
}

}  // namespace games
