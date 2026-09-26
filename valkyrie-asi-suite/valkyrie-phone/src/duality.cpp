// Duality, ported from the game's DUAL mission script (main.scm mission 4).
//
// The ship sits in the middle of the screen and the universe moves round it.
// Five dark bodies pull it in and hurt it; five light bodies push it away and
// heal it. Shooting a dark body is worth ten points, shooting a light one
// costs ten; the small light and dark pickups are five either way and refill
// the power that thrust, strafing and the gun all draw on. A positive score
// goes on the light table, a negative one on the dark table.
//
// Every rule and number of play here is the script's; only where things sit
// on screen is laid out again for the phone's upright screen. Where the script says "timed" it scales
// by the game's time step (1.0 at 50 frames a second); where it multiplies by
// its own frame time it uses seconds divided by 1.5.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "arcade.h"
#include "games.h"
#include "phone_data.h"

namespace games {
namespace {

constexpr float kPi = 3.14159265f;
constexpr uint32_t kDim = 0xFF969696;    // (150, 150, 150)
constexpr uint32_t kFull = 0xFFFFFFFF;
constexpr uint32_t kMenuGrey = 0xFFB4B4B4;  // the script's text colour, 180
// The middle of the screen, where the ship always is.
constexpr float kCentreX = arcade::kWidth * 0.5f;
constexpr float kCentreY = arcade::kHeight * 0.5f;
// Where the high-score table starts: its script position, centred.
constexpr float kTableLeft = 201.225f - 96.0f;
constexpr float kTableTop = 88.9168f + 112.0f;

float Sin(float deg) { return std::sin(deg * kPi / 180.0f); }
float Cos(float deg) { return std::cos(deg * kPi / 180.0f); }

bool Collide(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return std::fabs(ax - bx) * 2.0f < aw + bw && std::fabs(ay - by) * 2.0f < ah + bh;
}

// The letters a name is spelled from, in the script's order: DUAL_0 to
// DUAL_9, DUAL_AA to DUAL_Z, and DUAL_FS.
const char kLetters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.";
constexpr int kLetterCount = 37;

struct Entry {
    std::string name;
    int score;
};

// The tables as the script first fills them in - Rockstar's own initials.
std::vector<Entry> DefaultTable(bool light) {
    if (light) {
        return {{"ADZ", 10000}, {"CAN", 7500}, {"IMY", 5000}, {"WIL", 2500}, {"DBP", 1000},
                {"DAV", 750},   {"DOD", 500},  {"NF.", 250},  {"KMB", 175},  {"SJL", 100}};
    }
    return {{"DJ.", -10000}, {"KRY", -7500}, {"SIS", -5000}, {"WDY", -2500}, {"DAN", -1000},
            {"LRG", -750},   {"NON", -500},  {"GAZ", -250},  {"101", -175},  {"JUD", -100}};
}

std::vector<Entry> LoadTable(bool light) {
    auto& store = phone_data::Get().store;
    auto it = store.find(light ? "duality.light" : "duality.dark");
    std::vector<Entry> t;
    if (it != store.end()) {
        // name:score,name:score,...
        size_t p = 0;
        const std::string& s = it->second;
        while (p < s.size() && t.size() < 10) {
            const size_t colon = s.find(':', p), comma = s.find(',', p);
            if (colon == std::string::npos) break;
            const std::string name = s.substr(p, colon - p);
            const int score = atoi(s.substr(colon + 1, comma - colon - 1).c_str());
            t.push_back({name.substr(0, 3), score});
            if (comma == std::string::npos) break;
            p = comma + 1;
        }
    }
    return t.size() == 10 ? t : DefaultTable(light);
}

void SaveTable(bool light, const std::vector<Entry>& t) {
    std::string s;
    for (const auto& e : t) {
        if (!s.empty()) s += ",";
        s += e.name + ":" + std::to_string(e.score);
    }
    phone_data::Get().store[light ? "duality.light" : "duality.dark"] = s;
    phone_data::Save();
}

class Duality : public Game {
public:
    Duality() : rng_(GetTickCount()) {
        dict_ = arcade::Dictionary("LD_SPAC");
        auto t = [&](const char* n) { return arcade::Texture(dict_, n); };
        backgnd_ = t("backgnd"); layer_ = t("layer"); ship_ = t("rockshp");
        thrust_ = t("thrustG"); dark_ = t("dark"); light_ = t("light"); white_ = t("white");
        shoot_ = t("shoot"); healthTex_ = t("Health"); powerTex_ = t("power"); logo_ = t("DUALITY");
        black_ = t("black");
        ex_[0] = t("ex1"); ex_[1] = t("ex2"); ex_[2] = t("ex3"); ex_[3] = t("ex4");
        // One of three arrangements of the title screen, as the script picks.
        layout_ = RandomInt(0, 3);
    }

