# valkyrie phone

An in-game phone for **GTA San Andreas 1.0 US**, with calls, messages, a camera and a 3D map.

**[Download](https://github.com/darkcenturies/valkyrie-phone/releases/latest)** · [Controls and configuration](valkyrie-asi-suite/valkyrie-phone/README.md) · [Report an issue](https://github.com/darkcenturies/valkyrie-phone/issues)

## Features

- Calls, messages, contacts and services.
- Camera, photo gallery, flashlight, radio and minigames.
- 3D Maps with generated stock San Andreas map tiles included.
- iFruit and keypad handsets, with coarse pixel icons in the game's style.
- Original left-hand animations for holding, typing, calls, photos and selfies.
- Built-in trainer, available through Contacts or **Alt+Z**.

The Internet app includes **eight authored offline San Andreas pages**. External links open your desktop browser. GTA IV websites can be added in a [personal build](BUILDING.md#optional-browser-content).

## Installation

Requires GTA San Andreas **1.0 US** and an **x86 ASI loader**.

1. Download and extract `valkyrie-phone-gta-sa-1.0-full.zip`.
2. Close the game and copy `valkyrie-phone.asi`, `valkyrie-phone.ini` and `valkyrie-radar-tiles` into the folder containing `gta_sa.exe`.
3. Start the game and press **P** to open the phone. Use the mouse to navigate.

Settings are in `valkyrie-phone.ini`. In `[Phone]`, `Skin=Keypad` selects the keypad handset. `IconSize=16` is the default pixel artwork; `IconSize=64` selects the older detailed icons.

## Optional

- **Phone as a weapon:** requires [Modloader 0.3.10](https://github.com/thelink2012/modloader/releases/tag/v0.3.10) and [fastman92 7.6](https://www.fastman92.com/fastman92-limit-adjuster/). Copy the contents of `Optional/Phone as a weapon/Copy into game folder` into the game folder. The replacement configs are already prepared for stock SA plus the phone. Back up existing configs and retain any other custom weapon or limit settings.
- **Map builder:** `Optional/build-phone-map.ps1` lets you generate tiles from your own custom map. The included stock tiles are ready to use.

The download's README covers both optional installs. Dependency binaries are installed separately.

## Development

[Building from source](BUILDING.md) · [Contributing](CONTRIBUTING.md) · [Maintenance](MAINTENANCE.md)

Successful merges to `main` publish a complete build to Releases.

## Licence and credits

[BSD 3-Clause](LICENSE). See [third-party credits](THIRD_PARTY_NOTICES.md) for libraries, artwork and game-content attribution.
