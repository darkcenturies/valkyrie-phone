# Building valkyrie phone

Requires MSVC C++ Build Tools, a Windows SDK, the pinned SDK/ImGui submodules,
and Python with Pillow, Playwright and Chromium for the authored websites.

```powershell
git submodule update --init --recursive
python -m pip install Pillow playwright
python -m playwright install chromium
./valkyrie-asi-suite/build.ps1 -Release
```

Output: `valkyrie-asi-suite/build/valkyrie-phone.asi` (Windows x86).
The build fails if it cannot include the authored pages.

## Optional browser content

Public builds include eight authored offline SA pages: Cluckin' Bell (home/menu),
Epsilon (home/join), eXsorbeo, Maccer, West Coast Rap Legends and sp-rp.com.
GTA IV websites and archived Rockstar promotional websites are not bundled.

For GTA IV pages, supply your own installation containing `pc/html` and
`pc/text/american.gxt`:

```powershell
./valkyrie-asi-suite/build.ps1 -Release -Gta4Path "C:\Games\Grand Theft Auto IV\GTAIV"
```

This rebuilds the browser pack even if one already exists. Missing input or
conversion failure stops an explicitly requested GTA IV build. Check conversion
messages and the page count to confirm inclusion. Local builds can also discover
GTA IV or reuse an existing local pack.

To reuse a VWEB pack from a previous personal build:

```powershell
./valkyrie-asi-suite/build.ps1 -Release -WebPackPath "C:\PhoneAssets\valkyrie-web.dat"
```

Use one of `-Gta4Path` or `-WebPackPath`. Neither uploads your input to GitHub.
See [browser details](valkyrie-asi-suite/valkyrie-phone/README.md#the-internet)
for conversion limitations.

## Map builder

Readable sources are in `valkyrie-asi-suite/tools/map-data`. Verify the generated
one-file builder with:

```powershell
python valkyrie-asi-suite/tools/make-phone-map-builder.py --check
```

Sample builds do not install tiles or share caches with a full build.
`-SkipInstall` prepares full data without copying it into the game.

## Releases and validation

Every successful main merge publishes the full install ZIP to
[Releases](https://github.com/darkcenturies/valkyrie-phone/releases/latest).
[Actions](https://github.com/darkcenturies/valkyrie-phone/actions) keeps the same
package as a commit-named artifact for 30 days. The pipeline downloads the pinned
[stock map content](https://github.com/darkcenturies/valkyrie-phone/releases/tag/phone-content-v1)
and verifies archive and individual tile hashes before staging it.

Install packages include the ASI, INI, 142 stock map tile pairs, optional weapon
model/configs, optional map builder, one README and licence/credits. Source
archives, build reports, content manifests, checksums and private symbols stay
out of the install package. Game executables, original game archives and weapon
dependency binaries are not bundled. Generated map assets remain outside Git.

CI checks native icon recovery, action transitions, pre-HUD reflection capture,
device resets, weapon discovery, the eight-page browser inventory and package
layout. In-game startup, camera, maps, device resets and appearance still need
verification in the supported game.

See [maintenance](MAINTENANCE.md) for source ownership and reviewed synchronization.