    bool Frame(const arcade::Pad& pad, unsigned now, float dtMs) override {
        now_ = now;
        step_ = dtMs / 20.0f;           // the game's time step
        frame_ = dtMs / 1.5f / 1000.0f;  // the script's own frame time
        switch (mode_) {
            case Mode::Menu: return Menu(pad);
            case Mode::Play: Play(pad); return true;
            case Mode::Scores: Scores(pad); return true;
        }
        return true;
    }

private:
    enum class Mode { Menu, Play, Scores };

    int RandomInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi - 1)(rng_); }
    float RandomFloat(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }

    // --- The title screen ---------------------------------------------------

    struct Layout {
        float logoY, darkX, darkY, lightX, lightY, textX, row1, row2, pickLightX, pickY1,
            pickDarkX, pickY2;
    };

    bool Menu(const arcade::Pad& pad) {
        // The script's three arrangements, laid out again for the upright
        // screen: the logo above the menu, the menu split round the logo, and
        // the menu above the logo. Light is the left half, dark the right.
        static const Layout kLayouts[3] = {
            {220.0f, 356.0f, 120.0f, 70.0f, 330.0f, 192.0f, 440.0f, 482.0f, 208.0f, 455.0f, 240.0f, 497.0f},
            {330.0f, 368.0f, 330.0f, 70.0f, 330.0f, 192.0f, 100.0f, 540.0f, 208.0f, 114.0f, 240.0f, 555.0f},
            {470.0f, 368.0f, 560.0f, 70.0f, 330.0f, 192.0f, 100.0f, 142.0f, 208.0f, 114.0f, 240.0f, 156.0f},
        };
        const Layout& L = kLayouts[layout_];

        // Moving between the four: PLAY and QUIT on the top row, the two
        // HI-SCOREs under them; light on the left, dark on the right.
        if (pad.right && !prev_.right) { if (sel_ == 0) sel_ = 1; else if (sel_ == 2) sel_ = 3; }
        if (pad.left && !prev_.left) { if (sel_ == 1) sel_ = 0; else if (sel_ == 3) sel_ = 2; }
        if (pad.up && !prev_.up) { if (sel_ == 2) sel_ = 0; else if (sel_ == 3) sel_ = 1; }
        if (pad.down && !prev_.down) { if (sel_ == 0) sel_ = 2; else if (sel_ == 1) sel_ = 3; }

        arcade::Sprite(black_, kCentreX * 0.5f, kCentreY, kCentreX, arcade::kHeight + 50.0f, kFull);
        arcade::Sprite(white_, kCentreX * 1.5f, kCentreY, kCentreX, arcade::kHeight + 50.0f, kFull);
        arcade::Sprite(logo_, kCentreX, L.logoY, 256.0f, 128.0f, kFull);
        arcade::Sprite(dark_, L.darkX, L.darkY, 32.0f, 32.0f, kDim);
        arcade::Sprite(light_, L.lightX, L.lightY, 32.0f, 32.0f, kFull);

        arcade::TextStyle white;
        white.argb = kFull;
        white.align = sprite::Align::Right;
        white.shadow = false;
        // Narrower than the script's, so both halves' words fit their 224.
        white.scaleX = 0.75f;
        arcade::TextStyle blackText = white;
        blackText.argb = 0xFF000000;
        blackText.align = sprite::Align::Left;
        arcade::Text(L.textX, L.row1, "PLAY", white);
        arcade::Text(256.0f, L.row1, "QUIT", blackText);
        arcade::Text(L.textX, L.row2, "HI-SCORE", white);
        arcade::Text(256.0f, L.row2, "HI-SCORE", blackText);

        const float px = (sel_ == 0 || sel_ == 2) ? L.pickLightX : L.pickDarkX;
        const float py = sel_ < 2 ? L.pickY1 : L.pickY2;
        arcade::Sprite((sel_ == 0 || sel_ == 2) ? light_ : dark_, px, py, 16.0f, 16.0f, kFull);

        Help("Arrows Navigate~n~Space Select");

        const bool select = pad.cross && !prev_.cross;
        prev_ = pad;
        if (select) {
            if (sel_ == 0) StartGame();
            else if (sel_ == 1) return false;
            else OpenScores(sel_ == 2, false, 0);
        }
        return true;
    }

