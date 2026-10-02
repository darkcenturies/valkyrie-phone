param(
    [Parameter(Mandatory)] [string] $GamePath,
    [string] $WorkPath,
    [switch] $Test,
    [switch] $SkipInstall,
    [int] $Jobs = 4
)
$ErrorActionPreference = 'Stop'
$game = (Resolve-Path -LiteralPath $GamePath).Path
if (-not (Test-Path -LiteralPath (Join-Path $game 'gta_sa.exe'))) { throw 'Pass the GTA San Andreas 1.0 game folder.' }
if (-not $WorkPath) { throw 'Choose a work directory for the generated data.' }
# Sample and full runs must never reuse a partial bake or texture manifest.
$work = Join-Path $WorkPath $(if ($Test) { 'sample' } else { 'full' })
New-Item -ItemType Directory -Force -Path $work | Out-Null
$work = (Resolve-Path -LiteralPath $work).Path
$python = (Get-Command python -ErrorAction Stop).Source
& $python -c 'import PIL, numpy'
if ($LASTEXITCODE -ne 0) { throw 'Install the builder dependencies: python -m pip install Pillow numpy' }
function Stage([string] $Name, [string[]] $Arguments) {
    Write-Host "[phone map] $Name"
    & $python @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed." }
}
$index = Join-Path $work 'world-index.json'
$scene = Join-Path $work 'world-scene.json'
$tiles = Join-Path $work 'valkyrie-radar-tiles'
Stage 'Archive index' @((Join-Path $PSScriptRoot 'world3d-index.py'), $game, $index)
Stage 'World layout' @((Join-Path $PSScriptRoot 'gen-world-scene.py'), $game, $scene)
$region = @()
if ($Test) {
    $region = @('--region=1400,-1800,1900,-1300')
    $range = '2,3,-4,-3'
} else {
    $positions = (Get-Content -LiteralPath $scene -Raw | ConvertFrom-Json).instances
    if (-not $positions.Count) { throw 'No exterior placements found.' }
    $xs = $positions | ForEach-Object { [double]$_.p[0] } | Measure-Object -Minimum -Maximum
    $ys = $positions | ForEach-Object { [double]$_.p[1] } | Measure-Object -Minimum -Maximum
    $range = "$([Math]::Floor($xs.Minimum / 512) - 1),$([Math]::Floor($xs.Maximum / 512) + 1),$([Math]::Floor($ys.Minimum / 512) - 1),$([Math]::Floor($ys.Maximum / 512) + 1)"
}
Stage 'Textures' (@((Join-Path $PSScriptRoot 'world3d-textures.py'), $game, $index, $scene, $work) + $region)
Stage '3D tiles' (@((Join-Path $PSScriptRoot 'world3d-bake.py'), $scene, $index, $work, '--atlas', '--navigation', '--update', '--force') + $region)
Stage 'Phone tile format' @((Join-Path $PSScriptRoot 'world3d-radar-3dpack.py'), $work, $work, $tiles, "--range=$range", '--jobs', "$Jobs")
if (-not (Get-ChildItem -LiteralPath $tiles -Filter *.r3g -File).Count) { throw 'No radar geometry was generated.' }
if (-not $SkipInstall -and -not $Test) {
    if (Get-Process gta_sa,gta-sa -ErrorAction SilentlyContinue) { throw 'Map built. Close GTA San Andreas before installing it.' }
    $target = Join-Path $game 'valkyrie-radar-tiles'
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Get-ChildItem -LiteralPath $tiles -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $target $_.Name) -Force }
    Write-Host "Installed phone map data in $target."
} else {
    Write-Host "Map data is ready in $tiles. Sample runs are never installed."
}
