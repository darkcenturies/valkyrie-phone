param(
    [Parameter(Mandatory=$true)][string]$Version,
    # Each edition's map, as a Valkyrie-radar-tiles folder. An edition whose
    # folder is not given is not made.
    [string]$EagleTiles,
    [string]$GtaSaTiles,
    [string]$PshTiles,
    # Only these editions (script, gtasa, project-eagle, project-silent-hill).
    [string[]]$Only
)
# The phone's release archives, from build\valkyrie-phone.asi:
#
#   valkyrie-phone-<version>-script.7z               the ASI, and the script that builds
#                                                    the map from your own game
#   valkyrie-phone-<version>-gtasa.7z                the ASI and GTA: San Andreas' map
#   valkyrie-phone-<version>-project-eagle.7z        the ASI and Project Eagle's map
#   valkyrie-phone-<version>-project-silent-hill.7z  the ASI and Project Silent Hill's
#                                                    map (handed to that project, not public)
#
# Each has the ASI, the ini as the phone ships it, a README, and the
# phone-as-a-weapon modloader folder set apart as optional, with the detailed
# HUD icon as an option of its own. The weapon folder must not be installed
# without its line in fastman92's weapon config.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$asi = Join-Path $build "valkyrie-phone.asi"
if (-not (Test-Path $asi)) { throw "no $asi - build the phone first" }
$sevenZip = "C:\Program Files\7-Zip\7z.exe"

$editions = @(
    @{ Id = "script"; Title = "ANY INSTALL - BUILD THE MAP YOURSELF"; Tiles = $null; Game = "GTA: San Andreas, Project Eagle or Project Silent Hill"; Folder = "the one with gta_sa.exe or gta_pe.exe" },
    @{ Id = "gtasa"; Title = "GTA: SAN ANDREAS"; Tiles = $GtaSaTiles; Game = "GTA: San Andreas"; Folder = "the one with gta_sa.exe" },
    @{ Id = "project-eagle"; Title = "PROJECT EAGLE"; Tiles = $EagleTiles; Game = "Project Eagle"; Folder = "the one with gta_pe.exe" },
    @{ Id = "project-silent-hill"; Title = "PROJECT SILENT HILL"; Tiles = $PshTiles; Game = "Project Silent Hill"; Folder = "the one with gta_sa.exe" }
)

function Write-Readme($path, $e) {
    $script = $e.Id -eq "script"
    $lines = @(
        "VALKYRIE PHONE $Version - $($e.Title)",
        "",
        "A working 2007 phone for $($e.Game). Press P to take it out.",
        "",
        "INSTALL",
        "",
        "1. Close the game.",
        "2. Copy valkyrie-phone.asi and valkyrie-phone.ini into the game folder",
        "   ($($e.Folder)). You need an ASI loader, as for any ASI mod."
    )
    if ($script) {
        $lines += @(
            "3. For the Maps app, build the map from your own game (Python 3 needed):",
            "     .\build-phone-map.ps1 -GamePath ""C:\path\to\the\game"" -Test",
            "     .\build-phone-map.ps1 -GamePath ""C:\path\to\the\game""",
            "   It writes the Valkyrie-radar-tiles folder into the game folder. It",
            "   takes a while and needs several GB free. Without it the phone works",
            "   and Maps shows no map.")
    } else {
        $lines += @(
            "3. Copy the Valkyrie-radar-tiles folder there too: it is the map the",
            "   Maps app draws. Without it the phone works and Maps shows no map.")
    }
    $lines += @(
        "4. If valkyrie-trainer.asi is in the game folder, remove it: the phone",
        "   carries the trainer (call Trainer in Contacts, or Alt+Z).",
        "",
        "USING IT",
        "",
        "P takes the phone out and puts it away. The mouse works the screen. With",
        "the phone as a weapon (below), scroll to it like a gun: it sits lowered",
        "at CJ's side, and right click brings it up and takes it down.",
        "",
        "Every setting is in valkyrie-phone.ini.",
        "",
        "OPTIONAL - THE PHONE AS A WEAPON",
        "",
        "The 'Optional - phone as a weapon' folder gives the phone a weapon slot of",
        "its own. It needs modloader and fastman92's limit adjuster with its weapon",
        "type loader (Project Eagle has both). Read the README in that folder: it",
        "takes two steps, and both are needed.",
        "",
        "IF SOMETHING GOES WRONG",
        "",
        "Send valkyrie-phone.log and valkyrie-phone.previous.log from the game folder.",
        "",
        "The source: https://github.com/darkcenturies/valkyrie-phone"
    )
    Set-Content -LiteralPath $path -Encoding ASCII -Value $lines
}

