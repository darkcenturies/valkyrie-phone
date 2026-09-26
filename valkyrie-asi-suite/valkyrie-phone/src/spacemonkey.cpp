// Go Go Space Monkey, ported from the game's SHTR mission script
// (main.scm mission 5).
//
// A side-scrolling shooter, played sideways as on the machine: the monkey
// flies right and the UFOs come in from the right. The playfield fills the
// upright screen top to bottom, zoomed in, and the view follows the monkey
// along it. Formations of eight UFOs weave in along one sine wave, each formation a little wider than the last. Every
// kill is a hundred; the eighth kill of a formation drops the next power-up
// in a fixed chain - faster fire, a second and third barrel, faster again,
// autofire. One enemy shot is ever on screen. Three monkeys a game.
//
// Every rule and number of play is the script's. "Timed" steps scale by the game's time step
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

constexpr float kPi = 3.14159265f;
constexpr uint32_t kDim = 0xFF969696;
constexpr float kMid = arcade::kWidth * 0.5f;
constexpr float kTableTop = 96.0f + 112.0f;

// The script's playfield is 640 wide and 448 high. Its 448 fills the upright
// screen's height, so a little under half its width shows at once; the view
// slides along it after the monkey, keeping him near the left with the
// field ahead of him in sight.
constexpr float kFieldScale = arcade::kHeight / 448.0f;
constexpr float kViewWidth = arcade::kWidth / kFieldScale;  // of the playfield
constexpr float kLead = 70.0f;  // how far in from the left edge the monkey is kept
float g_viewX = 0.0f;           // the playfield's x at the screen's left edge
float ScreenX(float x, float) { return (x - g_viewX) * kFieldScale; }
float ScreenY(float, float y) { return y * kFieldScale; }

// A sprite of the playfield, at the playfield's scale.
void FieldSprite(uintptr_t tex, float x, float y, float w, float h, uint32_t argb) {
    arcade::Sprite(tex, ScreenX(x, y), ScreenY(x, y), w * kFieldScale, h * kFieldScale, argb);
}
const char kLetters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.";

bool Collide(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return std::fabs(ax - bx) * 2.0f < aw + bw && std::fabs(ay - by) * 2.0f < ah + bh;
}

struct Entry { std::string name; int score; };

