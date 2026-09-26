#include "input.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "game.h"
#include "instrument.h"
#include "log.h"

namespace input {
namespace {

// CPad::UpdatePads calls CPad::UpdateMouse for pad 0 from here. The five
// bytes are a relative call; its current destination is what we chain to, so
// another plugin that took the site first still runs.
constexpr uintptr_t kUpdateMouseCallSite = 0x541DD7;
constexpr uintptr_t kUpdateMouse = 0x53F3C0;

// CPad's mouse states: what UpdateMouse just read, and the frame before.
constexpr uintptr_t kNewMouseState = 0xB73418;
constexpr uintptr_t kOldMouseState = 0xB7342C;
constexpr size_t kMouseStateSize = 0x14;
// CMouseControllerState: bools at 0 (left), 1 (right), 3 (wheel up),
// 4 (wheel down); the movement is a CVector2D at 0x0C.
constexpr uintptr_t kMouseMoveX = kNewMouseState + 0x0C;
constexpr uintptr_t kMouseMoveY = kNewMouseState + 0x10;

// UpdateMouse multiplies the movement by these for the camera; the cursor
// wants the mouse's own direction back.
constexpr uintptr_t kInvertMouseX = 0xBA6744;
constexpr uintptr_t kInvertMouseY = 0xBA6745;

// RsGlobal.ps, whose first field is the game window.
constexpr uintptr_t kRsGlobalPs = 0xC17040 + 0x14;

using UpdateMouseFn = void(__fastcall*)(void* pad, void* edx);

UpdateMouseFn g_originalUpdateMouse = nullptr;
int g_mouseHookId = instrument::kNoHook;
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;
bool g_installed = false;
bool g_failed = false;

bool g_captured = false;
// Captured while a button was held (the click that opened whatever took the
// mouse): that press is nobody's - not the game's, not a tap - until it is let go.
bool g_heldBeforeCapture = false;
Point g_cursor{-1.0f, -1.0f};
// Mouse state gathered by the hook, handed out a frame at a time.
bool g_left = false, g_right = false;
bool g_prevLeft = false, g_prevRight = false;
bool g_frameLeft = false, g_frameRight = false, g_framePrevLeft = false, g_framePrevRight = false;
// The right button claimed from the uncaptured game (ClaimRight).
bool g_claimRight = false, g_claimedRight = false, g_claimedPrev = false;
int g_wheelAccum = 0, g_frameWheel = 0;

std::deque<Key> g_keys;
bool g_prevKeys[256] = {};

template <typename T>
T& At(uintptr_t address) {
    return *reinterpret_cast<T*>(address);
}

bool Foreground() {
    return g_window && GetForegroundWindow() == g_window;
}

void ClampCursor() {
    const game::Point screen = game::ScreenSize();
    if (g_cursor.x < 0.0f && g_cursor.y < 0.0f) {
        g_cursor = {screen.x * 0.5f, screen.y * 0.5f};
    }
    g_cursor.x = std::clamp(g_cursor.x, 0.0f, std::max(0.0f, screen.x - 1.0f));
    g_cursor.y = std::clamp(g_cursor.y, 0.0f, std::max(0.0f, screen.y - 1.0f));
}

void __fastcall UpdateMouseHook(void* pad, void* edx) {
    instrument::HookFired(g_mouseHookId);
    g_originalUpdateMouse(pad, edx);
    if (!g_captured) {
        if (g_claimRight) {
            g_claimedRight = At<bool>(kNewMouseState + 1);
            At<bool>(kNewMouseState + 1) = false;
            At<bool>(kOldMouseState + 1) = false;
        }
        return;
    }

    float dx = At<float>(kMouseMoveX);
    float dy = At<float>(kMouseMoveY);
    if (At<bool>(kInvertMouseX)) dx = -dx;
    if (At<bool>(kInvertMouseY)) dy = -dy;
    // DirectInput reports mickeys. On a 1080-line screen one mickey is about a
    // pixel at ordinary mouse speeds; scale with the screen so the cursor
    // crosses it in the same hand movement at any resolution.
    const float scale = std::max(0.5f, game::ScreenSize().y / 1080.0f);
    g_cursor.x += dx * scale;
    g_cursor.y += dy * scale;
    ClampCursor();

    g_left = At<bool>(kNewMouseState + 0);
    g_right = At<bool>(kNewMouseState + 1);
    if (g_heldBeforeCapture) {
        if (g_left || g_right) g_left = g_right = false;
        else g_heldBeforeCapture = false;
    }
    if (At<bool>(kNewMouseState + 3)) ++g_wheelAccum;
    if (At<bool>(kNewMouseState + 4)) --g_wheelAccum;

    // Leave the game a mouse that has not moved and is not pressed, this frame
    // and the one before, so nothing in it sees a click start or end.
    memset(reinterpret_cast<void*>(kNewMouseState), 0, kMouseStateSize);
    memset(reinterpret_cast<void*>(kOldMouseState), 0, kMouseStateSize);
}

bool IsControlKey(WPARAM vk) {
    switch (vk) {
        case VK_RETURN: case VK_BACK: case VK_ESCAPE: case VK_TAB: case VK_DELETE:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_captured) {
        switch (msg) {
            case WM_KEYDOWN:
                if (IsControlKey(wParam)) {
                    if (g_keys.size() < 64) g_keys.push_back({static_cast<int>(wParam), 0});
                }
                // Everything else arrives again as WM_CHAR if it types anything.
                return 0;
            case WM_CHAR:
                if (wParam >= 32 && wParam < 127) {
                    if (g_keys.size() < 64) g_keys.push_back({0, static_cast<char>(wParam)});
                }
                return 0;
            case WM_INPUT:
                // Raw-input camera mods must not see the cursor's movement.
                return DefWindowProcA(hwnd, msg, wParam, lParam);
            default:
                if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
                    return 0;
                }
                break;
        }
    }
    return CallWindowProcA(g_originalWndProc, hwnd, msg, wParam, lParam);
}

HWND FindGameWindow() {
    const uintptr_t ps = At<uintptr_t>(kRsGlobalPs);
    if (ps) {
        const HWND hwnd = At<HWND>(ps);
        DWORD pid = 0;
        if (hwnd && IsWindow(hwnd) && GetWindowThreadProcessId(hwnd, &pid) &&
            pid == GetCurrentProcessId()) {
            return hwnd;
        }
    }
    return nullptr;
}

template <typename Fn>
bool WithWritable(uintptr_t at, size_t len, Fn&& fn) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    fn();
    VirtualProtect(reinterpret_cast<void*>(at), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
    return true;
}

bool HookMouse() {
    HANDLE mutex = CreateMutexA(nullptr, FALSE, "Local\\SPRP.MouseHook");
    const DWORD wait = mutex ? WaitForSingleObject(mutex, 5000) : WAIT_FAILED;
    const bool held = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    bool ok = false;
    if (held) {
        const auto* site = reinterpret_cast<const uint8_t*>(kUpdateMouseCallSite);
        if (*site == 0xE8) {
            const int32_t rel = *reinterpret_cast<const int32_t*>(kUpdateMouseCallSite + 1);
            const uintptr_t target = kUpdateMouseCallSite + 5 + rel;
            MEMORY_BASIC_INFORMATION mbi{};
            const bool executable =
                VirtualQuery(reinterpret_cast<void*>(target), &mbi, sizeof(mbi)) &&
                mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY));
            if (target != reinterpret_cast<uintptr_t>(&UpdateMouseHook) && executable) {
                if (target != kUpdateMouse) {
                    logfile::Line("input: the mouse read is already hooked (%p) - chaining",
                                  reinterpret_cast<void*>(target));
                }
                g_originalUpdateMouse = reinterpret_cast<UpdateMouseFn>(target);
                const int32_t patched = static_cast<int32_t>(
                    reinterpret_cast<uintptr_t>(&UpdateMouseHook) - (kUpdateMouseCallSite + 5));
                ok = WithWritable(kUpdateMouseCallSite + 1, 4, [&] {
                    *reinterpret_cast<int32_t*>(kUpdateMouseCallSite + 1) = patched;
                });
                if (ok) {
                    g_mouseHookId = instrument::RegisterHook(
                        "the mouse read (CPad::UpdateMouse)", kUpdateMouseCallSite,
                        reinterpret_cast<const void*>(&UpdateMouseHook));
                }
            }
        } else {
            logfile::Line("input: the mouse call site does not look like a call (%02X)", *site);
        }
        ReleaseMutex(mutex);
    }
    if (mutex) CloseHandle(mutex);
    return ok;
}

}  // namespace