    void Help(const std::string& text) {
        int lines = 1;
        for (size_t p = text.find("~n~"); p != std::string::npos; p = text.find("~n~", p + 3)) ++lines;
        arcade::Window(33.8609f, 18.1114f, 230.0f, 18.1114f + 8.0f + lines * 17.0f);
        arcade::TextStyle s;
        s.face = sprite::Face::Subtitles;
        s.scaleX = 0.5014f;
        s.scaleY = 1.8889f;
        s.argb = 0xFFE1E1E1;
        s.align = sprite::Align::Left;
        s.shadow = false;
        float y = 20.4681f;
        size_t p = 0;
        while (p <= text.size()) {
            const size_t n = text.find("~n~", p);
            arcade::Text(38.1753f, y, text.substr(p, n == std::string::npos ? std::string::npos : n - p), s);
            y += 17.0f;
            if (n == std::string::npos) break;
            p = n + 3;
        }
    }

    // --- The game -------------------------------------------------------------

    struct Body { float x, y, mass; };
    struct Pickup { float x, y; };
    struct Bang { float x, y; int frame; unsigned next; };  // frame 0 free, 1 new, 9..12 showing
    struct Shot { float x, y, vx, vy; unsigned until; };

    void StartGame() {
        mode_ = Mode::Play;
        for (int i = 0; i < 4; ++i) layerX_[i] = layerY_[i] = 0.0f;
        shipX_ = shipY_ = lastX_ = lastY_ = 0.0f;
        velX_ = velY_ = 0.0f;
        heading_ = 0.1f;
        flame_ = 0;
        const float xs[10] = {200, -300, -300, 300, 0.1f, 0.1f, 300, -300, 0.1f, 0.1f};
        const float ys[10] = {200, -300, 300, -300, 300, -300, 0.1f, 0.1f, 500, -500};
        for (int i = 0; i < 10; ++i) bodies_[i] = {xs[i], ys[i], i < 5 ? -2.0e11f : 2.0e11f};
        for (auto& p : pickups_) p = {1000.0f, 1000.0f};
        for (auto& b : bangs_) b = {};
        for (auto& s : shots_) s = {};
        score_ = 0;
        power_ = 100.0f;
        health_ = 100.0f;
        quit_ = false;
        helpUntil_ = now_ + 4000;
        deadUntil_ = now_;
        nextShot_ = 0;
        triangleArmed_ = false;
    }

    void Explode(float x, float y) {
        for (auto& b : bangs_) {
            if (b.frame == 0) { b = {x, y, 1, 0}; return; }
        }
    }

    // Somewhere just off screen around the ship, as the script picks.
    void Respawn(float& x, float& y) {
        float dx, dy;
        if (RandomInt(0, 2) == 0) {
            dx = RandomFloat(340.0f, 700.0f);
            if (RandomInt(0, 2) == 0) dx = -dx;
            dy = RandomFloat(-700.0f, 700.0f);
        } else {
            dx = RandomFloat(-700.0f, 700.0f);
            dy = RandomFloat(340.0f, 700.0f);
            if (RandomInt(0, 2) == 0) dy = -dy;
        }
        x = shipX_ + dx;
        y = shipY_ + dy;
    }

    bool Alive() const { return now_ > deadUntil_; }

