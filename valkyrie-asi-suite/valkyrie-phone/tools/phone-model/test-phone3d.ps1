param([string]$Out = (Join-Path $env:TEMP "phone3d-test"))
# Builds phone3d-test.exe and draws the phone with it, outside the game.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$phone = Resolve-Path (Join-Path $here "..\..")
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$exe = Join-Path $Out "phone3d-test.exe"
$cmd = "`"$vcvars`" x86 >nul && cl /nologo /EHsc /std:c++17 /O2 /W3 /Fo`"$Out\\`" /Fe`"$exe`" " +
       "`"$here\phone3d-test.cpp`" `"$phone\src\phone3d.cpp`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "phone3d-test did not build" }
& $exe (Join-Path $phone "assets") $Out
if ($LASTEXITCODE -ne 0) { throw "phone3d-test failed" }
