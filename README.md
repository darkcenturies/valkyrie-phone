# valkyrie Phone for GTA San Andreas

The SP-RP phone for GTA San Andreas 1.0 US: calls, messages, contacts,
camera, flashlight, maps, services and a trainer carried inside the ASI.
Press P to open it. The iFruit appearance is the default; `Skin=Keypad`
selects valkyrie's optional keypad handset. Other original artwork is retained.

Initialize the pinned SDK/ImGui submodules, then build:

```powershell
git submodule update --init --recursive
python -m pip install Pillow playwright
python -m playwright install chromium
./valkyrie-asi-suite/build.ps1 -Release
```

MSVC C++ Build Tools and a Windows SDK are required. The bundled authored sites also need `python -m pip install Pillow playwright` and `python -m playwright install chromium`. The build fails if it cannot include the pages. An x86 ASI loader is
required. Install with the game closed. Actions packages include generated stock
SA map tiles. Custom maps can regenerate tiles using the included builder.
The optional phone-as-a-weapon folder includes the authored model and ready-to-copy
replacement configurations. Modloader 0.3.10 and fastman92 7.6 are required for
weapon mode; install those separately, then copy the supplied game-folder files. The embedded trainer replaces a separate copy.

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
| Configuration | Default INI |
| Optional tools | `Optional/build-phone-map.ps1` |
| Maps | `valkyrie-radar-tiles`: 142 generated stock GTA SA tile pairs, 284 `.r3g`/`.r3a` files; ready to copy into the game folder |
| Optional phone as a weapon | Ready-to-copy model/texture/modloader definitions, configured `fastman92limitAdjuster_GTASA.ini` and stock-plus-phone `data/gtasa_weapon_config.dat`; requires Modloader 0.3.10 and fastman92 7.6 |
| Installation | One `README.md` with normal installation, controls, optional weapon setup and map-builder instructions |
| Credits | Licence and third-party notices; source remains in this repository |

**Not included:** GTA IV websites, archived Rockstar promotional websites,
game executables, original game archives, dependency binaries, or private build symbols. Generated
map tiles use stock SA geometry/textures and are release assets, kept outside Git.
The normal installation copies the ASI, INI and tiles; weapon mode stays optional.
For weapon mode, install [Modloader](https://github.com/thelink2012/modloader/releases/tag/v0.3.10)
and [fastman92](https://www.fastman92.com/fastman92-limit-adjuster/), then copy
`Optional/Phone as a weapon/Copy into game folder` into the game folder,
replacing the supplied configuration files. They already enable the weapon loader,
retain its required author field and register the phone: no edits on a stock setup.
Back up existing configs first; these replacement files are for stock SA plus the
phone, so other custom weapon/limit settings need to be retained. The
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

To restore a page pack from a previous personal build instead of converting
GTA IV again, use its existing VWEB file:

```powershell
./valkyrie-asi-suite/build.ps1 -Release -WebPackPath "C:\PhoneAssets\valkyrie-web.dat"
```

This embeds that supplied local content in your ASI and prints its page count.
It is a personal build input, not content included in the Actions artifact.
Use one of `-Gta4Path` or `-WebPackPath`. Neither uploads the input to GitHub.

Build/tests establish package completeness; in-game startup, camera, maps,
device resets and appearance still require verification in the supported game.

[GTA Workshop](https://github.com/darkcenturies/gta-workshop) contains guides,
references and public research; this repository retains the public Phone source.

The source repository contains no game files. Release packages include generated
stock map data. Use your own copy of GTA San Andreas.
Original notices and third-party credits remain in THIRD_PARTY_NOTICES.md.

## Download merged builds

Download the **full install ZIP** from the [latest build release](https://github.com/darkcenturies/valkyrie-phone/releases/latest). Every successful merge to `main` publishes a permanent release containing the ASI, default configuration, **generated stock map tiles and ready-to-copy optional weapon configs**, optional map builder, one installation README, licence and notices. The install ZIP contains no source archive, build report, content manifest or checksum file.

The same complete package is also available as the `valkyrie-phone-gta-sa-1.0-<commit>` artifact in [Actions](https://github.com/darkcenturies/valkyrie-phone/actions), kept for 30 days. The separate [map content release](https://github.com/darkcenturies/valkyrie-phone/releases/tag/phone-content-v1) supplies pinned assets to the pipeline; it is not the full phone download. Actions verifies the map archive and every tile before publishing the complete build. No private symbols are included.
