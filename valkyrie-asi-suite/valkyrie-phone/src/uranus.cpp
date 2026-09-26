// They Crawled From Uranus, ported from the game's NONE mission script
// (main.scm mission 3).
//
// The ship runs round the rim of a ring and fires inwards. Waves of ten come
// out of the middle in one of three flight patterns and shoot back when
// they pass in front of the ship. Clearing a whole wave drops a warp token;
// three tokens, and the ship warps down the ring to the next level once the
// sky is clear. Each hit on the ship costs a fifth of its health, and three
// ships make a game.
//
// Every rule and number of play is the script's; only where things sit on
// screen is laid out again for the upright screen. "Timed" steps scale by the game's time step
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
constexpr uint32_t kBright = 0xFFDCDCDC;  // (220, 220, 220)
// The middle of the ring: the middle of the upright screen. The ring's 190
// radius fits across its 448 width, and the score, lives, health and level
// go in the corners above and below it.
constexpr float kCX = arcade::kWidth * 0.5f;
constexpr float kCY = arcade::kHeight * 0.5f;
constexpr float kTableLeft = 201.225f - 96.0f;
constexpr float kTableTop = 88.9168f + 112.0f;
const char kLetters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.";

float Sin(float d) { return std::sin(d * kPi / 180.0f); }
float Cos(float d) { return std::cos(d * kPi / 180.0f); }
float Wrap(float d) {
    d = std::fmod(d, 360.0f);
    return d < 0.0f ? d + 360.0f : d;
}
bool Collide(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return std::fabs(ax - bx) * 2.0f < aw + bw && std::fabs(ay - by) * 2.0f < ah + bh;
}

// The three flight patterns: for each of ten steps, how fast the enemy moves
// out from the middle, how fast it turns round it, and for how long.
struct Step { float out, turn; int ms; };
const Step kPatterns[3][10] = {
    {{3, 4, 300}, {2, 3, 200}, {1, 2, 400}, {0, -2, 600}, {-1, -4, 400},
     {-2, -6, 500}, {-3, -3, 200}, {-2, -2, 300}, {-1, -7, 400}, {-4, -6, 6000}},
    {{2, 0, 100}, {3, 3, 400}, {1, 4, 300}, {0, 5, 500}, {1, 12, 400},
     {3, 6, 200}, {-3, 3, 400}, {-3, 0, 400}, {2, -4, 400}, {-5, -6, 6000}},
    {{5, 4, 300}, {-3, 4, 300}, {6, 4, 300}, {-6, 4, 300}, {3, 4, 300},
     {-3, 4, 300}, {3, 4, 300}, {-3, 4, 300}, {3, 4, 300}, {-6, 4, 300}},
};
// Which way each of the eight pieces of a destroyed ship flies.
const float kDebrisX[8] = {0, 5, 5, -5, -5, 0, 5, -5};
const float kDebrisY[8] = {5, 5, 0, 0, -5, -5, -5, 5};

struct Entry { std::string name; int score; };

std::vector<Entry> LoadTable() {
    auto& store = phone_data::Get().store;
    auto it = store.find("uranus");
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
    return {{"ADZ", 250000}, {"CAN", 100000}, {"IMY", 75000}, {"WIL", 50000}, {"DBP", 25000},
            {"DAV", 10000},  {"DOD", 7500},   {"NF.", 5000},  {"KMB", 2500},  {"SJL", 1000}};
}

void SaveTable(const std::vector<Entry>& t) {
    std::string s;
    for (const auto& e : t) {
        if (!s.empty()) s += ",";
        s += e.name + ":" + std::to_string(e.score);
    }
    phone_data::Get().store["uranus"] = s;
    phone_data::Save();
}

class Uranus : public Game {
public:
    Uranus() : rng_(GetTickCount()) {
        const int d = arcade::Dictionary("LD_NONE");
        auto t = [&](const char* n) { return arcade::Texture(d, n); };
        ship_ = t("ship"); ship2_ = t("ship2"); ship3_ = t("ship3"); shoot_ = t("shoot");
        light_ = t("light"); force_ = t("force"); warp_ = t("warp"); shpnorm_ = t("shpnorm");
        shpwarp_ = t("shpwarp"); title_ = t("title");
        char name[16];
        for (int i = 0; i < 12; ++i) {
            snprintf(name, sizeof name, "explm%02d", i + 1);
            expl_[i] = t(name);
        }
        for (auto& s : stars_) { s.r = RandomFloat(0.0f, 300.0f); s.a = RandomFloat(0.0f, 360.0f); }
        table_ = LoadTable();
    }