    void Play(const arcade::Pad& pad) {
        float forceX = 0.0f, forceY = 0.0f;
        power_ = std::min(100.0f, power_ + 0.06f * step_);

        // Backspace, pressed and let go: end the game.
        if (pad.triangle) {
            if (triangleArmed_) { triangleArmed_ = false; quit_ = true; }
        } else {
            triangleArmed_ = true;
        }
        if (pad.right) heading_ += 5.0f * step_;
        else if (pad.left) heading_ -= 5.0f * step_;
        heading_ = std::fmod(heading_ + 360.0f, 360.0f);

        constexpr float kG = 6.6726e-6f;
        constexpr float kShipMass = 100.0f;
        for (auto& b : bodies_) {
            const float d = std::hypot(b.x - shipX_, b.y - shipY_);
            int gone = d > 700.0f ? 1 : 0;
            if (d < 100.0f && Collide(shipX_, shipY_, 32, 32, b.x, b.y, 32, 32)) {
                if (b.mass > 0.0f) {
                    health_ -= 1.0f * step_;
                    if (health_ < 0.0f) {
                        health_ = 0.0f;
                        Explode(shipX_, shipY_);
                        deadUntil_ = now_ + 3000;
                    }
                } else {
                    health_ = std::min(100.0f, health_ + 1.0f * step_);
                }
            }
            for (auto& s : shots_) {
                if (s.until > now_ && Collide(s.x, s.y, 12, 12, b.x, b.y, 32, 32)) {
                    gone = 2;
                    s.until = 0;
                    if (Alive()) score_ += b.mass > 0.0f ? 10 : -10;
                    break;
                }
            }
            if (gone) {
                if (gone == 2) Explode(b.x, b.y);
                Respawn(b.x, b.y);
            } else {
                // Newton's law, with the dark bodies' mass positive and the
                // light ones' negative: one pulls, the other pushes.
                const float f = b.mass * kShipMass * kG / (d * d);
                const float fx = std::clamp((b.x - shipX_) / d * f, -50000.0f, 50000.0f);
                const float fy = std::clamp((b.y - shipY_) / d * f, -50000.0f, 50000.0f);
                forceX += fx;
                forceY += fy;
            }
        }

        if (Alive()) {
            if (pad.cross) {
                if (power_ >= 0.2f) {
                    forceX += -Sin(heading_) * 20000.0f;
                    forceY += Cos(heading_) * 20000.0f;
                    flame_ = std::min(200, flame_ + 50);
                    power_ -= 0.2f * step_;
                }
            } else {
                flame_ = std::max(0, flame_ - 20);
            }
            if (power_ >= 0.1f && pad.r1) {
                forceX += -Sin(heading_ + 90.0f) * 6000.0f;
                forceY += Cos(heading_ + 90.0f) * 6000.0f;
                power_ -= 0.1f * step_;
            }
            if (power_ >= 0.1f && pad.l1) {
                forceX += -Sin(heading_ - 90.0f) * 6000.0f;
                forceY += Cos(heading_ - 90.0f) * 6000.0f;
                power_ -= 0.1f * step_;
            }
            forceX -= velX_ * 40.0f;
            forceY -= velY_ * 40.0f;
            velX_ += forceX / kShipMass * frame_;
            velY_ += forceY / kShipMass * frame_;
            if (power_ >= 1.0f && pad.circle && now_ > nextShot_) {
                for (auto& s : shots_) {
                    if (now_ > s.until) {
                        s = {shipX_, shipY_, -Sin(heading_) * 300.0f + velX_, Cos(heading_) * 300.0f + velY_,
                             now_ + 1600};
                        break;
                    }
                }
                nextShot_ = now_ + 250;
                power_ -= 1.0f * step_;
                if (score_ > 0) score_ -= 1;
            }
        }

        lastX_ = shipX_;
        lastY_ = shipY_;
        velX_ = std::clamp(velX_, -300.0f, 300.0f);
        velY_ = std::clamp(velY_, -300.0f, 300.0f);
        shipX_ += velX_ * frame_;
        shipY_ += velY_ * frame_;

        Stars();

        for (const auto& b : bodies_) {
            const float x = b.x - shipX_ + kCentreX, y = b.y - shipY_ + kCentreY;
            if (b.mass > 0.0f) arcade::Sprite(dark_, x, y, 32, 32, kDim);
            else arcade::Sprite(light_, x, y, 32, 32, kFull);
        }

        for (int i = 0; i < 20; ++i) {
            Pickup& p = pickups_[i];
            const float d = std::hypot(p.x - shipX_, p.y - shipY_);
            const bool touched = Collide(shipX_, shipY_, 32, 32, p.x, p.y, 18, 18);
            if (d > 600.0f || touched) {
                if (touched && Alive()) {
                    score_ += i < 10 ? 5 : -5;
                    power_ = std::min(100.0f, power_ + 25.0f);
                }
                Respawn(p.x, p.y);
            }
            const float x = p.x - shipX_ + kCentreX, y = p.y - shipY_ + kCentreY;
            if (i < 10) arcade::Sprite(light_, x, y, 16, 16, kFull);
            else arcade::Sprite(dark_, x, y, 16, 16, kDim);
        }

        for (auto& b : bangs_) {
            if (b.frame == 0) continue;
            if (b.frame == 1) {
                b.next = now_ + 500;
                b.frame = 9;
            } else if (now_ > b.next) {
                b.next = now_ + 500;
                if (++b.frame > 12) { b.frame = 0; continue; }
            }
            arcade::Sprite(ex_[b.frame - 9], b.x - shipX_ + kCentreX, b.y - shipY_ + kCentreY, 32, 32, kDim);
        }

        for (auto& s : shots_) {
            if (s.until <= now_) continue;
            s.x += s.vx * frame_;
            s.y += s.vy * frame_;
            arcade::Sprite(shoot_, s.x - shipX_ + kCentreX, s.y - shipY_ + kCentreY, 8, 8, kDim);
        }

        arcade::TextStyle big;
        big.shadow = false;
        if (Alive()) {
            // The rocket's nose points the way its thrust pushes it.
            arcade::SpriteRotated(ship_, kCentreX, kCentreY, 32, 32, heading_ + 180.0f, kDim);
            // Health and power, as two bars in the bottom left corner.
            constexpr float kBarBottom = arcade::kHeight - 48.0f;
            arcade::Sprite(white_, 50.0f, kBarBottom - 50.0f, 14.0f, 104.0f, 0xFF000000);
            arcade::Sprite(healthTex_, 50.0f, kBarBottom - health_ / 2.0f, 10.0f, health_, 0xFFC8C8C8);
            arcade::Sprite(white_, 70.0f, kBarBottom - 50.0f, 14.0f, 104.0f, 0xFF000000);
            arcade::Sprite(powerTex_, 70.0f, kBarBottom - power_ / 2.0f, 10.0f, power_, 0xFFC8C8C8);
            if (flame_ > 0) {
                const float a = heading_ + 180.0f;
                const uint32_t c = 0xFF000000 | (flame_ << 16) | (flame_ << 8) | flame_;
                arcade::SpriteRotated(thrust_, kCentreX - Sin(a) * 24.0f, kCentreY + Cos(a) * 24.0f, 16, 16,
                                      heading_ + 180.0f, c);
            }
        } else {
            big.argb = kMenuGrey;
            arcade::Text(kCentreX, 160.0f, "GAME OVER!", big);
            if (now_ + 1000 > deadUntil_) quit_ = true;
        }

        if (score_ > -1) {
            big.argb = kMenuGrey;
            arcade::Text(kCentreX, 60.0f, "SCORE " + std::to_string(score_), big);
        } else {
            big.argb = 0xFF000000;
            big.edge = 1;
            arcade::Text(kCentreX, 60.0f, "SCORE " + std::to_string(-score_), big);
        }
        if (helpUntil_ > now_) {
            Help("Left Right Rotate~n~Space Thrust~n~Click Shoot~n~Backspace Exit");
        }

        if (quit_) {
            const auto lightTable = LoadTable(true), darkTable = LoadTable(false);
            if (score_ > lightTable[9].score) OpenScores(true, true, score_);
            else if (score_ < darkTable[9].score) OpenScores(false, true, score_);
            else { mode_ = Mode::Menu; prev_ = pad; }
        }
    }

