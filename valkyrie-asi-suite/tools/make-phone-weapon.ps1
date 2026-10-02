param(
    [Parameter(Mandatory=$true)][string]$Output
)
# The phone as a weapon of its own, as a modloader folder, laid out the way
# modloader adds to a game rather than replaces it:
#
# - its model and textures, with the HUD's icon among them (modloader - or
#   a compatible streaming loader - streams them);
# - its own object definitions, data\valkyrie-phone.ide, under a name of its
#   own: an .ide named after one of the game's is taken as a whole copy of
#   that file, and a copy with one line in it deletes all the others;
# - a readme with the two data lines modloader picks out of readmes and adds
#   to the game's: the gta.dat line that loads that .ide, and the weapon's
#   weapon.dat line. (A partial weapon.dat would, like the .ide, be taken as a
#   copy of the whole file with everything else deleted.)
#
# The one line fastman92's weapon type loader needs is in its own
# data\gtasa_weapon_config.dat, which modloader does not handle; the readme
# says where it goes.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$phone = Join-Path $root "valkyrie-phone"
$modelId = 19990       # Stock SA has 20000 model-info entries; 23900 is outside that array.
$name = "valkyriephone"

if (Test-Path -LiteralPath $Output) { Remove-Item -Recurse -Force -LiteralPath $Output }
New-Item -ItemType Directory -Force -Path (Join-Path $Output "data") | Out-Null
Copy-Item -LiteralPath (Join-Path $phone "assets\model\valkyrie-phone-model.dff") -Destination (Join-Path $Output "$name.dff") -Force

# The model's textures, and the HUD icon as <model>icon - where the game's
# HUD looks for a weapon's icon (Atmosphere's phone icon).
$stage = Join-Path ([IO.Path]::GetTempPath()) "valkyrie-phone-weapon-txd"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path $stage | Out-Null
Get-ChildItem -LiteralPath (Join-Path $phone "assets\model\textures") -Filter *.png | Copy-Item -Destination $stage
Copy-Item -LiteralPath (Join-Path $phone "assets\weapon-icon\phone.png") -Destination (Join-Path $stage "${name}icon.png")
& (Join-Path $root "tools\pack-phone-txd.ps1") -Source $stage -Output (Join-Path $Output "$name.txd") | Out-Null
Remove-Item -Recurse -Force $stage

# The phone's own definitions: the model, as the game's own phone (330) is
# declared in weapons.ide.
Set-Content -LiteralPath (Join-Path $Output "data\valkyrie-phone.ide") -Encoding ASCII -Value @(
    "# Valkyrie Phone",
    "weap",
    "$modelId, $name, $name, null, 1, 50, 0",
    "end")

# The readme: the lines modloader adds to gta.dat and weapon.dat, and what
# the player adds by hand. A held item in the detonator's slot (12), which
# almost never holds anything, swung like the flowers if clicked; melee
# lines start with a pound sign, as a Latin-1 byte.
$latin1 = [Text.Encoding]::GetEncoding(28591)
$pound = [char]0xA3
[IO.File]::WriteAllLines((Join-Path $Output "Valkyrie Phone.txt"), [string[]]@(
    "VALKYRIE PHONE - THE PHONE AS A WEAPON",
    "",
    "This folder goes in the game's modloader folder, as it is. It gives CJ's",
    "phone a weapon slot of its own: scroll to it like any weapon and it comes",
    "out; scroll away and it goes back in the pocket.",
    "",
    "Modloader adds these two lines to the game's own files by itself - they",
    "are here for it to find, and there is nothing to do with them:",
    "",
    "IDE DATA\VALKYRIE-PHONE.IDE",
    "$pound VALKYRIEPHONE            MELEE 10.0  1.6  $modelId -1  12 FLOWERS        1  1    null",
    "",
    "It needs fastman92's limit adjuster with its weapon type loader on.",
    "If it is disabled in your installation, add these lines to the end of",
    "fastman92limitAdjuster_GTASA.ini -",
    "",
    "[WEAPON LIMITS]",
    "Enable weapon type loader = 1",
    "Weapon type loader, number of type IDs = 80",
    "",
    "- and put fastman92's stock data\gtasa_weapon_config.dat in the game's",
    "data folder with one line added by hand at the very end of it.",
    "70 is its weapon ID; if another line",
    "already uses 70, any free number below the loader's limit works too:",
    "",
    "70    VALKYRIEPHONE      -1   1   0   1   97   194   247   1.0   FLOWERS",
    "",
    "Without that line the game does not know the weapon, and the phone works",
    "as it does without this folder: P takes it out."), $latin1)
Write-Host "[phone weapon] $Output"
