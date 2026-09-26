@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

if not defined VALKYRIE_CORE set "VALKYRIE_CORE=%~dp0..\valkyrie-asi-suite\valkyrie-core\src"
if not exist "%VALKYRIE_CORE%\instrument.cpp" (
 echo Valkyrie Core missing. Set VALKYRIE_CORE to the suite valkyrie-core\src directory.
 exit /b 1
)

set SDK=third_party\plugin-sdk
set SHARED=%SDK%\shared
set PLUGINSA=%SDK%\plugin_sa
set GAMESA=%SDK%\plugin_sa\game_sa

set IMGUI=third_party\imgui
set INC=/I"%VALKYRIE_CORE%" /I"%SHARED%" /I"%SHARED%\game" /I"%SDK%" /I"%PLUGINSA%" /I"%GAMESA%" /I"%GAMESA%\enums" /I"%GAMESA%\meta" /I"%GAMESA%\rw" /I"%SDK%\safetyhook" /I"%SDK%\injector" /I"%SDK%\hooking" /I"%SHARED%\dxsdk" /I"%IMGUI%" /I"%IMGUI%\backends"
set DEFS=/D GTASA /D RW /D PLUGIN_SGV_10US /D _USRDLL /D _WINDOWS /D WIN32 /D _CRT_SECURE_NO_WARNINGS /D _CRT_NON_CONFORMING_SWPRINTFS /D TARGET_NAME=\"valkyrie-trainer\" /D GTAGAME_NAME=\"SanAndreas\" /D GTAGAME_ABBR=\"SA\" /D GTAGAME_ABBRLOW=\"sa\" /D GTAGAME_PROTAGONISTNAME=\"CJ\" /D GTAGAME_CITYNAME=\"SanAndreas\"

set SRCS=src\Main.cpp src\Branding.cpp src\ActionBindings.cpp src\AutoWalk.cpp ^
 "%SHARED%\plugin.cpp" ^
 "%SHARED%\common_sdk.cpp" ^
 "%SHARED%\PluginBase.cpp" ^
 "%SHARED%\Patch.cpp" ^
 "%SHARED%\Pattern.cpp" ^
 "%SHARED%\DynAddress.cpp" ^
 "%SHARED%\GameVersion.cpp" ^
 "%SHARED%\Other.cpp" ^
 "%SHARED%\Color.cpp" ^
 "%SHARED%\StringUtils.cpp" ^
 "%SHARED%\extensions\ScriptCommands.cpp" ^
 "%SHARED%\extensions\Paths.cpp" ^
 "%SDK%\safetyhook\safetyhook.cpp" ^
 "%SDK%\safetyhook\Zydis.c" ^
 "%SHARED%\game\CRGBA.cpp" ^
 "%GAMESA%\CMessages.cpp" ^
 "%GAMESA%\RenderWare.cpp" ^
 "%GAMESA%\CCheat.cpp" ^
 "%GAMESA%\common.cpp" ^
 "%GAMESA%\CEntity.cpp" ^
 "%GAMESA%\CPed.cpp" ^
 "%GAMESA%\CClothes.cpp" ^
 "%GAMESA%\CPedClothesDesc.cpp" ^
 "%GAMESA%\CText.cpp" ^
 "%GAMESA%\CPlayerPed.cpp" ^
 "%GAMESA%\CPlaceable.cpp" ^
 "%GAMESA%\CAEAudioEntity.cpp" ^
 "%GAMESA%\CAESound.cpp" ^
 "%GAMESA%\CAEWeaponAudioEntity.cpp" ^
 "%GAMESA%\CRect.cpp" ^
 "%GAMESA%\CPad.cpp" ^
 "%GAMESA%\CTask.cpp" ^
 "%GAMESA%\CTaskSimple.cpp" ^
 "%GAMESA%\CTaskSimpleAnim.cpp" ^
 "%GAMESA%\CTaskSimpleRunNamedAnim.cpp" ^
 "%GAMESA%\CTaskManager.cpp" ^
 "%GAMESA%\CTaskTimer.cpp" ^
 "%GAMESA%\CPedIntelligence.cpp" ^
 "%GAMESA%\CAnimBlendAssociation.cpp" ^
 "%GAMESA%\CAnimManager.cpp" ^
 "%GAMESA%\CRunningScript.cpp" ^
 "%GAMESA%\CPools.cpp" ^
 "%GAMESA%\CStreaming.cpp" ^
 "%GAMESA%\CWorld.cpp" ^
 "%GAMESA%\CWaterLevel.cpp" ^
 "%GAMESA%\CTxdStore.cpp" ^
 "%GAMESA%\CVehicle.cpp" ^
 src\Render.cpp ^
 src\Menu.cpp ^
 src\TrainerUI.cpp ^
 src\VehicleList.cpp ^
 src\PedList.cpp ^
 src\AnimationList.cpp ^
 src\AnimationPlayer.cpp ^
 src\AnimationGame.cpp ^
 src\Hotkeys.cpp ^
 src\ModelPreview.cpp ^
 src\PreviewRenderer.cpp ^
 "%GAMESA%\CBaseModelInfo.cpp" ^
 "%GAMESA%\NodeName.cpp" ^
 "%VALKYRIE_CORE%\log.cpp" ^
 "%VALKYRIE_CORE%\instrument.cpp" ^
 "%IMGUI%\imgui.cpp" ^
 "%IMGUI%\imgui_draw.cpp" ^
 "%IMGUI%\imgui_widgets.cpp" ^
 "%IMGUI%\imgui_tables.cpp" ^
 "%IMGUI%\backends\imgui_impl_win32.cpp" ^
 "%IMGUI%\backends\imgui_impl_dx9.cpp"

rc.exe /nologo /fo bundle.res bundle.rc
if errorlevel 1 exit /b 1
cl.exe /nologo /MP4 /LD /EHsc /MT /std:c++latest /Zc:__cplusplus %DEFS% %INC% %SRCS% bundle.res /Fe:valkyrie-trainer.asi /link /MACHINE:X86 user32.lib gdi32.lib d3d9.lib d3dcompiler.lib gdiplus.lib shell32.lib

set "BUILD_RESULT=%ERRORLEVEL%"
endlocal & exit /b %BUILD_RESULT%
