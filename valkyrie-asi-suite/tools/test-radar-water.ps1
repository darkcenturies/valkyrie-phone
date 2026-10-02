$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio x86 C++ Build Tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
New-Item -ItemType Directory -Force -Path (Join-Path $root 'build\water-tests') | Out-Null
$compile = "`"$vcvars`" x86 >nul && cd /d `"$root`" && cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /WX /Gy /Ivalkyrie-core\src /Ivalkyrie-radar\src /DRADAR_WATER_TEST_MAIN tools\test-radar-water.cpp /Fobuild\water-tests\ /Febuild\water-tests\test-radar-water.exe /link /OPT:REF"
cmd /c $compile
if ($LASTEXITCODE -ne 0) { throw 'Radar water test compilation failed.' }
& (Join-Path $root 'build\water-tests\test-radar-water.exe')
if ($LASTEXITCODE -ne 0) { throw 'Radar water regression test failed.' }
