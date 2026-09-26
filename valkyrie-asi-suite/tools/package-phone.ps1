param(
    [Parameter(Mandatory=$true)][string]$Version,
    # The Project Eagle install whose Valkyrie-radar-tiles go in the map edition.
    [Parameter(Mandatory=$true)][string]$GamePath
)
# The phone's release archives for Project Eagle, from build\valkyrie-phone.asi:
#
#   valkyrie-phone-<version>-project-eagle.7z         the Maps app on, with the tiles
#   valkyrie-phone-<version>-project-eagle-no-map.7z  the Maps app off, no tiles
#
# Each has the ASI, the ini as the phone ships it (Maps on or off), a README,
# and the phone-as-a-weapon modloader folder set apart as optional: it must
# not be installed without its line in fastman92's weapon config (without it
# the game takes the phone's weapon.dat line for the fists').
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$asi = Join-Path $build "valkyrie-phone.asi"
if (-not (Test-Path $asi)) { throw "no $asi - build the phone first" }
$sevenZip = "C:\Program Files\7-Zip\7z.exe"
$tiles = Join-Path $GamePath "Valkyrie-radar-tiles"

function Write-Readme($path, [bool]$maps) {
    $mapLines = if ($maps) {
        @("- Valkyrie-radar-tiles: the Project Eagle map the Maps app draws.",
          "  Without it the phone works and Maps shows no map.")
    } else {
        @("This edition has the Maps app switched off and no map tiles, for a",
          "small download. The edition with the map has both; or set Maps=1 in",
          "valkyrie-phone.ini and add a Valkyrie-radar-tiles folder yourself.")
    }
    Set-Content -LiteralPath $path -Encoding ASCII -Value (@(
        "VALKYRIE PHONE $Version - PROJECT EAGLE$(if (-not $maps) { ' (NO MAP)' })",
        "",
        "INSTALL",
        "",
        "1. Close Project Eagle.",
        "2. Copy valkyrie-phone.asi and valkyrie-phone.ini$(if ($maps) { ' and the Valkyrie-radar-tiles folder' })",
        "   into your Project Eagle game folder (the one with gta_pe.exe).",
        "3. Remove valkyrie-trainer.asi$(if ($maps) { ' and valkyrie-radar.asi' }) from the game folder if",
        "   they are there: the phone carries the trainer$(if ($maps) { ' and the Maps app''s map' }) itself.",
        "4. Start the game and press P for the phone. With the phone as a weapon,",
        "   scroll to it: it sits lowered at CJ's side, and right click brings it",
        "   up to use and takes it down again.",
        "",
        "WHAT IS IN IT",
        "",
        "- valkyrie-phone.asi: the phone, with the Valkyrie trainer built in",
        "  (call Trainer on the phone, or Alt+Z).",
        "- valkyrie-phone.ini: every setting, as shipped.") + $mapLines + @(
        "- Optional - phone as a weapon: gives the phone a weapon slot of its own.",
        "  Read the README inside before installing it - it needs a line added",
        "  by hand as well.",
        "",
        "IF SOMETHING GOES WRONG",
        "",
        "Send valkyrie-phone.log and valkyrie-phone.previous.log from the game folder."))
}

$made = @()
foreach ($maps in $true, $false) {
    $name = "valkyrie-phone-$Version-project-eagle$(if (-not $maps) { '-no-map' })"
    $dir = Join-Path $build "packages\$name"
    if (Test-Path $dir) { cmd /c rd /s /q "$dir" }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item -LiteralPath $asi -Destination $dir

    # The ini as the phone writes it, with the Maps app on or off.
    $ini = Join-Path $dir "valkyrie-phone.ini"
    python (Join-Path $root "tools\phone-default-ini.py") $ini | Out-Null
    $text = [IO.File]::ReadAllText($ini)
    $text = [regex]::Replace($text, "(?m)^Maps=\d", "Maps=$(if ($maps) { 1 } else { 0 })")
    [IO.File]::WriteAllText($ini, $text, [Text.Encoding]::ASCII)

    Write-Readme (Join-Path $dir "README.txt") $maps

    # The weapon folder, set apart, with a README saying both halves are needed.
    $optional = Join-Path $dir "Optional - phone as a weapon"
    & (Join-Path $root "tools\make-phone-weapon.ps1") -Output (Join-Path $optional "Valkyrie Phone") | Out-Null
    Set-Content -LiteralPath (Join-Path $optional "README.txt") -Encoding ASCII -Value @(
        "THE PHONE AS A WEAPON - OPTIONAL",
        "",
        "This gives the phone a weapon slot of its own (scroll to it like any",
        "weapon). It takes two steps, and both are needed:",
        "",
        "1. Put the 'Valkyrie Phone' folder in the game's modloader folder.",
        "2. Add the line in 'Valkyrie Phone\Valkyrie Phone.txt' to the very end",
        "   of data\gtasa_weapon_config.dat.",
        "",
        "Never do step 1 without step 2: the game would take the phone for CJ's",
        "fists. To take it out again, undo both.")

    if ($maps) {
        if (-not (Test-Path $tiles)) { throw "no tiles at $tiles" }
        # Linked, not copied: the same 7 GB on disk until it is packed.
        New-Item -ItemType Directory -Path (Join-Path $dir "Valkyrie-radar-tiles") | Out-Null
        Get-ChildItem -LiteralPath $tiles -File | ForEach-Object {
            New-Item -ItemType HardLink -Path (Join-Path $dir "Valkyrie-radar-tiles\$($_.Name)") -Target $_.FullName | Out-Null
        }
    }

    $archive = Join-Path $build "packages\$name.7z"
    if (Test-Path $archive) { Remove-Item -Force $archive }
    & $sevenZip a -t7z -mx=5 -md=64m -mmt=8 $archive "$dir\*" -bsp0 -bso0
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed on $name" }
    & $sevenZip t $archive -bsp0 -bso0
    if ($LASTEXITCODE -ne 0) { throw "7-Zip's test failed on $name" }
    $made += Get-Item $archive
}
$made | ForEach-Object { "{0}  {1:N0} bytes" -f $_.FullName, $_.Length }