std::vector<Entry> LoadTable(std::mt19937& rng) {
    auto& store = phone_data::Get().store;
    auto it = store.find("spacemonkey");
    std::vector<Entry> t;
    if (it != store.end()) {
        size_t p = 0;
        const std::string& s = it->second;
        while (p < s.size() && t.size() < 10) {
            const size_t colon = s.find(':', p), comma = s.find(',', p);
            if (colon == std::string::npos) break;
            t.push_back({s.substr(p, 3), atoi(s.substr(colon + 1, comma - colon - 1).c_str())});
            if (comma == std::string::npos) break;
            p = comma + 1;
        }
    }
    if (t.size() == 10) return t;
    // "C.." first, then nine different initials from the staff the script
    // picks from - the same pool Bumble uses.
    static const char* const kPool[] = {"NF.", "IMY", "WIL", "CKR", "DBP", "DAV", "DOD", "SJL",
                                        "STE", "JUD", "KMB", "GSW", "DSW", "WDY", "GAZ", "WAZ",
                                        "KIN", "BEA", "BAX", "LOU", "JNO", "MYT", "DEF", "KHZ",
                                        "MEO", "GFW", "WRM", "A.W", "RIC"};
    std::vector<int> order(29);
    for (int i = 0; i < 29; ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    t = {{"C..", 1000}};
    for (int i = 1; i < 10; ++i) t.push_back({kPool[order[i - 1]], 1000 - i * 100});
    return t;
}

void SaveTable(const std::vector<Entry>& t) {
    std::string s;
    for (const auto& e : t) {
        if (!s.empty()) s += ",";
        s += e.name + ":" + std::to_string(e.score);
    }
    phone_data::Get().store["spacemonkey"] = s;
    phone_data::Save();
}

class SpaceMonkey : public Game {
public:
    SpaceMonkey() : rng_(GetTickCount()) {
        const int d = arcade::Dictionary("ld_shtr");
        auto t = [&](const char* n) { return arcade::Texture(d, n); };
        splash_ = t("splsh"); stars_ = t("bstars"); hiA_ = t("hi_a"); hiB_ = t("hi_b"); hiC_ = t("hi_c");
        unA_ = t("un_a"); unB_ = t("un_b"); unC_ = t("un_c"); ship_ = t("ship"); fire_ = t("fire");
        ufo_ = t("ufo"); nmef_ = t("nmef");
        ex_[0] = t("ex1"); ex_[1] = t("ex2"); ex_[2] = t("ex3"); ex_[3] = t("ex4");
        // The power-ups, in the numbering the script gives its sprites.
        pickups_[28] = t("pa"); pickups_[29] = t("pm2"); pickups_[30] = t("pm3");
        pickups_[31] = t("ps1"); pickups_[32] = t("ps2"); pickups_[33] = t("ps3");
        table_ = LoadTable(rng_);
        if (phone_data::Get().store.find("spacemonkey") == phone_data::Get().store.end()) SaveTable(table_);
        Reset();
    }

    bool Frame(const arcade::Pad& pad, unsigned now, float dtMs) override {
        now_ = now;
        step_ = dtMs / 20.0f;
        timerB_ += static_cast<unsigned>(dtMs);
        // One screen a frame, whichever was up when the frame began. The
        // view eases after the monkey, never past either end of the field.
        const int screen = frontEnd_;
        const float wanted = std::clamp(plyrX_ - kLead, 0.0f, 640.0f - kViewWidth);
        g_viewX += (wanted - g_viewX) * (1.0f - std::exp(-dtMs / 160.0f));
        sprite::BeginScissor(arcade::X(0.0f), arcade::Y(0.0f), arcade::X(arcade::kWidth),
                             arcade::Y(arcade::kHeight));
        Stars();
        if (screen == 0) Play(pad);
        sprite::Flush();
        sprite::EndScissor();
        bool keep = true;
        if (screen >= 1 && screen <= 3) keep = Menu(pad);
        else if (screen != 0) Table(pad);
        prev_ = pad;
        return keep;
    }

private:
    int RandomInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi - 1)(rng_); }
    float RandomFloat(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }

    arcade::TextStyle White(float sx = 1.0f, float sy = 2.0f) const {
        arcade::TextStyle s;
        s.face = sprite::Face::Menu;
        s.scaleX = sx;
        s.scaleY = sy;
        s.argb = 0xFFFFFFFF;
        s.shadow = false;
        return s;
    }

    // Two rows of stars scrolling left.
    void Stars() {
        float origin = background_;
        for (float y : {336.0f, 112.0f}) {
            for (int i = -1; i < 4; ++i) FieldSprite(stars_, origin + i * 256.0f, y, 256, 256, kDim);
            origin -= 1.0f * step_;
            if (0.0f > origin) origin += 256.0f;
        }
        background_ = origin;
    }

    // What SHTR_17446 sets up: a fresh game at the title screen.
    void Reset() {
        reloading_ = true;
        alive_ = 1;
        frontEnd_ = 1;
        score_ = 0;
        lives_ = 2;
        menuHeld_ = true;
        g_viewX = 0.0f;
        plyrX_ = 80.0f;
        plyrY_ = 224.0f;
        rangeStart_ = 1.0f;
        rangeEnd_ = 3.0f;
        shotOn_ = false;
        pickupCounter_ = 0;
        pickupState_ = 1;
        pickupCurrent_ = 31;
        reloadMs_ = 250;
        autofire_ = false;
        barrels_ = 0;
        pickupMove_ = 0;
        highlight_ = false;
        for (auto& e : enemies_) e = {};
        for (auto& s : enemyShots_) s = {};
        for (auto& p : shots_) p = {};
        currentShot_ = 1;
        amplitude_ = 0.0f;
        respawn_ = 0;
        formationReady_ = false;
        hiScore_ = table_[0].score;
    }

    bool Menu(const arcade::Pad& pad) {
        if (menuHeld_ && !pad.up && !pad.down) menuHeld_ = false;
        arcade::Sprite(splash_, kMid, 250.0f, 386.0f, 192.0f, kDim);
        arcade::Sprite(frontEnd_ == 1 ? hiA_ : unA_, kMid, 416.0f, 64, 32, kDim);
        arcade::Sprite(frontEnd_ == 2 ? hiB_ : unB_, kMid, 448.0f, 128, 32, kDim);
        arcade::Sprite(frontEnd_ == 3 ? hiC_ : unC_, kMid, 480.0f, 64, 32, kDim);
        if (pad.cross && !prev_.cross) {
            if (frontEnd_ == 1) {
                frontEnd_ = 0;
                reloadAt_ = now_;
                startedAt_ = now_;
                return true;
            }
            if (frontEnd_ == 2) {
                frontEnd_ = 4;
                onTable_ = 4;
                helpStart_ = now_;
                return true;
            }
            return false;
        }
        if (!menuHeld_) {
            if (pad.up) { frontEnd_ = frontEnd_ == 1 ? 3 : frontEnd_ - 1; menuHeld_ = true; }
            else if (pad.down) { frontEnd_ = frontEnd_ == 3 ? 1 : frontEnd_ + 1; menuHeld_ = true; }
        }
        return true;
    }

    // --- The game -------------------------------------------------------------

    struct Enemy { int alive; float x, y, speed; unsigned boomAt; };  // 1 flying, 2 exploding
    struct Shot { bool on; float x, y; };
    struct Volley { Shot straight, up, down; };

    void Explosion(unsigned since, float x, float y) {
        const unsigned t = now_ - since;
        if (t < 400) FieldSprite(ex_[t / 100], x, y, 32, 32, kDim);
    }

    void NewFormation() {
        enemyOrigin_ = RandomFloat(640.0f, 720.0f);
        enemies_[0].x = enemyOrigin_;
        cycle_ = RandomFloat(240.0f, 640.0f);
        genY_ = RandomFloat(80.0f, 368.0f);
        if (80.0f > genY_ - amplitude_) genY_ += amplitude_;
        if (80.0f > genY_ + amplitude_) genY_ -= amplitude_;
        const float speed = RandomFloat(rangeStart_, rangeEnd_);
        enemies_[0].speed = speed;
        formationSpeed_ = speed;
        // Faster formations follow each other more closely: 500 ms apart at
        // speed 1, 25 ms less for each half unit faster.
        const int band = std::clamp(static_cast<int>((speed - 1.0f) / 0.5f), 0, 13);
        respawnMs_ = 500 - band * 25;
        enemies_[0].alive = 1;
        formationReady_ = true;
    }

    void Play(const arcade::Pad& pad) {
        // A lost life: two explosions' worth of pause, then two seconds
        // blinking back in.
        if (alive_ == 0) {
            startAt_ = now_;
            if (lives_ > 0) {
                --lives_;
                rangeStart_ = 2.0f;
                rangeEnd_ = 4.0f;
                alive_ = 2;
            }
        }
        if (alive_ == 1) FieldSprite(ship_, plyrX_, plyrY_, 32, 32, kDim);
        if (alive_ == 1 || alive_ == 3) {
            if (pad.up) plyrY_ -= 6.0f * step_;
            if (pad.down) plyrY_ += 6.0f * step_;
            if (pad.left) plyrX_ -= 6.0f * step_;
            if (pad.right) plyrX_ += 6.0f * step_;
        }
        plyrX_ = std::clamp(plyrX_, 80.0f, 560.0f);
        plyrY_ = std::clamp(plyrY_, 80.0f, 368.0f);

        // Reloading: the fire button has to come up, and the reload time pass;
        // with autofire, either will do.
        if (reloading_) {
            const unsigned since = now_ - reloadAt_;
            if (!autofire_) {
                if (!pad.cross && since > static_cast<unsigned>(reloadMs_)) reloading_ = false;
            } else if (!pad.cross || since > static_cast<unsigned>(reloadMs_)) {
                reloading_ = false;
            }
        }
        if ((alive_ == 1 || alive_ == 3) && !reloading_ && pad.cross) {
            Volley& v = shots_[currentShot_];
            if (!v.straight.on) v.straight = {true, plyrX_ + 16.0f, plyrY_};
            if (barrels_ >= 1 && !v.up.on) v.up = {true, plyrX_ + 16.0f, plyrY_};
            if (barrels_ == 2 && !v.down.on) v.down = {true, plyrX_ + 16.0f, plyrY_};
            reloadAt_ = now_;
            reloading_ = true;
            // Slots 1 to 15: slot 0 is never moved or hit-tested.
            if (++currentShot_ == 16) currentShot_ = 1;
        }

        MoveShots();
        Kills();

        if (pickupState_ == 0) {
            FieldSprite(pickups_[pickupCurrent_], pickupX_, pickupY_, 32, 32, kDim);
            MovePickup();
            if (Collide(plyrX_, plyrY_, 32, 32, pickupX_, pickupY_, 32, 32)) TakePickup();
        }

        // Formations: the first ship waits half a second after the last
        // formation, the other seven follow at the formation's spacing.
        if (respawn_ == 0 && timerB_ > 500 && enemies_[0].alive == 0) {
            if (60.0f > amplitude_) amplitude_ += 5.0f;
            if (!formationReady_) NewFormation();
            timerB_ = 0;
            ++respawn_;
        }
        if (respawn_ > 0 && 8 > respawn_) {
            Enemy& e = enemies_[respawn_];
            if (timerB_ > static_cast<unsigned>(respawnMs_) && e.alive == 0) {
                e.x = enemyOrigin_;
                // The formation's own speed: the leader's slot is emptied
                // once it is shot, and a follower given its speed then would
                // never move, sitting off the edge where it cannot be
                // seen, and the next formation would never come.
                e.speed = formationSpeed_;
                e.alive = 1;
                timerB_ = 0;
                ++respawn_;
            }
        }
        if (respawn_ == 8) {
            bool any = false;
            for (const auto& e : enemies_) any |= e.alive != 0;
            if (!any) {
                pickupCounter_ = 0;
                formationReady_ = false;
                respawn_ = 0;
            }
        }

        FlyEnemies();
        for (auto& e : enemies_) {
            if (e.alive != 2) continue;
            Explosion(e.boomAt, e.x, e.y);
            if (now_ - e.boomAt > 499) e = {};
        }
        EnemyFire();

        if (alive_ == 2) {
            Explosion(startAt_, plyrX_, plyrY_);
            if (now_ - startAt_ > 499) Respawn();
        }
        if (alive_ == 3) {
            const unsigned t = now_ - startAt_;
            if ((t / 100) % 2 == 0 && t < 2000) FieldSprite(ship_, plyrX_, plyrY_, 32, 32, 0x96969696);
            if (t >= 2000) alive_ = 1;
        }

        if (score_ > hiScore_) hiScore_ = score_;
        // Along the top of the playfield.
        const float hudY = 12.0f;
        arcade::TextStyle hud = White(0.6f, 1.2f);
        hud.align = sprite::Align::Right;
        arcade::Text(arcade::kWidth - 16.0f, hudY, "LIVES " + std::to_string(lives_), hud);
        hud.align = sprite::Align::Left;
        arcade::Text(16.0f, hudY, "1UP " + std::to_string(score_), hud);
        hud.align = sprite::Align::Centre;
        arcade::Text(kMid, hudY, "HI-SCORE " + std::to_string(hiScore_), hud);
        if (now_ - startedAt_ < 4000) arcade::Help("Arrows Move~n~Space Shoot~n~Backspace Exit");

        if (alive_ == 0 && lives_ == 0) {
            frontEnd_ = 4;
            onTable_ = 0;
        }
        if (pad.triangle && !prev_.triangle) {
            frontEnd_ = 4;
            onTable_ = 0;
        }
    }

    void Respawn() {
        startAt_ = now_;
        plyrX_ = 80.0f;
        plyrY_ = 224.0f;
        alive_ = 3;
        pickupCounter_ = 0;
        pickupState_ = 1;
        pickupCurrent_ = 31;
        reloadMs_ = 250;
        autofire_ = false;
        barrels_ = 0;
        pickupMove_ = 0;
    }

    void MoveShots() {
        constexpr float kFireSpeed = 15.0f, kShotAngle = 10.0f;
        const float xAdd = kFireSpeed * std::cos(kShotAngle * kPi / 180.0f);
        const float yAdd = kFireSpeed * std::sin(kShotAngle * kPi / 180.0f);
        for (int i = 1; i < 16; ++i) {
            Volley& v = shots_[i];
            if (v.straight.on) {
                FieldSprite(fire_, v.straight.x, v.straight.y, 8, 8, kDim);
                v.straight.x += kFireSpeed * step_;
                if (v.straight.x > 639.0f) v.straight = {};
            }
            if (v.up.on) {
                FieldSprite(fire_, v.up.x, v.up.y, 8, 8, kDim);
                v.up.x += xAdd * step_;
                v.up.y -= yAdd * step_;
                if (v.up.x > 639.0f) v.up = {};
            }
            if (v.down.on) {
                FieldSprite(fire_, v.down.x, v.down.y, 8, 8, kDim);
                v.down.x += xAdd * step_;
                v.down.y += yAdd * step_;
                if (v.down.x > 639.0f) v.down = {};
            }
        }
    }

    void Kills() {
        for (auto& e : enemies_) {
            if (e.alive != 1) continue;
            for (int i = 1; i < 16 && e.alive == 1; ++i) {
                for (Shot* s : {&shots_[i].straight, &shots_[i].up, &shots_[i].down}) {
                    if (!s->on || !Collide(s->x, s->y, 8, 8, e.x, e.y, 32, 32)) continue;
                    score_ += 100;
                    e.alive = 2;
                    e.boomAt = now_;
                    *s = {};
                    if (pickupState_ == 1 && ++pickupCounter_ == 8) {
                        pickupX_ = e.x;
                        pickupY_ = e.y;
                        pickupState_ = 0;
                    } else if (pickupState_ != 1) {
                        ++pickupCounter_;
                    }
                    break;
                }
            }
            if (e.alive == 1 && alive_ == 1 && Collide(plyrX_, plyrY_, 32, 32, e.x, e.y, 32, 32)) {
                e.alive = 2;
                e.boomAt = now_;
                alive_ = 0;
            }
        }
    }

    void MovePickup() {
        // Drifting diagonally round the screen, turning at random off the edges.
        const int m = pickupMove_;
        if (m == 0) {
            if (368.0f > pickupY_ && pickupX_ > 80.0f) { pickupX_ -= 1.0f; pickupY_ += 1.0f; }
            else pickupMove_ = RandomInt(0, 4);
        } else if (m == 1) {
            if (pickupX_ > 80.0f && pickupY_ > 80.0f) { pickupX_ -= 1.0f; pickupY_ -= 1.0f; }
            else pickupMove_ = RandomInt(0, 4);
        } else if (m == 2) {
            if (pickupY_ > 80.0f && 560.0f > pickupX_) { pickupX_ += 1.0f; pickupY_ -= 1.0f; }
            else pickupMove_ = RandomInt(0, 4);
        } else {
            if (560.0f > pickupX_ && 368.0f > pickupY_) { pickupX_ += 1.0f; pickupY_ += 1.0f; }
            else pickupMove_ = RandomInt(0, 4);
        }
    }

    void TakePickup() {
        // Each one makes the formations a little faster, up to a point.
        if (6.5f > rangeEnd_) { rangeStart_ += 0.5f; rangeEnd_ += 0.5f; }
        pickupX_ = pickupY_ = 0.0f;
        pickupMove_ = 0;
        // The chain: PS1 fire faster, PM2 two barrels, PS2 faster, PM3 three
        // barrels, PS3 fastest, PA autofire - and then no more.
        switch (pickupCurrent_) {
            case 28: autofire_ = true; pickupState_ = 2; break;
            case 33: reloadMs_ = 50; pickupCurrent_ = 28; pickupState_ = 1; break;
            case 30: barrels_ = 2; pickupCurrent_ = 33; pickupState_ = 1; break;
            case 32: reloadMs_ = 100; pickupCurrent_ = 30; pickupState_ = 1; break;
            case 29: barrels_ = 1; pickupCurrent_ = 32; pickupState_ = 1; break;
            case 31: reloadMs_ = 150; pickupCurrent_ = 29; pickupState_ = 1; break;
        }
    }

    void FlyEnemies() {
        for (auto& e : enemies_) {
            if (e.alive != 1) continue;
            FieldSprite(ufo_, e.x, e.y, 32, 32, kDim);
            e.x -= e.speed * step_;
            e.y = std::sin(e.x / cycle_ * 360.0f * kPi / 180.0f) * amplitude_ + genY_;
            if (0.0f > e.x) {
                // Letting one through costs points - but only while holding
                // the table's top score.
                if (score_ > 24 && score_ == hiScore_) {
                    bool top = true;
                    for (const auto& t : table_) top &= score_ > t.score;
                    if (top) { hiScore_ -= 25; score_ -= 25; }
                }
                e = {};
            }
        }
    }

    void EnemyFire() {
        for (int i = 0; i < 8; ++i) {
            Enemy& e = enemies_[i];
            Shot& s = enemyShots_[i];
            if (e.x > 320.0f && !shotOn_ && e.alive == 1 && !s.on) {
                s = {true, e.x, e.y};
                shotOn_ = true;
                enemyShotSpeed_ = e.speed * 1.5f;
            }
            if (shotOn_ && s.on) {
                if (alive_ == 1 && Collide(plyrX_, plyrY_, 32, 32, s.x, s.y, 8, 8)) {
                    s = {};
                    shotOn_ = false;
                    alive_ = 0;
                    continue;
                }
                FieldSprite(nmef_, s.x, s.y, 8, 8, kDim);
                s.x -= enemyShotSpeed_ * step_;
                if (0.0f > s.x) {
                    s = {};
                    shotOn_ = false;
                }
            }
        }
    }

    // --- The table ------------------------------------------------------------

    void Table(const arcade::Pad& pad) {
        if (pad.triangle && !prev_.triangle && onTable_ != 1) {
            Reset();
            return;
        }
        if (onTable_ == 0) {
            gameOverAt_ = now_;
            onTable_ = 1;
        }
        if (onTable_ == 1) {
            arcade::Text(kMid, 290.0f, "GAME OVER!", White(2.0f, 4.0f));
            if (now_ - gameOverAt_ > 5000) onTable_ = 2;
            return;
        }
        if (onTable_ == 2) {
            if (table_[9].score > score_) {
                onTable_ = 4;
                helpStart_ = now_;
            } else {
                int rank = 9;
                while (rank >= 0 && score_ > table_[rank].score) --rank;
                ++rank;
                for (int i = 9; i > rank; --i) table_[i] = table_[i - 1];
                table_[rank] = {"A..", score_};
                rank_ = rank;
                letter_ = 10;
                write_ = 0;
                pressed_ = true;
                highlight_ = true;
                onTable_ = 3;
            }
        }
        if (onTable_ == 3) EnterName(pad);

        for (int i = 0; i < 10; ++i) {
            const float y = kTableTop + i * 32.0f;
            const bool mine = highlight_ && lives_ == 0 && i == rank_;
            for (int j = 0; j < 3; ++j) {
                arcade::TextStyle s = White();
                if (mine && (onTable_ != 3 || j == write_)) s.argb = 0xFF00FF00;
                arcade::Text(kMid - 144.0f + j * 48.0f, y, std::string(1, table_[i].name[j]), s);
            }
            arcade::TextStyle s = White();
            s.align = sprite::Align::Right;
            if (mine) s.argb = 0xFF00FF00;
            arcade::Text(kMid + 144.0f, y, std::to_string(table_[i].score), s);
        }
        arcade::Text(kMid, kTableTop - 32.0f, "HI-SCORE", White());
        if (onTable_ == 4 && 10000 > now_ - helpStart_) {
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
                onTable_ = 4;
                helpStart_ = now_;
            }
        }
    }

    std::mt19937 rng_;
    uintptr_t splash_ = 0, stars_ = 0, hiA_ = 0, hiB_ = 0, hiC_ = 0, unA_ = 0, unB_ = 0, unC_ = 0, ship_ = 0,
              fire_ = 0, ufo_ = 0, nmef_ = 0, ex_[4] = {}, pickups_[34] = {};

    unsigned now_ = 0, timerB_ = 0;
    float step_ = 1.0f, background_ = 0.0f;
    arcade::Pad prev_{};

    int frontEnd_ = 1, onTable_ = 4;
    bool menuHeld_ = true;

    int alive_ = 1, score_ = 0, lives_ = 2, hiScore_ = 1000;
    float plyrX_ = 80.0f, plyrY_ = 224.0f, rangeStart_ = 1.0f, rangeEnd_ = 3.0f;
    bool reloading_ = true, autofire_ = false, shotOn_ = false, formationReady_ = false;
    unsigned reloadAt_ = 0, startAt_ = 0, startedAt_ = 0;
    int reloadMs_ = 250, barrels_ = 0, currentShot_ = 1;
    Volley shots_[16] = {};
    Enemy enemies_[8] = {};
    Shot enemyShots_[8] = {};
    float enemyShotSpeed_ = 0.0f;
    int pickupCounter_ = 0, pickupState_ = 1, pickupCurrent_ = 31, pickupMove_ = 0;
    float pickupX_ = 0.0f, pickupY_ = 0.0f;
    float amplitude_ = 0.0f, enemyOrigin_ = 660.0f, cycle_ = 400.0f, genY_ = 224.0f, formationSpeed_ = 1.0f;
    int respawn_ = 0, respawnMs_ = 500;

    std::vector<Entry> table_;
    unsigned gameOverAt_ = 0, helpStart_ = 0;
    int rank_ = -1, letter_ = 10, write_ = 0;
    bool pressed_ = false, highlight_ = false;
};

}  // namespace

std::unique_ptr<Game> MakeSpaceMonkey() {
    return std::make_unique<SpaceMonkey>();
}

}  // namespace games
