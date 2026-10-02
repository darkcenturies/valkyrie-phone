param(
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Cache
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'phone-content.json') -Raw | ConvertFrom-Json
if (-not $Cache) { $Cache = Join-Path ([IO.Path]::GetTempPath()) 'valkyrie-phone-content-v1' }
New-Item -ItemType Directory -Force -Path $Cache, $Output | Out-Null
foreach ($asset in $manifest.assets) {
    $path = Join-Path $Cache $asset.name
    if (-not (Test-Path -LiteralPath $path)) {
        Invoke-WebRequest -Uri $asset.url -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $asset.sha256) {
        throw "Content checksum failed: $($asset.name)"
    }
}
$tiles = Join-Path $Output 'valkyrie-radar-tiles'
New-Item -ItemType Directory -Force -Path $tiles | Out-Null
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
& $sevenZip x (Join-Path $Cache 'gta-sa-map-tiles.7z') "-o$tiles" -y -bsp0 -bso0
if ($LASTEXITCODE -ne 0) { throw 'Map archive extraction failed.' }
$files = @(Get-ChildItem -LiteralPath $tiles -File)
if ($files.Count -ne $manifest.tiles.Count) { throw 'Unexpected map tile file count.' }
foreach ($tile in $manifest.tiles) {
    $path = Join-Path $tiles $tile.name
    if (-not (Test-Path -LiteralPath $path) -or
        (Get-Item -LiteralPath $path).Length -ne $tile.bytes -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $tile.sha256) {
        throw "Map tile validation failed: $($tile.name)"
    }
}
$optional = Join-Path $Output 'Optional/Phone as a weapon'
$gameFiles = Join-Path $optional 'Copy into game folder'
$weapon = Join-Path $gameFiles 'modloader/valkyrie phone'
& (Join-Path $PSScriptRoot 'make-phone-weapon.ps1') -Output $weapon
if (-not (Test-Path -LiteralPath (Join-Path $weapon 'valkyriephone.txd'))) {
    throw 'Phone weapon generation failed.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $gameFiles 'data') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'phone-weapon-config/gtasa_weapon_config.dat') -Destination (Join-Path $gameFiles 'data')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'phone-weapon-config/fastman92limitAdjuster_GTASA.ini') -Destination $gameFiles
# This TXT is data consumed by Modloader, not a second install readme.
Remove-Item -LiteralPath (Join-Path $weapon 'Valkyrie Phone.txt')
$weaponLines = @(
    'IDE DATA\VALKYRIE-PHONE.IDE',
    "$([char]0xA3) VALKYRIEPHONE            MELEE 10.0  1.6  19990 -1  12 FLOWERS        1  1    null"
)
[IO.File]::WriteAllLines((Join-Path $weapon 'valkyrie phone.txt'), [string[]]$weaponLines, [Text.Encoding]::GetEncoding(28591))
$builder = Join-Path $Output 'Optional'
New-Item -ItemType Directory -Force -Path $builder | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build-phone-map.ps1') -Destination $builder
Set-Content -LiteralPath (Join-Path $Output 'README.md') -Encoding ASCII -Value @'
# valkyrie phone for GTA San Andreas 1.0 US

## Install

Close the game. Copy valkyrie-phone.asi, valkyrie-phone.ini and the
valkyrie-radar-tiles folder into the game folder containing gta_sa.exe.
An x86 ASI loader is required. Settings are in valkyrie-phone.ini.

## Controls and contents

Press P to open/close the phone; use the mouse to navigate.
Calls, texts, contacts, camera, photos, flashlight, maps, services and trainer
are included. Settings are in valkyrie-phone.ini. Call Trainer in Contacts
or press Alt+Z for the trainer. Skin=Keypad selects the optional keypad handset.
In [Phone], IconSize=16 uses the default pixel artwork; IconSize=64 selects
the older detailed icons. The original GTA Settings wrench stays unchanged.
Maps includes 142 generated stock SA tile pairs. Internet includes eight
authored offline SA pages. External links open the desktop browser; GTA IV
browser pages are personal local build inputs.

## Optional: Phone as a weapon

Requires Modloader 0.3.10 and fastman92 limit adjuster 7.6, installed separately:
https://github.com/thelink2012/modloader/releases/tag/v0.3.10
https://www.fastman92.com/fastman92-limit-adjuster/

Back up fastman92limitAdjuster_GTASA.ini and data/gtasa_weapon_config.dat.
Copy the contents of Optional/Phone as a weapon/Copy into game folder into
the game folder, replacing those two configs. The loader and phone entry are
already configured; no edits are needed on stock SA. Existing custom weapon
or limit settings must be retained when combining mods.

Scroll to the phone; right click raises/lowers it. Weapon 70, model 19990, slot 12.
To remove weapon mode, restore both backed-up configs and remove
modloader/valkyrie phone. The normal P-key phone does not need weapon mode.

## Optional: Map builder

The supplied stock map is ready to use. For custom maps, install Python 3 and
run: python -m pip install Pillow numpy
From Optional, run:
./build-phone-map.ps1 -GamePath 'C:\Games\GTA San Andreas' -Test
./build-phone-map.ps1 -GamePath 'C:\Games\GTA San Andreas'
The full run installs generated tiles into your game. -SkipInstall only builds
files. The sample run does not install tiles. Several GB free space are needed.

## Help, source and credits

Report issues with valkyrie-phone.log and valkyrie-phone.previous.log:
https://github.com/darkcenturies/valkyrie-phone
Source and full controls: https://github.com/darkcenturies/valkyrie-phone

Project licence: LICENSE. Upstream credits/licences: THIRD_PARTY_NOTICES.md
and the licence texts embedded in the ASI. Stock weapon config template:
fastman92. Rockstar Games retains ownership of the underlying stock SA map
geometry/textures. Use your own copy of GTA San Andreas.
'@
"Verified $($manifest.tiles.Count) map files and staged ready-to-copy optional weapon configs."
$allowed = @('valkyrie-phone.asi', 'valkyrie-phone.ini', 'valkyrie-radar-tiles', 'Optional', 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')
foreach ($item in Get-ChildItem -LiteralPath $Output) {
    if ($item.Name -cnotin $allowed) { throw "Unexpected install package item: $($item.Name)" }
}
if (@(Get-ChildItem -LiteralPath $Output -Recurse -File -Filter 'README*').Count -ne 1) {
    throw 'Install package must have one README.'
}
foreach ($item in Get-ChildItem -LiteralPath $Output -Recurse) {
    if ($item.Name -cmatch 'Valkyrie') { throw "Mixed-case project name: $($item.Name)" }
}
