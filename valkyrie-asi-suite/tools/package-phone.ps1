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
    # Both editions carry the stock map and ready-to-copy optional weapon configs.
    # The script edition additionally retains the builder for custom maps.
    & (Join-Path $root 'tools/stage-phone-content.ps1') -Output $dir
    if ($e.Tiles) {
        # Linked, not copied: the same gigabytes on disk until they are packed.
        Remove-Item -LiteralPath (Join-Path $dir 'valkyrie-radar-tiles') -Recurse -Force
        New-Item -ItemType Directory -Path (Join-Path $dir "valkyrie-radar-tiles") | Out-Null
        Get-ChildItem -LiteralPath $e.Tiles -File | ForEach-Object {
            New-Item -ItemType HardLink -Path (Join-Path $dir "valkyrie-radar-tiles\$($_.Name)") -Target $_.FullName | Out-Null
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
