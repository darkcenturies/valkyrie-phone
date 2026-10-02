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
$tiles = Join-Path $Output 'Valkyrie-radar-tiles'
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
$optional = Join-Path $Output 'Optional - phone as a weapon'
$gameFiles = Join-Path $optional 'Copy into game folder'
$weapon = Join-Path $gameFiles 'modloader/Valkyrie Phone'
& (Join-Path $PSScriptRoot 'make-phone-weapon.ps1') -Output $weapon
if (-not (Test-Path -LiteralPath (Join-Path $weapon 'valkyriephone.txd'))) {
    throw 'Phone weapon generation failed.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $gameFiles 'data') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'phone-weapon-config/gtasa_weapon_config.dat') -Destination (Join-Path $gameFiles 'data')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'phone-weapon-config/fastman92limitAdjuster_GTASA.ini') -Destination $gameFiles
# Modloader consumes these data lines from the readme; keep them when simplifying setup.
$weaponLines = @(
    'VALKYRIE PHONE - Requires Modloader 0.3.10 and fastman92 limit adjuster 7.6.',
    'IDE DATA\VALKYRIE-PHONE.IDE',
    "$([char]0xA3) VALKYRIEPHONE            MELEE 10.0  1.6  19990 -1  12 FLOWERS        1  1    null"
)
[IO.File]::WriteAllLines((Join-Path $weapon 'Valkyrie Phone.txt'), [string[]]$weaponLines, [Text.Encoding]::GetEncoding(28591))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'phone-content.json') -Destination $Output
Set-Content -LiteralPath (Join-Path $optional 'README.txt') -Encoding ASCII -Value @'
OPTIONAL PHONE AS A WEAPON - GTA SA 1.0 US

REQUIRES: Modloader 0.3.10 and fastman92 limit adjuster 7.6, already installed.
Downloads: https://github.com/thelink2012/modloader/releases/tag/v0.3.10
           https://www.fastman92.com/fastman92-limit-adjuster/

Close the game. Back up your existing fastman92limitAdjuster_GTASA.ini and
data/gtasa_weapon_config.dat. Copy everything inside 'Copy into game folder'
into the folder containing gta_sa.exe, replacing those two configuration files.
The supplied files already enable the loader and register the phone. No editing
is needed on a stock setup. Other custom weapon/limit configs need their own
settings retained; these replacement files configure stock SA plus the phone.

Scroll to the phone; right click raises/lowers it. Weapon 70, model 19990, slot 12.
To remove weapon mode, restore both backed-up configs and remove the
modloader/Valkyrie Phone folder. P-key phone and Maps do not need weapon mode.
Stock weapon configuration template: fastman92. Dependency binaries not bundled.
'@
Set-Content -LiteralPath (Join-Path $Output 'INSTALL.txt') -Encoding ASCII -Value @'
VALKYRIE PHONE - GTA SAN ANDREAS 1.0 US

Close the game. Copy valkyrie-phone.asi, valkyrie-phone.ini and the included
Valkyrie-radar-tiles folder into the game folder containing gta_sa.exe.
An x86 ASI loader is required. Preserve your existing phone INI when upgrading.
The map contains 142 generated stock SA tile pairs (284 .r3g/.r3a files).
Custom maps can use the included build-phone-map.ps1 to regenerate their tiles.
Press P to open the phone. Remove a separate valkyrie-trainer.asi if installed:
the phone embeds the trainer. Internet contains eight authored offline SA pages.

Phone-as-a-weapon is optional: read Optional - phone as a weapon/README.txt.
Its model and ready-to-copy replacement configs are included. Modloader and
fastman92 are requirements for weapon mode and are not bundled. GTA IV browser
pages remain personal local build inputs.
'@
"Verified $($manifest.tiles.Count) map files and staged ready-to-copy optional weapon configs."