    bool Frame(const arcade::Pad& pad, unsigned now, float dtMs) override {
        now_ = now;
        step_ = dtMs / 20.0f;
        arcade::Box(kCX, kCY, arcade::kWidth, arcade::kHeight, 0xFF000000);
        bool keep = true;
        if (mode_ == Mode::Menu) keep = Menu(pad);
        else if (mode_ == Mode::Play) Play(pad);
        else Scores(pad);
        prev_ = pad;
        return keep;
    }

private:
    enum class Mode { Menu, Play, Scores };

    int RandomInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi - 1)(rng_); }
    float RandomFloat(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }

    // The script's angles have 180 as upright.
    void Rotated(uintptr_t tex, float x, float y, float w, float h, float angle, uint32_t argb) {
        arcade::SpriteRotated(tex, x, y, w, h, angle - 180.0f, argb);
    }

    arcade::TextStyle Text(float sx = 1.0f, float sy = 3.0f) const {
        arcade::TextStyle s;
        s.face = sprite::Face::Menu;
        s.scaleX = sx;
        s.scaleY = sy;
        s.argb = 0xFFB4B4B4;
        s.shadow = false;
        return s;
    }

    // Sixty stars flying out of the middle.
    void Stars(float speed, bool timed) {
        for (auto& s : stars_) {
            s.r += timed ? speed / 2.0f * step_ : speed;
            const float x = Cos(s.a) * s.r + kCX, y = Sin(s.a) * s.r + kCY;
            arcade::Box(x, y, 1.0f, 1.0f, 0xFFFFFFFF);
            if (x > arcade::kWidth || 0.0f > x || 0.0f > y || y > arcade::kHeight) {
                s.r = 10.0f;
                s.a = RandomFloat(0.0f, 360.0f);
            }
        }
    }

    // --- The title screen ---------------------------------------------------

    bool Menu(const arcade::Pad& pad) {
        if (pad.up && !prev_.up && menu_ > 0) --menu_;
        if (pad.down && !prev_.down && menu_ < 2) ++menu_;
        const char* const items[3] = {"PLAY", "HI-SCORE", "QUIT"};
        for (int i = 0; i < 3; ++i) {
            arcade::TextStyle s = Text();
            s.argb = menu_ == i ? 0xFFFF0000 : 0xFFFFFFFF;
            arcade::Text(kCX, 380.0f + i * 50.0f, items[i], s);
        }
        Stars(8.0f, true);
        arcade::Sprite(title_, kCX, 230.0f, 256.0f, 128.0f, kBright);
        if (pad.cross && !prev_.cross) {
            if (menu_ == 0) StartGame();
            else if (menu_ == 1) OpenScores(false);
            else return false;
        }
        return true;
    }

    // --- The game -------------------------------------------------------------

    struct Shot { bool on; float angle, radius, speed, heading, x, y; };
    struct Enemy {
        bool on;
        float angle, turn, out, radius, x, y, px, py;
        int step;
        unsigned next;
    };
    struct Boom { int frame; float x, y; };  // 0 free, 1 new, then 6..16
    struct Bullet { bool on; float angle, radius, speed, x, y; };

    void StartGame() {
        mode_ = Mode::Play;
        angle_ = 90.0f;
        for (auto& s : shots_) s = {};
        for (auto& e : enemies_) e = {};
        for (auto& b : booms_) b = {};
        for (auto& b : bullets_) b = {};
        for (auto& d : debris_) d = {};
        score_ = 0;
        spawnAt_ = 0;
        dying_ = 0;
        blinkUntil_ = 0;
        alpha_ = 255;
        alphaStep_ = -50;
        quit_ = false;
        health_ = 100;
        shownHealth_ = 100;
        glow_ = 0;
        glowAlpha_ = 0;
        glowStep_ = 100;
        lives_ = 3;
        kills_ = 0;
        bonus_ = 0;
        bonusUntil_ = 0;
        tokens_ = 0;
        tokenOn_ = false;
        warp_state_ = 0;
        level_ = 1;
        radius_ = 190.0f;
        starSpeed_ = 8.0f;
        waveAt_ = 0;
        toSpawn_ = 0;
        fireArmed_ = true;
        startedAt_ = now_;
    }

    void Explode(float x, float y) {
        for (auto& b : booms_) {
            if (b.frame == 0) { b = {1, x, y}; return; }
        }
    }

    void Play(const arcade::Pad& pad) {
        Stars(starSpeed_, true);
        float shipX = 0.0f, shipY = 0.0f, boxW = 0.0f, boxH = 0.0f;

        if (dying_ == 0) {
            if (2 > warp_state_) {
                if (pad.right) angle_ = Wrap(angle_ - 1.325f * step_);
                else if (pad.left) angle_ = Wrap(angle_ + 1.325f * step_);
            }
            boxW = std::fabs(Sin(angle_)) * 22.0f + 26.0f;
            boxH = std::fabs(Cos(angle_)) * 22.0f + 26.0f;
            if (warp_state_ == 2) {
                // Down the ring, fast...
                radius_ -= 1.0f * step_;
                starSpeed_ = 16.0f;
                if (20.0f > radius_) warp_state_ = 3;
            } else if (warp_state_ == 3) {
                // ...and out again at the next level.
                radius_ += 1.0f * step_;
                if (radius_ > 190.0f) {
                    radius_ = 190.0f;
                    tokens_ = 0;
                    ++level_;
                    warp_state_ = 0;
                }
            } else {
                starSpeed_ = 8.0f;
                radius_ = 190.0f;
            }
            shipX = Cos(angle_) * radius_ + kCX;
            shipY = Sin(angle_) * radius_ + kCY;
            if (blinkUntil_ > now_) {
                alpha_ += alphaStep_;
                if (alpha_ > 255) { alpha_ = 255; alphaStep_ = -50; }
                if (0 > alpha_) { alpha_ = 0; alphaStep_ = 50; }
            } else {
                alpha_ = 255;
            }
            const float h = radius_ / 5.9375f;
            const uint32_t argb = (static_cast<uint32_t>(alpha_) << 24) | 0x00DCDCDC;
            Rotated(2 > warp_state_ ? shpnorm_ : shpwarp_, shipX, shipY, h * 2.0f, h, angle_ + 90.0f, argb);
            if (2 > warp_state_) {
                if (pad.circle) {
                    if (fireArmed_) {
                        for (auto& s : shots_) {
                            if (!s.on) { s = {true, angle_, 190.0f, -12.0f, angle_ + 90.0f, 0, 0}; break; }
                        }
                        fireArmed_ = false;
                    }
                } else {
                    fireArmed_ = true;
                }
            }
        } else if (dying_ == 1) {
            for (int k = 0; k < 8; ++k) debris_[k] = {1, shipX_, shipY_};
            dying_ = 2;
        } else if (dying_ == 2) {
            int alive = 0;
            for (int k = 0; k < 8; ++k) {
                Boom& d = debris_[k];
                if (d.frame == 0) continue;
                if (d.frame == 1) d.frame = 5;
                d.x += kDebrisX[k];
                d.y += kDebrisY[k];
                if (++d.frame > 16) { d.frame = 0; continue; }
                arcade::Sprite(expl_[d.frame - 5], d.x, d.y, 32, 32, kBright);
                alive += d.frame;
            }
            if (alive == 0) {
                angle_ = 90.0f;
                blinkUntil_ = now_ + 2500;
                glow_ = 0;
                glowAlpha_ = 0;
                glowStep_ = 100;
                shownHealth_ = 100;
                if (--lives_ > 0) {
                    dying_ = 0;
                    health_ = 100;
                } else {
                    overUntil_ = now_ + 3000;
                    dying_ = 3;
                }
            }
        } else if (dying_ == 3) {
            if (overUntil_ > now_) arcade::Text(kCX, 100.0f, "GAME OVER!", Text());
            else quit_ = true;
        }
        if (dying_ == 0) { shipX_ = shipX; shipY_ = shipY; }

        // The player's shots, falling in towards the middle.
        for (auto& s : shots_) {
            if (!s.on) continue;
            s.radius += s.speed / 2.0f * step_;
            s.x = Cos(s.angle) * s.radius + kCX;
            s.y = Sin(s.angle) * s.radius + kCY;
            Rotated(shoot_, s.x, s.y, 8, 8, s.heading, 0xFF34B7C3);
            if (10.0f > s.radius) s.on = false;
        }

        // A new wave every eight seconds, while not warping.
        if (warp_state_ == 0) {
            if (now_ > waveAt_) {
                waveAngle_ = RandomFloat(0.0f, 360.0f);
                pattern_ = RandomInt(0, 3);
                kills_ = 0;
                toSpawn_ = 10;
                waveAt_ = now_ + 8000;
            }
        } else {
            waveAt_ = now_ + 1000;
        }
        if (toSpawn_ > 0 && now_ > spawnAt_) {
            for (auto& e : enemies_) {
                if (!e.on) {
                    e = {true, waveAngle_, 5.0f, 3.0f, 10.0f, kCX, kCY, kCX, kCY, 0, now_ + 500};
                    spawnAt_ = now_ + 200;
                    --toSpawn_;
                    break;
                }
            }
        }

        bool anyEnemy = false;
        for (auto& e : enemies_) {
            if (!e.on) continue;
            if (10 > e.step && now_ > e.next) {
                const Step& st = kPatterns[pattern_][e.step];
                e.out = st.out;
                e.turn = st.turn;
                e.next = now_ + st.ms;
                ++e.step;
            }
            anyEnemy = true;
            e.radius += e.out / 2.0f * step_;
            e.angle = Wrap(e.angle + e.turn / 2.0f * step_);
            e.px = e.x;
            e.py = e.y;
            e.x = Cos(e.angle) * e.radius + kCX;
            e.y = Sin(e.angle) * e.radius + kCY;
            const float size = e.radius / 5.0f;
            // get_heading_from_vector_2d: the heading the enemy is moving in.
            const float heading = std::atan2(-(e.x - e.px), e.y - e.py) * 180.0f / kPi;
            const uintptr_t tex = pattern_ == 0 ? ship_ : pattern_ == 1 ? ship2_ : ship3_;
            Rotated(tex, e.x, e.y, size, size, heading, kBright);

            // Passing in front of the ship, it may fire: more often each level.
            // (Measured round the ring, so a ship at 355 sees one at 3.)
            if (std::fabs(Wrap(e.angle - angle_ + 180.0f) - 180.0f) < 10.0f) {
                const int odds = std::max(4, 18 - level_);
                if (RandomInt(0, odds) == 0) {
                    for (auto& b : bullets_) {
                        if (!b.on) { b = {true, e.angle, e.radius, 6.0f, 0, 0}; break; }
                    }
                }
            }
            if (e.radius > 320.0f || 10.0f > e.radius) e.on = false;
            for (auto& s : shots_) {
                if (s.on && e.on && Collide(s.x, s.y, 12, 12, e.x, e.y, size, size)) {
                    e.on = false;
                    s.on = false;
                    if (++kills_ == 10) {
                        // The whole wave: a bonus, and a warp token where the
                        // last one fell.
                        bonusX_ = e.x;
                        bonusY_ = e.y;
                        bonusUntil_ = now_ + 1500;
                        tokenAngle_ = e.angle;
                        tokenRadius_ = e.radius;
                        tokenOn_ = true;
                        bonus_ = 100 * level_;
                        score_ += bonus_;
                    } else {
                        score_ += 10 * level_;
                    }
                    Explode(e.x, e.y);
                    break;
                }
            }
        }
        if (warp_state_ == 1 && !anyEnemy) warp_state_ = 2;

        // Enemy fire, flying outwards.
        for (auto& b : bullets_) {
            if (!b.on) continue;
            b.radius += b.speed / 2.0f * step_;
            b.x = Cos(b.angle) * b.radius + kCX;
            b.y = Sin(b.angle) * b.radius + kCY;
            arcade::Sprite(light_, b.x, b.y, 8, 8, 0xFFF40000);
            if (!Collide(b.x, b.y, 8, 8, kCX, kCY, arcade::kWidth, arcade::kHeight)) b.on = false;
            if (dying_ == 0 && b.on && Collide(b.x, b.y, 8, 8, shipX_, shipY_, boxW, boxH)) {
                if (now_ > blinkUntil_) health_ -= 20;
                b.on = false;
                Explode(b.x, b.y);
            }
        }

        // The shield flashes once for each hit taken.
        if (dying_ == 0 && glow_ > 0) {
            if (glow_ > 1) { glowStep_ = 100; --glow_; }
            glowAlpha_ += glowStep_;
            if (glowAlpha_ > 255) { glowAlpha_ = 255; glowStep_ = -20; }
            if (0 > glowAlpha_) { glowAlpha_ = 0; glowStep_ = 100; glow_ = 0; }
            Rotated(force_, shipX_, shipY_, 64, 32, angle_ + 90.0f,
                    (static_cast<uint32_t>(glowAlpha_) << 24) | 0x00DCDCDC);
        }

        for (auto& b : booms_) {
            if (b.frame == 0) continue;
            if (b.frame == 1) b.frame = 5;
            if (++b.frame > 16) { b.frame = 0; continue; }
            arcade::Sprite(expl_[b.frame - 5], b.x, b.y, 32, 32, kBright);
        }

        if (tokenOn_) {
            tokenRadius_ += 1.0f * step_;
            tokenAngle_ = Wrap(tokenAngle_);
            const float x = Cos(tokenAngle_) * tokenRadius_ + kCX;
            const float y = Sin(tokenAngle_) * tokenRadius_ + kCY;
            arcade::Sprite(warp_, x, y, 16, 16, 0xFFFFFFFF);
            if (dying_ == 0 && Collide(x, y, 16, 16, shipX_, shipY_, boxW, boxH)) {
                if (++tokens_ == 3) warp_state_ = 1;
                tokenOn_ = false;
            }
            if (!Collide(x, y, 16, 16, kCX, kCY, arcade::kWidth, arcade::kHeight)) tokenOn_ = false;
        }

        if (0 >= health_) {
            health_ = 0;
            if (dying_ == 0) dying_ = 1;
        }
        if (shownHealth_ > health_) {
            shownHealth_ = health_;
            ++glow_;
        }

        arcade::TextStyle hud = Text(0.6f, 2.6f);
        constexpr float kLeft = 80.0f, kRight = arcade::kWidth - 80.0f;
        constexpr float kBottom = arcade::kHeight - 100.0f;
        arcade::Text(kLeft, 45.0f, "SCORE", hud);
        arcade::Text(kLeft, 70.0f, std::to_string(score_), hud);
        arcade::Text(kRight, 45.0f, "LIVES", hud);
        arcade::Sprite(shpnorm_, kRight - 22.7471f, 84.2723f, 48.0f, 24.0f, kBright);
        arcade::TextStyle left = hud;
        left.align = sprite::Align::Left;
        arcade::Text(kRight + 4.7283f, 70.0f, "x " + std::to_string(lives_), left);
        arcade::Text(kRight, kBottom, "HEALTH", hud);
        arcade::Text(kRight, kBottom + 25.0f, std::to_string(health_) + "%", hud);
        arcade::Text(kLeft, kBottom, "LEVEL", hud);
        arcade::Text(kLeft, kBottom + 25.0f, std::to_string(level_), hud);
        if (bonusUntil_ > now_) {
            arcade::TextStyle b = Text(0.5f, 1.5f);
            b.argb = 0xFF00B4B4;
            arcade::Text(bonusX_, bonusY_, std::to_string(bonus_), b);
        }
        if (dying_ == 0 && warp_state_ > 0) arcade::Text(kCX, 100.0f, "WARPING", Text());
        if (now_ - startedAt_ < 4000) arcade::Help("Left Right Move~n~Click Shoot~n~Backspace Exit");

        if (pad.triangle) quit_ = true;
        if (quit_) {
            if (score_ > table_[9].score) OpenScores(true);
            else { mode_ = Mode::Menu; }
        }
    }

    // --- The high-score table -------------------------------------------------

    void OpenScores(bool entering) {
        mode_ = Mode::Scores;
        editRow_ = -1;
        if (entering) {
            for (int i = 9; i >= 0 && score_ > table_[i].score; --i) {
                if (i != 9) table_[i + 1] = table_[i];
                table_[i] = {"...", score_};
                editRow_ = i;
            }
            letter_ = 10;
            letterPos_ = 0;
            nextUp_ = nextDown_ = 0;
        }
        crossArmed_ = true;
    }

    void Scores(const arcade::Pad& pad) {
        if (editRow_ >= 0) {
            std::string& name = table_[editRow_].name;
            name[letterPos_] = kLetters[letter_];
            for (int j = letterPos_ + 1; j < 3; ++j) name[j] = '.';
            if (pad.up) {
                if (now_ > nextUp_) { letter_ = 36 > letter_ ? letter_ + 1 : 0; nextUp_ = now_ + 250; }
            } else {
                nextUp_ = 0;
            }
            if (pad.down) {
                if (now_ > nextDown_) { letter_ = letter_ > 0 ? letter_ - 1 : 36; nextDown_ = now_ + 250; }
            } else {
                nextDown_ = 0;
            }
            if (pad.cross) {
                if (!crossArmed_) {
                    crossArmed_ = true;
                    if (++letterPos_ == 3) {
                        editRow_ = -1;
                        SaveTable(table_);
                    }
                }
            } else {
                crossArmed_ = false;
            }
        }
        // The table moves the stars on by a fixed step, not a timed one.
        Stars(8.0f, false);
        arcade::TextStyle title = Text();
        title.face = sprite::Face::Subtitles;
        arcade::Text(kCX + 16.4137f, kTableTop - 30.6736f, "HI-SCORE", title);
        float y = kTableTop;
        for (int i = 0; i < 10; ++i) {
            arcade::TextStyle row = Text();
            row.face = sprite::Face::Subtitles;
            row.align = sprite::Align::Left;
            if (i == editRow_) row.argb = 0xFF009B00;
            float x = kTableLeft;
            for (int j = 0; j < 3; ++j) {
                const char c = table_[i].name[j];
                const float nudge = (c == '1' || c == 'I') ? 8.0f : 0.0f;
                arcade::Text(x + nudge, y, std::string(1, c), row);
                x += 29.78f;
            }
            row.align = sprite::Align::Right;
            arcade::Text(kTableLeft + 260.0f, y, std::to_string(table_[i].score), row);
            y += 28.5713f;
        }
        if (editRow_ < 0) {
            if (pad.triangle || pad.square || pad.circle) mode_ = Mode::Menu;
            if (pad.cross) {
                if (!crossArmed_) mode_ = Mode::Menu;
            } else {
                crossArmed_ = false;
            }
        }
    }

    std::mt19937 rng_;
    uintptr_t ship_ = 0, ship2_ = 0, ship3_ = 0, shoot_ = 0, light_ = 0, force_ = 0, warp_ = 0,
              shpnorm_ = 0, shpwarp_ = 0, title_ = 0, expl_[12] = {};
    struct Star { float r, a; } stars_[60] = {};

    Mode mode_ = Mode::Menu;
    int menu_ = 0;
    arcade::Pad prev_{};
    unsigned now_ = 0;
    float step_ = 1.0f;

    float angle_ = 90.0f, radius_ = 190.0f, starSpeed_ = 8.0f, shipX_ = kCX, shipY_ = kCY + 190.0f;
    Shot shots_[10] = {};
    Enemy enemies_[10] = {};
    Boom booms_[10] = {};
    Boom debris_[8] = {};
    Bullet bullets_[10] = {};
    int score_ = 0, dying_ = 0, alpha_ = 255, alphaStep_ = -50, health_ = 100, shownHealth_ = 100;
    int glow_ = 0, glowAlpha_ = 0, glowStep_ = 100, lives_ = 3, kills_ = 0, bonus_ = 0, tokens_ = 0;
    int warp_state_ = 0, level_ = 1, pattern_ = 0, toSpawn_ = 0;
    unsigned spawnAt_ = 0, blinkUntil_ = 0, bonusUntil_ = 0, waveAt_ = 0, overUntil_ = 0, startedAt_ = 0;
    float waveAngle_ = 0.0f, bonusX_ = 0.0f, bonusY_ = 0.0f, tokenAngle_ = 0.0f, tokenRadius_ = 0.0f;
    bool tokenOn_ = false, quit_ = false, fireArmed_ = true;

    std::vector<Entry> table_;
    int editRow_ = -1, letter_ = 10, letterPos_ = 0;
    unsigned nextUp_ = 0, nextDown_ = 0;
    bool crossArmed_ = false;
};

}  // namespace

std::unique_ptr<Game> MakeUranus() {
    return std::make_unique<Uranus>();
}

}  // namespace games
