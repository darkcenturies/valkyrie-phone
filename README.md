# Valkyrie Phone for GTA San Andreas

The SP-RP phone for GTA San Andreas 1.0 US: calls, messages, contacts,
camera, flashlight, maps, services and a trainer carried inside the ASI.
Press P to open it. The iFruit appearance is the default; `Skin=Keypad`
selects Valkyrie's optional keypad handset. Other original artwork is retained.

Initialize the pinned SDK/ImGui submodules, then build:

```powershell
git submodule update --init --recursive
python -m pip install Pillow playwright
python -m playwright install chromium
./valkyrie-asi-suite/build.ps1 -Release
```

MSVC C++ Build Tools and a Windows SDK are required. The bundled authored sites also need `python -m pip install Pillow playwright` and `python -m playwright install chromium`. The build fails if it cannot include the pages. An x86 ASI loader is
required. Install with the game closed. Maps needs tiles built from the player's
own game using the included builder. Optional phone-as-a-weapon installation needs
modloader and a compatible fastman92 weapon type loader; follow both steps in
the generated package's README. The embedded trainer replaces a separate copy.

See [the detailed controls and configuration](valkyrie-asi-suite/valkyrie-phone/README.md)
and [source maintenance](MAINTENANCE.md). Native compilation and isolated tests
are separate from in-game startup, camera, device-reset and weapon validation.
The release packages now target GTA San Andreas only.

Opening the handset reflects the current game frame; it does not force a second
world/mirror render or flush the model streaming queue. Only the Camera app uses
the separate lens view. Weapon packages use stock-safe model ID 19990. Phone
weapon discovery reads its explicit loader configuration and validates the ID
before calling into the game's weapon-info array.

Home-screen apps fall back to their bundled artwork when a custom or HUD icon
cannot load. HUD icons that become available after the first lookup are retried.
Builds refuse to package missing built-in app artwork.

The stock map builder's readable sources are in `valkyrie-asi-suite/tools/map-data`.
`python valkyrie-asi-suite/tools/make-phone-map-builder.py --check` verifies the
one-file distributable against those sources. Sample builds never install tiles
or share their caches with a full build. Add `-SkipInstall` to a full build to
prepare data without copying it into the game.

## Optional content and validation

The public Actions artifact ships:

| Included | Contents |
| --- | --- |
| `valkyrie-phone.asi` | Windows x86 phone, embedded trainer, authored handset model, artwork and tones |
| Offline Internet app | Eight authored pages: Cluckin' Bell (home/menu), Epsilon (home/join), eXsorbeo, Maccer, West Coast Rap Legends and sp-rp.com; embedded in the ASI |
| Configuration and tools | Default INI and map builder |
| Source and credits | Source ZIP, licence, notices and `BUILD.txt` identifying the commit and ASI hash |

**Not included:** GTA IV websites, archived Rockstar promotional websites,
GTA game files, generated map tiles, the optional weapon loader, or private
build symbols. Maps needs data generated from your own GTA SA install. The
Internet app navigates bundled offline pages; external links open your desktop
browser. It does not browse arbitrary live websites.

For GTA IV pages, build locally with your own GTA IV `pc/html` and
`pc/text/american.gxt` files:

```powershell
./valkyrie-asi-suite/build.ps1 -Release -Gta4Path "C:\Games\Grand Theft Auto IV\GTAIV"
```

This rebuilds the browser pack even if one already exists. Check conversion
messages to confirm GTA IV pages were included. An explicitly requested GTA IV
build fails if its input is missing or conversion fails. Local builds can also reuse an existing page
pack or discover a local GTA IV install. **Actions uses a clean checkout and
ships only the eight authored SA pages.** See [browser details and limitations](valkyrie-asi-suite/valkyrie-phone/README.md#the-internet).

Build/tests establish package completeness; in-game startup, camera, maps,
device resets and appearance still require verification in the supported game.

[GTA Workshop](https://github.com/darkcenturies/gta-workshop) contains guides,
references and public research; this repository retains the public Phone source.

The repository contains no game files. Use your own copy of GTA San Andreas.
Original notices and third-party credits remain in THIRD_PARTY_NOTICES.md.

## Download merged builds

Every successful merge to `main` produces a Windows x86 build in [Actions](https://github.com/darkcenturies/valkyrie-phone/actions). Open the successful main-branch run and download its `valkyrie-phone-gta-sa-1.0-<commit>` artifact. It contains the ASI, default configuration, map-building tools, source archive, licence and notices. Builds are kept for 30 days. Generate map data from your own game; no game data or private symbols are included.