$made = @()
foreach ($e in $editions) {
    if ($Only -and $e.Id -notin $Only) { continue }
    if ($e.Id -ne "script" -and -not $e.Tiles) { Write-Host "[package] $($e.Id): no tiles given - skipped" -ForegroundColor Yellow; continue }
    if ($e.Tiles -and -not (Test-Path (Join-Path $e.Tiles "*.r3g"))) { throw "no tiles in $($e.Tiles)" }
    $name = "valkyrie-phone-$Version-$($e.Id)"
    $dir = Join-Path $build "packages\$name"
    if (Test-Path $dir) { cmd /c rd /s /q "$dir" }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item -LiteralPath $asi -Destination $dir

    # The ini as the phone writes it.
    python (Join-Path $root "tools\phone-default-ini.py") (Join-Path $dir "valkyrie-phone.ini") | Out-Null
    Write-Readme (Join-Path $dir "README.txt") $e
    if ($e.Id -eq "script") {
        Copy-Item -LiteralPath (Join-Path $root "tools\build-phone-map.ps1") -Destination $dir
    }

    # The weapon folder, set apart, with the detailed icon as its own option.
    $optional = Join-Path $dir "Optional - phone as a weapon"
    & (Join-Path $root "tools\make-phone-weapon.ps1") -Output (Join-Path $optional "Valkyrie Phone") | Out-Null
    $detailedDir = Join-Path $optional "Optional - detailed HUD icon"
    $stage = Join-Path ([IO.Path]::GetTempPath()) "valkyrie-phone-detailed-icon"
    & (Join-Path $root "tools\make-phone-weapon.ps1") -Output $stage -Icon "weapon_phone_eagle_detailed" | Out-Null
    New-Item -ItemType Directory -Force -Path $detailedDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $stage "valkyriephone.txd") -Destination $detailedDir
    Remove-Item -Recurse -Force $stage
    Set-Content -LiteralPath (Join-Path $detailedDir "README.txt") -Encoding ASCII -Value @(
        "THE DETAILED HUD ICON - OPTIONAL",
        "",
        "The phone's HUD weapon icon with its screen drawn in, instead of the plain",
        "one. Copy this valkyriephone.txd over the one in 'Valkyrie Phone', and set",
        "WeaponIcon=Detailed in valkyrie-phone.ini.")
    Set-Content -LiteralPath (Join-Path $optional "README.txt") -Encoding ASCII -Value @(
        "THE PHONE AS A WEAPON - OPTIONAL",
        "",
        "This gives the phone a weapon slot of its own (scroll to it like any",
        "weapon). It needs modloader and fastman92's limit adjuster with its weapon",
        "type loader - Project Eagle has both. It takes two steps, and both are",
        "needed:",
        "",
        "1. Put the 'Valkyrie Phone' folder in the game's modloader folder.",
        "2. Add the line in 'Valkyrie Phone\Valkyrie Phone.txt' to the very end",
        "   of data\gtasa_weapon_config.dat.",
        "",
        "Never do step 1 without step 2: the game would take the phone for CJ's",
        "fists. To take it out again, undo both.")

    if ($e.Tiles) {
        # Linked, not copied: the same gigabytes on disk until they are packed.
        New-Item -ItemType Directory -Path (Join-Path $dir "Valkyrie-radar-tiles") | Out-Null
        Get-ChildItem -LiteralPath $e.Tiles -File | ForEach-Object {
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
