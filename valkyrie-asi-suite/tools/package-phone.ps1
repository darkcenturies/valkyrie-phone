param(
    [Parameter(Mandatory=$true)][string]$Version,
    # Optional custom map folder; otherwise use the pinned stock SA map.
    [string]$GtaSaTiles,
    # Only these editions (script, gtasa).
    [string[]]$Only
)
# The phone's release archives, from build\valkyrie-phone.asi:
#
#   valkyrie-phone-<version>-script.7z               the ASI, and the script that builds
#                                                    the map from your own game
#   valkyrie-phone-<version>-gtasa.7z                the ASI and GTA: San Andreas' map
#
# Each has the ASI, the ini as the phone ships it, a README, and the
# phone-as-a-weapon modloader folder set apart as optional. The weapon folder must not be installed
# without its line in fastman92's weapon config.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$asi = Join-Path $build "valkyrie-phone.asi"
if (-not (Test-Path $asi)) { throw "no $asi - build the phone first" }
$sevenZip = "C:\Program Files\7-Zip\7z.exe"

$editions = @(
    @{ Id = "script"; Title = "ANY INSTALL - BUILD THE MAP YOURSELF"; Tiles = $null; Game = "GTA: San Andreas"; Folder = "the one with gta_sa.exe" },
    @{ Id = "gtasa"; Title = "GTA: SAN ANDREAS"; Tiles = $GtaSaTiles; Game = "GTA: San Andreas"; Folder = "the one with gta_sa.exe" }
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
        "its own. It requires Modloader 0.3.10 and fastman92 limit adjuster 7.6.",
        "With those installed, copy the contents of 'Copy into game folder' into",
        "the game folder, replacing the supplied configs. No editing on stock SA.",
        "Back up existing configs; custom weapon/limit settings need to be retained.",
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
    if ($e.Tiles -and -not (Test-Path (Join-Path $e.Tiles "*.r3g"))) { throw "no tiles in $($e.Tiles)" }
    $name = "valkyrie-phone-$Version-$($e.Id)"
    $dir = Join-Path $build "packages\$name"
    $packageRoot = [IO.Path]::GetFullPath((Join-Path $build 'packages')) + [IO.Path]::DirectorySeparatorChar
    $dir = [IO.Path]::GetFullPath($dir)
    if (-not $dir.StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Package path escapes build/packages.' }
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item -LiteralPath $asi -Destination $dir

    # The ini as the phone writes it.
    python (Join-Path $root "tools\phone-default-ini.py") (Join-Path $dir "valkyrie-phone.ini") | Out-Null
    Write-Readme (Join-Path $dir "README.txt") $e
    if ($e.Id -eq "script") {
        Copy-Item -LiteralPath (Join-Path $root "tools\build-phone-map.ps1") -Destination $dir
    }

    # Both editions carry the stock map and ready-to-copy optional weapon configs.
    # The script edition additionally retains the builder for custom maps.
    & (Join-Path $root 'tools/stage-phone-content.ps1') -Output $dir
    if ($e.Tiles) {
        # Linked, not copied: the same gigabytes on disk until they are packed.
        Remove-Item -LiteralPath (Join-Path $dir 'Valkyrie-radar-tiles') -Recurse -Force
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