    // Three layers of stars, the far one moving half as fast as the ship.
    void Stars() {
        for (int l = 0; l < 3; ++l) {
            const float k = (l + 1) * 0.3f;
            float dx = (lastX_ - shipX_) * 0.6f * k;
            float dy = (lastY_ - shipY_) * 0.6f * k;
            const float tile = 512.0f * k;
            if (l == 0) { dx /= 2.0f; dy /= 2.0f; }
            layerX_[l] += dx;
            layerY_[l] += dy;
            if (layerX_[l] > tile / 2.0f) layerX_[l] -= tile;
            if (-tile > layerX_[l]) layerX_[l] += tile;
            if (layerY_[l] > tile / 2.0f) layerY_[l] -= tile;
            if (-tile > layerY_[l]) layerY_[l] += tile;
            const int cols = static_cast<int>(std::ceil((arcade::kWidth + tile) / tile)) + 1;
            const int rows = static_cast<int>(std::ceil((arcade::kHeight + tile) / tile)) + 1;
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    arcade::Sprite(l == 0 ? backgnd_ : layer_, layerX_[l] + c * tile, layerY_[l] + r * tile,
                                   tile, tile, kDim);
                }
            }
        }
    }

    // --- The high-score tables ------------------------------------------------

    void OpenScores(bool light, bool entering, int newScore) {
        mode_ = Mode::Scores;
        light_side_ = light;
        table_ = LoadTable(light);
        editRow_ = -1;
        // The stars drift slowly behind the table.
        shipX_ = shipY_ = 0.0f;
        lastX_ = RandomFloat(-15.0f, 15.0f);
        lastY_ = RandomFloat(-15.0f, 15.0f);
        if (entering) {
            for (int i = 9; i >= 0; --i) {
                const bool better = light ? newScore > table_[i].score : table_[i].score > newScore;
                if (!better) break;
                if (i != 9) table_[i + 1] = table_[i];
                table_[i] = {"...", newScore};
                editRow_ = i;
            }
            if (editRow_ >= 0) {
                letter_ = 10;  // 'A'
                letterPos_ = 0;
                nextUp_ = nextDown_ = 0;
            }
        }
        // The button that opened the table has to be let go of first.
        crossArmed_ = true;
    }

    void Scores(const arcade::Pad& pad) {
        Stars();
        if (editRow_ >= 0) {
            std::string& name = table_[editRow_].name;
            name[letterPos_] = kLetters[letter_];
            for (int i = letterPos_ + 1; i < 3; ++i) name[i] = '.';
            if (pad.up) {
                if (now_ > nextUp_) { letter_ = letter_ < 36 ? letter_ + 1 : 0; nextUp_ = now_ + 250; }
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
                        SaveTable(light_side_, table_);
                    }
                }
            } else {
                crossArmed_ = false;
            }
        }

        arcade::TextStyle t;
        t.face = sprite::Face::Subtitles;
        t.shadow = false;
        if (!light_side_) { t.argb = 0xFF000000; t.edge = 1; } else { t.argb = kMenuGrey; }
        arcade::TextStyle title = t;
        title.face = sprite::Face::Menu;
        // The script's table, moved to the middle of the upright screen.
        arcade::Text(kCentreX + 1.4137f, kTableTop - 30.6736f, "HI-SCORE", title);
        float y = kTableTop;
        for (int i = 0; i < 10; ++i) {
            arcade::TextStyle row = t;
            row.align = sprite::Align::Left;
            if (i == editRow_) { row.argb = 0xFF009B00; }
            float x = kTableLeft;
            for (int j = 0; j < 3; ++j) {
                const char c = j < static_cast<int>(table_[i].name.size()) ? table_[i].name[j] : '.';
                // The narrow '1' and 'I' are nudged to the middle of their space.
                const float nudge = (c == '1' || c == 'I') ? 8.0f : 0.0f;
                arcade::Text(x + nudge, y, std::string(1, c), row);
                x += 29.78f;
            }
            row.align = sprite::Align::Right;
            arcade::Text(kTableLeft + 230.5296f, y, std::to_string(std::abs(table_[i].score)), row);
            y += 28.5713f;
        }
        if (editRow_ >= 0) {
            Help("Up Down Navigate~n~Space Select");
        } else if (pad.triangle || pad.circle || pad.square || (pad.cross && !crossArmed_)) {
            mode_ = Mode::Menu;
            prev_ = pad;
        }
        if (!pad.cross) crossArmed_ = false;
    }

    std::mt19937 rng_;
    int dict_ = -1;
    uintptr_t backgnd_ = 0, layer_ = 0, ship_ = 0, thrust_ = 0, dark_ = 0, light_ = 0,
              white_ = 0, shoot_ = 0, healthTex_ = 0, powerTex_ = 0, logo_ = 0, black_ = 0, ex_[4] = {};

    Mode mode_ = Mode::Menu;
    int layout_ = 0;
    int sel_ = 0;
    arcade::Pad prev_{};
    unsigned now_ = 0;
    float step_ = 1.0f, frame_ = 0.0f;

    float layerX_[4] = {}, layerY_[4] = {};
    float shipX_ = 0, shipY_ = 0, lastX_ = 0, lastY_ = 0, velX_ = 0, velY_ = 0, heading_ = 0.1f;
    int flame_ = 0;
    Body bodies_[10] = {};
    Pickup pickups_[20] = {};
    Bang bangs_[10] = {};
    Shot shots_[10] = {};
    int score_ = 0;
    float power_ = 100.0f, health_ = 100.0f;
    bool quit_ = false, triangleArmed_ = false;
    unsigned helpUntil_ = 0, deadUntil_ = 0, nextShot_ = 0;

    bool light_side_ = true;
    std::vector<Entry> table_;
    int editRow_ = -1, letter_ = 10, letterPos_ = 0;
    unsigned nextUp_ = 0, nextDown_ = 0;
    bool crossArmed_ = false;
};

}  // namespace

std::unique_ptr<Game> MakeDuality() {
    return std::make_unique<Duality>();
}

}  // namespace games
