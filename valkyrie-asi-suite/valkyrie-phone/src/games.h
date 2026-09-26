// San Andreas' arcade machines, ported from their mission scripts to run on
// the phone. Each one keeps the script's rules, numbers, sprites and text; the
// high-score tables are kept by the phone instead of the save game.
#pragma once

#include <memory>

#include "arcade.h"

namespace games {

class Game {
public:
    virtual ~Game() = default;
    // One frame: read the pad, move everything on, draw. `now` is the game
    // clock in milliseconds and `dtMs` the time since the last frame. False
    // once the player has quit the machine.
    virtual bool Frame(const arcade::Pad& pad, unsigned now, float dtMs) = 0;
};

// Duality (the DUAL script, LD_SPAC).
std::unique_ptr<Game> MakeDuality();
// Let's Get Ready To Bumble (the GRAV script, ld_grav).
std::unique_ptr<Game> MakeBumble();
// They Crawled From Uranus (the NONE script, LD_NONE).
std::unique_ptr<Game> MakeUranus();
// Go Go Space Monkey (the SHTR script, ld_shtr).
std::unique_ptr<Game> MakeSpaceMonkey();

}  // namespace games
