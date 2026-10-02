# Valkyrie Phone for GTA San Andreas

The SP-RP phone for GTA San Andreas 1.0 US: calls, messages, contacts,
camera, flashlight, maps, services and a trainer carried inside the ASI.
Press P to open it. The iFruit appearance is the default; `Skin=Keypad`
selects Valkyrie's optional keypad handset. Other original artwork is retained.

Initialize the pinned SDK/ImGui submodules, then build:

```powershell
git submodule update --init --recursive
./valkyrie-asi-suite/build.ps1 -Release
```

MSVC C++ Build Tools and a Windows SDK are required. An x86 ASI loader is
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

The core build includes the phone's artwork, held model, textures, tones and
embedded trainer. Map tiles are supplied/generated separately. No browser-page
pack is checked in at `valkyrie-asi-suite/valkyrie-phone/assets/web/valkyrie-web.dat`;
without a generated/supplied pack the browser has no pages. GTA IV page generation
requires the player's own permitted input and its documented extra dependencies.

On 2026-10-01 the optimized x86 build and script-edition packaging/7-Zip integrity
test passed after initializing the pinned submodules. No installation or gameplay
test was performed. These checks establish build/package completeness, not
in-game startup, camera, maps or device-reset behavior.

[GTA Workshop](https://github.com/darkcenturies/gta-workshop) contains guides,
references and public research; this repository retains the public Phone source.

The repository contains no game files. Use your own copy of GTA San Andreas.
Original notices and third-party credits remain in THIRD_PARTY_NOTICES.md.

## Download merged builds

Every successful merge to `main` produces a Windows x86 build in [Actions](https://github.com/darkcenturies/valkyrie-phone/actions). Open the successful main-branch run and download its `valkyrie-phone-gta-sa-1.0-<commit>` artifact. It contains the ASI, default configuration, map-building tools, source archive, licence and notices. Builds are kept for 30 days. Generate map data from your own game; no game data or private symbols are included.
