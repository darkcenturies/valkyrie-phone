#include <plugin.h>
#include <common.h>
#include <CPad.h>
#include <CMessages.h>
#include <d3d9.h>
#include <safetyhook.hpp>
#include <string>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "Render.h"
#include "FaultGuard.h"
#include "Menu.h"
#include "ModelPreview.h"
#include "TrainerUI.h"
#include "Branding.h"
#include "AutoWalk.h"
#include "instrument.h"
#include "log.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Render
{
    static safetyhook::VmtHook g_vmt;
    static safetyhook::VmHook g_endSceneHook;
    static safetyhook::VmHook g_resetHook;
    static safetyhook::InlineHook g_updateMouseHook;
    static safetyhook::InlineHook g_cursorPositionHook;
    static safetyhook::InlineHook g_cursorClipHook;

    static int g_drawInstrument = instrument::kNoHook;
    static int g_mouseInstrument = instrument::kNoHook;
    static int g_resetInstrument = instrument::kNoHook;

    static HWND g_hWnd = nullptr;
    static WNDPROC g_origWndProc = nullptr;
    static bool g_imguiInit = false;
    static bool g_menuOpen = false;
    static bool g_faulted = false;
    static bool g_hookInstalled = false;
    static bool g_suppressNextMouseDelta = false;
    static RECT g_previousClip{};
    static bool g_restoreClip=false;
    static ImFont *g_titleFont = nullptr;

    ImFont *GetTitleFont() { return g_titleFont; }

    static bool OwnsCursor(){return g_menuOpen && g_hWnd && GetForegroundWindow()==g_hWnd;}
    static BOOL WINAPI HookedSetCursorPos(int x,int y){
        // Camera code and input plugins can warp the OS cursor independently
        // of CPad::UpdateMouse. ImGui reads this same cursor position.
        if(OwnsCursor())return TRUE;
        return g_cursorPositionHook.stdcall<BOOL>(x,y);
    }
    static BOOL WINAPI HookedClipCursor(const RECT* rect){
        if(OwnsCursor())return g_cursorClipHook.stdcall<BOOL>(static_cast<const RECT*>(nullptr));
        return g_cursorClipHook.stdcall<BOOL>(rect);
    }
    static void OpenMenu(){
        if(!g_imguiInit || !g_cursorPositionHook || !g_cursorClipHook || !g_updateMouseHook){
            logfile::Line("Menu could not open: input capture hooks unavailable.");
            CMessages::AddMessageJumpQ("Trainer input capture unavailable; check valkyrie-trainer.log",3000,0);
            return;
        }
        g_restoreClip=GetClipCursor(&g_previousClip)!=FALSE;
        g_menuOpen=true;
        ClipCursor(nullptr);
        ReleaseCapture();
    }

    static std::string GetExeDir()
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
        {
            path = path.substr(0, slash);
        }
        return path;
    }

    // CPad::UpdateMouse is __thiscall; a __fastcall destination with a dummy
    // second (edx) parameter matches its register usage (this in ecx).
    static void __fastcall HookedUpdateMouse(CPad *pad, void * /*edx*/)
    {
        instrument::HookFired(g_mouseInstrument);
        if (g_menuOpen)
        {
            // Hold the game's own mouse state at neutral so camera look and
            // weapon fire stop consuming the same raw motion/clicks ImGui
            // is using for the menu cursor.
            CPad::NewMouseControllerState={};
            CPad::OldMouseControllerState={};
            CPad::PCTempMouseControllerState={};
            return;
        }

        g_updateMouseHook.thiscall<void>(pad);

        if (g_suppressNextMouseDelta)
        {
            // The real update just resynced its internal "last cursor
            // position" against wherever the mouse ended up while the menu
            // was open (we'd been feeding it a neutral state, not moving
            // its tracked position). Without this, the very next real
            // update sees the whole accumulated on-screen mouse movement
            // as one huge delta and slams the camera. Eat just this one
            // frame's look delta so the camera stays where it was.
            CPad::NewMouseControllerState.x = 0;
            CPad::NewMouseControllerState.y = 0;
            g_suppressNextMouseDelta = false;
        }
    }

    bool Faulted() { return g_faulted; }
    void DisableAfterFault() { g_faulted=true; CloseMenu(); AutoWalk::Cancel(); logfile::Line("Trainer disabled after runtime fault; restart required."); }
    bool IsMenuOpen() { return g_menuOpen; }
    void CloseMenu() {
        const bool wasOpen=g_menuOpen;g_menuOpen=false;g_suppressNextMouseDelta=true;
        if(wasOpen){
            if(GetCapture()==g_hWnd)ReleaseCapture();
            // Never restore the game's confinement after Alt-Tab: that would
            // trap the cursor in the background window.
            if(g_restoreClip && GetForegroundWindow()==g_hWnd)ClipCursor(&g_previousClip);
        }
        g_restoreClip=false;
    }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if((msg==WM_ACTIVATEAPP && !wParam)||msg==WM_KILLFOCUS){g_restoreClip=false;CloseMenu();}
        if (!g_faulted && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == 'Z' && (lParam & 0x40000000) == 0)
        {
            if (GetAsyncKeyState(VK_MENU) & 0x8000)
            {
                if(g_menuOpen)CloseMenu();else OpenMenu();
                return 0;
            }
        }

        if (g_imguiInit && !g_faulted)
        {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        }

        if (g_menuOpen)
        {
            ImGuiIO &io = ImGui::GetIO();
            // Raw-input camera handlers must not receive menu motion either.
            // DefWindowProc performs Windows' WM_INPUT cleanup for us.
            if(msg==WM_INPUT)return DefWindowProc(hWnd,msg,wParam,lParam);
            // Never swallow release events: the game may have received the
            // matching press before this menu opened.
            bool blockKeyboard = io.WantCaptureKeyboard && (msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_SYSKEYDOWN);
            bool blockMouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
            if (blockKeyboard || blockMouse)
            {
                return true;
            }
        }

        return CallWindowProc(g_origWndProc, hWnd, msg, wParam, lParam);
    }

    static void SetupImGui(IDirect3DDevice9 *device)
    {
        D3DDEVICE_CREATION_PARAMETERS params{};
        device->GetCreationParameters(&params);
        g_hWnd = params.hFocusWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        Branding::SetDevice(device);
        TrainerUI::Setup(GetExeDir());
        g_titleFont=TrainerUI::TitleFont();

        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX9_Init(device);

        g_origWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);

        g_imguiInit = true;
    }

    static HRESULT __stdcall HookedReset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pp)
    {
        instrument::HookFired(g_resetInstrument);
        ModelPreview::InvalidateDeviceObjects();
        if (g_imguiInit)
        {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }
        HRESULT hr = g_resetHook.stdcall<HRESULT>(device, pp);
        if (g_imguiInit && SUCCEEDED(hr))
        {
            ImGui_ImplDX9_CreateDeviceObjects();
        }
        return hr;
    }

    // Last line of defense: if any menu action triggers a hardware
    // exception (access violation, divide-by-zero, etc) deep inside game
    // code we called into, catch it here instead of letting it take the
    // whole process down. This is a leaf call with no local C++ objects
    // needing unwind, so it's safe under /EHsc's __try restrictions.
    static void SafeDrawMenu()
    {
        __try
        {
            FaultGuard::stage="menu draw";Menu::Draw();
        }
        __except (FaultGuard::Report(GetExceptionInformation()))
        {
            DisableAfterFault();
            CMessages::AddMessageJumpQ(
                "Valkyrie Trainer: disabled after an internal error. Please restart the game.",
                4000, 0);
        }
    }

    static HRESULT __stdcall HookedEndScene(IDirect3DDevice9 *device)
    {
        if(g_faulted)return g_endSceneHook.stdcall<HRESULT>(device);
        instrument::HookFired(g_drawInstrument);
        if (!g_imguiInit)
        {
            SetupImGui(device);
        }

        ImGui::GetIO().MouseDrawCursor = g_menuOpen;

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ModelPreview::BeginFrame();
        if (g_menuOpen)
        {
            SafeDrawMenu();
            if(g_faulted)return g_endSceneHook.stdcall<HRESULT>(device);
        }
        ModelPreview::EndFrame();

        ImGui::EndFrame();
        ImGui::Render();
        if (auto *drawData = ImGui::GetDrawData())
        {
            ImGui_ImplDX9_RenderDrawData(drawData);
        }

        return g_endSceneHook.stdcall<HRESULT>(device);
    }

    static void TryInstallHook()
    {
        if (g_hookInstalled)
        {
            return;
        }

        if (!g_updateMouseHook)
        {
            if (auto hook = safetyhook::InlineHook::create(reinterpret_cast<void *>(0x53F3C0), reinterpret_cast<void *>(HookedUpdateMouse)))
            {
                g_updateMouseHook = std::move(*hook);
                g_mouseInstrument = instrument::RegisterHook("trainer mouse capture", 0x53F3C0, reinterpret_cast<void*>(HookedUpdateMouse));
            }
        }

        if(!g_cursorPositionHook){
            if(auto hook=safetyhook::InlineHook::create(reinterpret_cast<void*>(&SetCursorPos),reinterpret_cast<void*>(&HookedSetCursorPos)))g_cursorPositionHook=std::move(*hook);
        }
        if(!g_cursorClipHook){
            if(auto hook=safetyhook::InlineHook::create(reinterpret_cast<void*>(&ClipCursor),reinterpret_cast<void*>(&HookedClipCursor)))g_cursorClipHook=std::move(*hook);
        }
        logfile::Line("Input capture: mouse %d, cursor warp %d, cursor confinement %d",bool(g_updateMouseHook),bool(g_cursorPositionHook),bool(g_cursorClipHook));

        void *devicePtr = GetD3DDevice();
        if (!devicePtr)
        {
            return;
        }

        auto *device = reinterpret_cast<IDirect3DDevice9 *>(devicePtr);

        if (auto vmt = safetyhook::VmtHook::create(device))
        {
            g_vmt = std::move(*vmt);
        }
        else
        {
            return;
        }

        g_endSceneHook = safetyhook::create_vm(g_vmt, 42, HookedEndScene);
        g_resetHook = safetyhook::create_vm(g_vmt, 16, HookedReset);
        if (!g_endSceneHook.original<void*>() || !g_resetHook.original<void*>()) { logfile::Line("D3D hooks could not be installed"); return; }
        g_vmt.apply(device);
        auto** vtable = *reinterpret_cast<void***>(device);
        g_drawInstrument = instrument::RegisterHook("trainer model and menu draw", reinterpret_cast<uintptr_t>(&vtable[42]), reinterpret_cast<void*>(HookedEndScene));
        g_resetInstrument = instrument::RegisterHook("trainer device reset", reinterpret_cast<uintptr_t>(&vtable[16]), reinterpret_cast<void*>(HookedReset));
        logfile::Line("Trainer render/input hooks installed; live DFF/TXD preview enabled");

        g_hookInstalled = true;
    }

    void Init()
    {
        plugin::Events::gameProcessEvent += [] { TryInstallHook(); };
    }

    bool OpenMenuFromOutside()
    {
        if (g_faulted) return false;
        if (!g_menuOpen) OpenMenu();
        return g_menuOpen;
    }
}

// The phone's trainer number opens the menu through this (valkyrie-phone
// finds it with GetProcAddress, so neither ASI needs the other to load).
extern "C" __declspec(dllexport) BOOL __cdecl ValkyrieTrainerOpen()
{
    return Render::OpenMenuFromOutside() ? TRUE : FALSE;
}
