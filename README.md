# Valkyrie Phone

For shared-source updates and related repositories, see [maintenance](MAINTENANCE.md).

A working phone for GTA: San Andreas single player, built as one ASI plugin.
CJ carries a 2007-style iFruit: he takes it out, holds it, and uses it in the
world while the game goes on around him.

It runs on Project Eagle, on GTA: San Andreas 1.0 US, and on Project Silent
Hill.

## What it does

- **Phone and contacts.** Calls with the phone held to CJ's ear, a keypad,
  recent calls, and contacts of your own.
- **911.** Police come and guard you, or come for you if you are wanted. Fire
  crews put out the fires around you. Paramedics treat you.
- **Food delivery.** Well Stacked Pizza, Burger Shot and Cluckin' Bell
  deliver to where you are, at the shops' own prices.
- **Text messages**, with text tones.
- **Signal from the map's radio masts.** Bars drop indoors and far from a
  mast, some valleys have no service, and calls, texts and web pages follow
  the signal. `tools/signal-coverage` works it out.
- **Camera and Photos.** A live viewfinder, selfies, and a camera roll saved
  to your pictures folder.
- **Maps.** A 3D map of the world you can drag, zoom and tilt, with CJ and the
  map marker on it.
- **Internet.** A 2007 mobile browser, loading pages as slowly as EDGE did.
- **Radio.** San Andreas' stations on foot, or the car radio when in a car.
- **Games.** Duality, Let's Get Ready To Bumble, They Crawled From Uranus and
  Go Go Space Monkey, the game's own arcade machines, with high scores.
- **Flashlight**, Weather, Stocks, Calendar, Clock with alarm, stopwatch and
  timer, Calculator, Notes and Settings.
- **The trainer**, as an app inside the phone.
- **The handset itself** is a lit 3D model: it reflects the world around it,
  picks up scratches, rain, cracks and blood, and has working side buttons.
- **The phone as a weapon.** An optional weapon slot, so you can scroll to it
  like a gun.

Everything is set in `valkyrie-phone.ini`, which the phone writes on first
start.

## Installing

Download a release from <https://files.sp-rp.com/> (you need to be a member of
the SP-RP Discord). There are three: with Project Eagle's map, with GTA: San
Andreas' map, or with a script that builds the map from your own game
(`build-phone-map.ps1`). Each archive has a README saying what goes where.

The Maps app needs the map tiles for your world: the Project Eagle set or the
original GTA: San Andreas set. The tiles are rendered from the games' own
models and textures, so they are not in this repository. Download them from
<https://files.sp-rp.com/> and put the `Valkyrie-radar-tiles` folder next to
the game's executable.

## Building

You need Windows, Visual Studio's C++ build tools (x86), Git and Python 3 with
Pillow.

```
git clone --recurse-submodules https://github.com/darkcenturies/valkyrie-phone.git
cd valkyrie-phone\valkyrie-asi-suite
powershell -ExecutionPolicy Bypass -File build.ps1
```

The ASI is written to `valkyrie-asi-suite\build\valkyrie-phone.asi`.
`build.ps1 -Install` also copies it into your game folder (found through
Steam, or give `-GamePath`).

### What you build yourself

Some of the phone's content comes from games you own, so it is made on your
machine rather than kept here:

- **The browser's pages.** Built from your own copy of GTA IV's in-game
  internet. With GTA IV installed, `build.ps1` finds it and builds the pages
  once (it needs Playwright and Chrome, see
  `valkyrie-asi-suite\valkyrie-phone\README.md`). Without it, the browser has
  no pages.
- **The Project Eagle HUD icon** for the phone as a weapon.
  `valkyrie-phone\tools\phone-model\make-pe-weapon-icon.py` makes it from
  Project Eagle's own weapon icons.

The game's radar icons, cursor, fonts and arcade graphics are read from your
game's `models` folder at run time.

## Layout

- `valkyrie-asi-suite\valkyrie-phone`: the phone, its art, model, tones and
  tools.
- `valkyrie-asi-suite\valkyrie-radar`: the 3D map renderer and road router
  the Maps app uses.
- `valkyrie-asi-suite\valkyrie-core`: the shared game hooks and drawing code.
- `valkyrie-trainer`: the trainer the phone carries, built on plugin-sdk and
  Dear ImGui (submodules).

## Contributing

Pull requests are welcome; each one is reviewed by the maintainer before it
is merged. See `CONTRIBUTING.md`.

## Licence

The code here is under the BSD 3-Clause licence (`LICENSE`). Third-party
code keeps its own licence: see `THIRD_PARTY_NOTICES.md`.

This is an unofficial fan project. It is not affiliated with or endorsed by
Rockstar Games or Take-Two Interactive. Grand Theft Auto and San Andreas are
their trademarks. Project Eagle, Stars & Stripes Multiplayer and Project
Silent Hill are named only to say what the phone works with. This repository
contains no game files; you need your own copy of the game.

Rights holders can write to legal@sp-rp.com.