bool Install() {
    if (g_installed) {
        return true;
    }
    if (g_failed) {
        return false;
    }
    const HWND hwnd = FindGameWindow();
    if (!hwnd) {
        return false;  // not created yet; try again next frame
    }
    if (!HookMouse()) {
        logfile::Line("input: could not hook the mouse read - no cursor");
        g_failed = true;
        return false;
    }
    g_window = hwnd;
    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    if (!g_originalWndProc) {
        logfile::Line("input: could not take the window's messages (%lu)", GetLastError());
        g_failed = true;
        return false;
    }
    g_installed = true;
    logfile::Line("input: installed on window %p", hwnd);
    return true;
}

void Capture(bool on) {
    if (on == g_captured) {
        return;
    }
    g_captured = on;
    g_keys.clear();
    g_left = g_right = g_prevLeft = g_prevRight = false;
    g_wheelAccum = 0;
    if (on) {
        g_heldBeforeCapture = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000;
        ClampCursor();
    }
}

bool Captured() {
    return g_captured;
}

Point Cursor() {
    ClampCursor();
    return g_cursor;
}

void SetCursor(Point at) {
    g_cursor = at;
    ClampCursor();
}

void BeginFrame() {
    g_framePrevLeft = g_prevLeft;
    g_framePrevRight = g_prevRight;
    g_frameLeft = g_left;
    g_frameRight = g_right;
    g_prevLeft = g_left;
    g_prevRight = g_right;
    g_frameWheel = g_wheelAccum;
    g_wheelAccum = 0;
}

bool LeftHeld() { return g_captured && g_frameLeft; }
bool LeftPressed() { return g_captured && g_frameLeft && !g_framePrevLeft; }
bool LeftReleased() { return g_captured && !g_frameLeft && g_framePrevLeft; }
bool RightPressed() { return g_captured && g_frameRight && !g_framePrevRight; }
int Wheel() { return g_captured ? g_frameWheel : 0; }

void ClaimRight(bool on) {
    if (on == g_claimRight) return;
    g_claimRight = on;
    // Held when claimed (still down from before): not a press until let go.
    g_claimedRight = g_claimedPrev = on && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
}
bool RightClaimPressed() {
    const bool pressed = g_claimRight && !g_captured && g_claimedRight && !g_claimedPrev;
    g_claimedPrev = g_claimedRight;
    return pressed;
}

bool NextKey(Key& out) {
    if (g_keys.empty()) {
        return false;
    }
    out = g_keys.front();
    g_keys.pop_front();
    return true;
}

bool KeyDown(int vk) {
    return vk > 0 && vk < 256 && Foreground() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool KeyPressed(int vk) {
    if (vk <= 0 || vk >= 256) {
        return false;
    }
    const bool down = KeyDown(vk);
    const bool pressed = down && !g_prevKeys[vk];
    g_prevKeys[vk] = down;
    return pressed;
}

}  // namespace input
