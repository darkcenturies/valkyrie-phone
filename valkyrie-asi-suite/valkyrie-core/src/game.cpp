#include "game.h"
#include "layout_check.h"

#include <windows.h>

#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "instrument.h"
#include "log.h"
#include "radarcfg.h"
#include "radarbox.h"

namespace game {
namespace {

// GTA SA 1.0 US, HOODLUM. plugin-sdk identifies this build by the first four
// bytes of the entry section, which the unpacker stub leaves alone.
constexpr uintptr_t kVersionProbe = 0x401000;
constexpr uint32_t kHoodlum = 0x16197BE9;
constexpr uint32_t kCompact = 0x53EC8B55;

// CRadar, all cdecl.
//
// Two ways to plant a coordinate blip. The short range one is what the game
// uses for shops: on the radar only when you are near, on the full map always.
// That is the behaviour we want for four hundred businesses, and the long range
// one is kept for the few things worth seeing from across the state.
constexpr uintptr_t kSetCoordBlip = 0x583820;
constexpr uintptr_t kSetShortRangeCoordBlip = 0x583920;
constexpr uintptr_t kChangeBlipScale = 0x583CC0;
constexpr uintptr_t kSetBlipSprite = 0x583D70;
constexpr uintptr_t kClearBlip = 0x587CE0;
constexpr uintptr_t kDisplayThisBlip = 0x583B40;
constexpr int8_t kRadarSpriteNorth = 4;
constexpr int8_t kRadarSpriteWaypoint = 41;

// The call to CGame::Process inside Idle(). plugin-sdk hangs its
// gameProcessEvent on this exact call site, which makes it the best documented
// once-per-frame point in the game.
constexpr uintptr_t kFrameCallSite = 0x53E981;

// FrontEndMenuManager, and the fields in it we care about. The offsets are
// plugin-sdk's, which asserts them against the real structure layout.
constexpr uintptr_t kMenuManager = 0xBA6748;
constexpr uintptr_t kTargetBlipIndex = kMenuManager + 0x2C;
constexpr uintptr_t kDrawRadarOrMap = kMenuManager + 0x59;
constexpr uintptr_t kMenuActive = kMenuManager + 0x5C;
// The map's crosshair, in world coordinates - see the note in game.h.
constexpr uintptr_t kMapCursor = kMenuManager + 0x70;  // CVector2D

// The real screen cursor, as whole pixels.
constexpr uintptr_t kMouseScreenX = kMenuManager + 0xBC;
constexpr uintptr_t kMouseScreenY = kMenuManager + 0xC0;
constexpr uintptr_t kMapZoom = kMenuManager + 0x64;
constexpr uintptr_t kMapBaseX = kMenuManager + 0x68;
constexpr uintptr_t kMapBaseY = kMenuManager + 0x6C;
constexpr uintptr_t kCurrentMenuPage = kMenuManager + 0x15D;

// CRadar's live range and origin. Fastman's 48,000-unit map patch changes the
// range from the stock 2990 to an expanded-map value of 23920. The frontend map is never
// rotated, so using these values directly avoids the driving radar's cached
// heading that CRadar::TransformRealWorldPointToRadarSpace would apply here.
constexpr uintptr_t kRadarRange = 0xBA8314;
constexpr uintptr_t kRadarOrigin = 0xBAA248;  // CVector2D
constexpr uintptr_t kRadarAngle = 0xBA8310;

// The rotation CRadar itself applies going from world space to radar space -
// cos and sin of m_fRadarOrientation, refreshed once a frame by the game's own
// CalculateCachedSinCos. Two scalars beside kRadarAngle, not part of any array
// the limit adjuster resizes, so - like the three above - they hold still
// regardless of what it does to ms_RadarTrace. See
// RecoverBlipWorldPosition, which is the reason they are read here at all.
// CRadar::ShowRadarTraceWithHeight - what draws a spriteless marker, and the
// only place its colour is available without reading ms_RadarTrace. It is
// handed the colour as plain parameters, which is why the objective marker's
// exact yellow can be matched rather than guessed at.
constexpr uintptr_t kShowRadarTraceWithHeight = 0x584070;
// mov al, byte ptr [0x00BA67A1] - the stock entry, read out of this
// executable rather than guessed at (an earlier attempt at this hook assumed a
// six-byte prologue and was stood down before it could relocate half an
// instruction). It comes to exactly the five bytes a jump needs, so nothing is
// split. It is the same prologue CRadar::DrawRotatingRadarSprite opens with.
constexpr size_t kShowRadarTracePrologue = 5;

constexpr uintptr_t kRadarCachedCos = 0xBA8308;
constexpr uintptr_t kRadarCachedSin = 0xBA830C;

// CRadar::ms_RadarTrace is deliberately not addressed here.
//
// Its stock address, the entry stride and the waypoint sprite id were all
// written down at this point once, for a scan that looked for the player's
// waypoint in that array. The scan could never have worked: fastman92's limit
// adjuster raises the radar trace limit and relocates the array, so the stock
// address is dead memory and the scan silently found nothing, every time. The
// waypoint is read from CMenuManager instead, which is a static object and
// does not move. Leaving the addresses here invites the next person to try the
// same thing, so they are gone.
constexpr uintptr_t kFindPlayerVehicle = 0x56E0D0;

// CPad::NewMouseControllerState (CMouseControllerState). The wheel booleans
// are bytes 3 and 4 in the stock 0x14-byte structure; byte 0 is LMB.
constexpr uintptr_t kNewMouseState = 0xB73418;
constexpr uintptr_t kMouseLeft = kNewMouseState;
constexpr uintptr_t kMouseWheelUp = kNewMouseState + 3;
constexpr uintptr_t kMouseWheelDown = kNewMouseState + 4;

// CMenuManager::ProcessUserInput, GTA SA 1.0 US. The first two conditional
// branches enter native map zoom when the wheel byte is set. The last address
// begins GTA's 6000-unit-map origin clamp; we jump over that block while
// retaining its cursor-coordinate clamp at 0x578821.
constexpr uintptr_t kMapWheelUpBranch = 0x57762B;
constexpr uintptr_t kMapWheelDownBranch = 0x57795E;
constexpr uintptr_t kMapOriginClamp = 0x578785;
constexpr uintptr_t kMapOriginClampEnd = 0x578821;

// CRadar::DrawRadarSectionMap. ProperRadar patches an internal texture lookup
// at 0x586584; hooking the function entry lets the detailed path (and that
// existing patch) continue unchanged while replacing only the unstable full
// overview with one pre-composed texture.
constexpr uintptr_t kDrawRadarSectionMap = 0x586520;
constexpr size_t kDrawRadarSectionMapPrologue = 7;
constexpr uintptr_t kDrawRadarSection = 0x586110;
constexpr size_t kDrawRadarSectionPrologue = 6;
constexpr uintptr_t kTransformRadarPointToScreen = 0x583480;
constexpr uintptr_t kLimitRadarPoint = 0x5832F0;
constexpr size_t kLimitRadarPointPrologue = 5;
// mov al,[absolute] (5 bytes) + mov ecx,[esp+4] (4 bytes). A trampoline may
// only copy complete instructions; cutting the latter after its ModRM byte
// would make the pause-map fallback execute invalid machine code.
constexpr size_t kTransformRadarPointToScreenPrologue = 9;
// `mov al,[0xBA67A1]` is the opening instruction of
// TransformRadarPointToScreenSpace in this GTA SA executable. This is
// the frontend-map flag; using the nearby menu-manager field at 0xBA6767 made
// the validated stock transform look modified and left the 3D HUD inactive.
constexpr uintptr_t kDrawingMap = 0xBA67A1;
// At 650 the expanded map still asks ProperRadar to draw hundreds of sections
// in one frame; the zoom animation consequently flashes its black invalid
// tiles immediately before the overview takes over. Keep the composed atlas
// through the transition and hand back only once the view is genuinely close.
constexpr float kOverviewTextureMaxZoom = 1600.0f;

// CRadar::DrawRadarMask, GTA SA 1.0 US. plugin-sdk's own CRadar.cpp names this
// address for DrawRadarMask, and the independent gta-reversed decompilation
// vendored at reference/gta-reversed/source/game_sa/Radar.cpp:1168 shows the
// same address doing exactly what the name says: four TRIFAN quarter-circles,
// one per screen corner, alpha-blended over the square radar to cut it round.
// The function takes no arguments and returns nothing, so the only byte that
// matters is its first: a real function starts with a prologue, never a bare
// ret, so finding 0xC3 there already means a previous session (or another
// SP-RP ASI) patched it, not that this is the wrong address.
constexpr uintptr_t kDrawRadarMask = 0x585700;
// push ebp / mov ebp,esp / and esp,~7 - six bytes, all position independent,
// so they relocate into a trampoline without rewriting anything.
constexpr size_t kDrawRadarMaskPrologue = 6;

// CRadar::DrawRadarMap. DrawRadarMask is the very first call inside it
// (0x586887), before the map sections, the gang overlay and the blips - so
// that mask is not decoration over the top, it is the clip everything drawn
// afterwards depends on. Removing it is what let gang zones escape the radar
// and paint across the screen.
//
// Hooking the whole function gives somewhere to put a scissor rectangle
// around the radar box that lasts for every one of those draws and is taken
// down again immediately afterwards.
constexpr uintptr_t kDrawRadarMap = 0x586880;
// sub esp,0x2c / push ebx / push ebp - five bytes, position independent.
constexpr size_t kDrawRadarMapPrologue = 5;

// CHud::DrawRadar draws the stock circular bezel as four copies of
// SPRITE_RADAR_DISC. They are not part of CRadar::DrawRadarMap, so changing
// the map transform cannot move or reshape them.
constexpr uintptr_t kRadarDiscDrawCalls[] = {
    0x58A823, 0x58A8CD, 0x58A977, 0x58AA25,
};

// The game's live IDirect3DDevice9*, the same one Valkyrie Radar composites
// through.
constexpr uintptr_t kMainD3DDevice = 0xC97C28;

// CTxdStore / CSprite2d, GTA SA 1.0 US. These are the same plugin-sdk entry
// points used by the game's own frontend texture loader.
constexpr uintptr_t kTxdPushCurrent = 0x7316A0;
constexpr uintptr_t kTxdPopCurrent = 0x7316B0;
constexpr uintptr_t kTxdFindSlot = 0x731850;
constexpr uintptr_t kTxdSetCurrent = 0x7319C0;
constexpr uintptr_t kTxdAddRef = 0x731A00;
constexpr uintptr_t kTxdAddSlot = 0x731C80;
constexpr uintptr_t kTxdRemoveSlot = 0x731CD0;
constexpr uintptr_t kTxdLoadFile = 0x7320B0;
constexpr uintptr_t kRwTextureRead = 0x4C7510;
constexpr uintptr_t kSpriteDraw = 0x728350;
constexpr size_t kSpriteDrawPrologue = 5;
// The first HUD sprite lives at BAB208 in this executable. It is the textured
// radar-disc used by both stock CHud::DrawRadar and HUD hooks that replay a
// copied version of that routine.
constexpr uintptr_t kRadarDiscSprite = 0xBAB208;

// eMenuPage: the map is page 5. Checking the page rather than just "a menu is
// open" is what keeps the overlay off the pause menu, where it has no business
// being and covered half the buttons.
constexpr unsigned char kPageMap = 5;

// The call to the menu's own drawing, hooked after it runs so we paint on top
// of the map rather than under it. plugin-sdk calls this menuDrawingEvent and
// lists two call sites for San Andreas: one for the menu before a game has
// started, and this one for the pause menu during play. The pause menu is the
// only one with a map on it.
//
// It sits right beside the frame hook above, in the same Idle loop, which is a
// good sign it is the right one. The first address tried here came from the
// GTA 2 block at the top of plugin-sdk's Events.h rather than the San Andreas
// one, and pointed at a push instruction - caught only because the hook checks
// for a call opcode before patching anything.
constexpr uintptr_t kMenuDrawCallSite = 0x53EB8C;

// Where an overlay is drawn on the in-game HUD.
//
// Idle (after Render2dStuff and the frontend) has these live calls:
//
//   0053EBA2  call 0058D490   CHud::DrawAfterFade
//   0053EBA9  call 0069EFC0
//   0053EBB1  call 0071A210   CFont::DrawFonts
//
// The last one is the point, and it is NOT obvious from the names. CFont does
// not draw anything when it is asked to print - PrintString only queues into a
// buffer, and CFont::DrawFonts is what puts the whole buffer on screen. So the
// HUD's own ammo count and vehicle name are still sitting unrendered when
// DrawAfterFade returns.
//
// Hooking after DrawAfterFade, which is where this started, therefore lands in
// the middle of the game's text pass, and drawing there does two things wrong
// at once: setting up CFont for our own text restyles the game's queued
// strings, and flushing the buffer to get ours on screen renders the game's
// too - which the final text flush then renders a second time. The symptom is
// a black ammo readout and doubled text, and nothing of ours visible at all.
//
// Hooking the FLUSH and drawing after it is what makes an overlay independent
// of that: by then the buffer is empty, so our font state and our flush are
// only ever our own.
// Verified in the shipped gta_sa.exe and archived 1.0 US disassembly.
// Render2dStuff ends at 0x53E52B; 0x53E55F is not the live Idle path.
constexpr uintptr_t kHudAfterFadeCallSite = 0x53EBB1;
constexpr uintptr_t kFontDrawFontsTarget = 0x71A210;

// The vehicle the player is in, and the fields worth reading off it.
//
// Offsets are San Andreas 1.0 US and were read out of the MTA headers this
// repository already vendors under valkyrie-crashfix's third-party tree -
// CVehicleSA.h annotates m_nVehicleFlags as +1064 (0x428) and m_fGasPedal as
// +1180 (0x49C), and CEntitySA.h annotates m_nModelIndex as 34 (0x22). The
// two flag offsets were then cross-checked against plugin-sdk's CVehicle,
// whose field order between those two anchors lands m_pDriver on 0x460 and
// m_fHealth on 0x4C0 exactly, and whose tail fills sizeof(CVehicle) == 0x5A0
// with m_nVehicleClass at 0x590.
constexpr size_t kEntityModelIndex = 0x22;   // uint16
constexpr size_t kPhysicalMoveSpeed = 0x44;  // CVector, units per game step
constexpr size_t kVehicleFlags = 0x428;
constexpr size_t kVehicleCreationTime = 0x430;  // uint32, CTimer milliseconds
constexpr size_t kVehicleDriver = 0x460;     // CPed*
constexpr size_t kVehicleHealth = 0x4C0;     // float, 1000 = undamaged

// Bits inside m_nVehicleFlags, counted the way plugin-sdk lays the bitfield
// out: eight flags to a byte, in declaration order.
//
//   byte 0, bit 4  bEngineOn      "for sound purposes" - the game turns this
//                                 back on by itself whenever the driver is
//                                 holding the accelerator, so it is what the
//                                 engine SOUNDS like, not whether it runs.
//   byte 5, bit 1  bEngineBroken  "engine doesn't work. Player can get in but
//                                 the vehicle won't drive" - which is exactly
//                                 an empty tank, so this is the one that puts
//                                 a car out of fuel.
constexpr size_t kFlagEngineOnByte = 0;
constexpr uint8_t kFlagEngineOnMask = 0x10;
constexpr size_t kFlagEngineBrokenByte = 5;
constexpr uint8_t kFlagEngineBrokenMask = 0x02;

// The radar's world-to-screen pair. On the map screen the game sets these up
// for the map, so the same calls that place a blip place our cursor test.
constexpr uintptr_t kTransformRealWorldToRadar = 0x583530;
constexpr uintptr_t kTransformRadarToScreen = 0x583480;
constexpr uintptr_t kFindPlayerPed = 0x56E210;

// Text and rectangles.
constexpr uintptr_t kFontSetScale = 0x719380;
constexpr uintptr_t kFontSetColor = 0x719430;
constexpr uintptr_t kFontSetFontStyle = 0x719490;
constexpr uintptr_t kFontSetWrapx = 0x7194D0;
constexpr uintptr_t kFontSetRightJustifyWrap = 0x7194F0;
constexpr uintptr_t kFontSetDropColor = 0x719510;
constexpr uintptr_t kFontSetDropShadow = 0x719570;
constexpr uintptr_t kFontSetEdge = 0x719590;
constexpr uintptr_t kFontSetProportional = 0x7195B0;
constexpr uintptr_t kFontSetBackground = 0x7195C0;
constexpr uintptr_t kFontSetJustify = 0x719600;
constexpr uintptr_t kFontSetOrientation = 0x719610;
constexpr uintptr_t kFontPrintString = 0x71A700;
constexpr uintptr_t kFontGetStringWidth = 0x71A0E0;
constexpr uintptr_t kSpriteDrawRect = 0x727B60;

// Drawing a blip picture at a size of our choosing. The table address is the
// stock one and may have moved - see SpriteTableUsable, which checks before
// anything indexes it.
constexpr uintptr_t kDrawRotatingRadarSprite = 0x584850;
constexpr uintptr_t kRadarBlipSprites = 0xBAA250;
constexpr int kRadarSpriteCentre = 2;
constexpr size_t kDrawRotatingRadarSpritePrologue = 5;

// Common rectangle-overlay path used by GTA territories and multiplayer gang
// zones. Its first two instructions occupy eight bytes in total.
constexpr uintptr_t kDrawAreaOnRadar = 0x5853D0;
constexpr size_t kDrawAreaOnRadarPrologue = 8;

// The game's own "draw a blip picture" function. It has to index the picture
// table, so the table's address is a plain number inside its code - which is
// how we find it when it has been moved.
constexpr uintptr_t kDrawRadarSprite = 0x585FF0;
constexpr uintptr_t kDrawCoordBlip = 0x586D60;
constexpr size_t kDrawCoordBlipPrologue = 5;
constexpr uintptr_t kGetActualBlipArrayIndex = 0x582870;
constexpr size_t kScanBytes = 0x300;

// A CSprite2d is one texture pointer.
constexpr size_t kSpriteStride = 4;

// Eagle's table runs to 130; anything past that is certainly not ours.
constexpr int kMaxBlipSprites = 200;

// The flush.
//
// CFont::PrintString does not draw anything - it adds the text to a batch, and
// CFont::DrawFonts renders the batch. The menu calls that itself as the last
// thing it does, which is before we run, so text we queued afterwards sat in
// the buffer and was never painted. Panels appeared with nothing written on
// them and nothing said why.
constexpr uintptr_t kFontDrawFonts = 0x71A210;

// eFontAlignment: centre, left, right.
constexpr unsigned char kAlignLeft = 1;

// Font 1 is the game's plain menu face. Style 2 is the blocky one used for
// headings, which is unreadable at the size we need.
constexpr short kFontStyle = 1;

// Base size, in the 640x448 space the game's font is designed against. These
// are multiplied up by the real back buffer so the panel is the same size on
// any monitor rather than shrinking on a big one.
constexpr float kFontScaleX = 0.30f;
constexpr float kFontScaleY = 0.62f;

// Line spacing, in the same base space. Comfortably clear of the glyph height:
// text that overlaps is unreadable, text that is a little airy is not, so this
// errs the safe way. The first version of this had it far too tight and the
// legend came out as a solid black smear.
constexpr float kLineBase = 11.0f;

// RsGlobal - appName, then the back buffer size.
constexpr uintptr_t kRsGlobal = 0xC17040;
constexpr uintptr_t kScreenWidth = kRsGlobal + 0x4;
constexpr uintptr_t kScreenHeight = kRsGlobal + 0x8;

// The game's rectangle: x1, y1, x2, y2 in memory order.
struct Rect {
    float left, bottom, right, top;
};

struct Colour {
    uint8_t r, g, b, a;
};

struct Sprite {
    void* texture;
};

bool g_ready = false;
const char* g_version = "";
bool g_overviewDrawn = false;
bool g_overviewLoadTried = false;
void* g_overviewTexture = nullptr;
void* g_overviewQuadrants[4]{};
bool g_overviewFullDetail = false;
uint32_t g_overviewWaterArgb = 0xFF7486ADu;

using DrawRadarSectionMapFn = void(__cdecl*)(int, int, Rect);
DrawRadarSectionMapFn g_originalDrawRadarSectionMap = nullptr;
using DrawRadarSectionFn = void(__cdecl*)(int, int);
DrawRadarSectionFn g_originalDrawRadarSection = nullptr;
struct RadarPoint { float x, y; };
using TransformRadarPointToScreenFn = void(__cdecl*)(RadarPoint*, const RadarPoint*);
TransformRadarPointToScreenFn g_originalTransformRadarPointToScreen = nullptr;
using LimitRadarPointFn = float(__cdecl*)(RadarPoint*);
LimitRadarPointFn g_originalLimitRadarPoint = nullptr;
using SpriteDrawFn = void(__thiscall*)(Sprite*, const Rect&, const Colour&);
SpriteDrawFn g_originalSpriteDraw = nullptr;
// The 3D backdrop, marker transform, clipping rectangle and bezel must switch
// as one layout. Showing only some caused the rectangular map and old circle
// to overlap.
bool g_radarBoxHudActive = false;

HMODULE ValkyrieRadarModule() {
    if (HMODULE module = GetModuleHandleA("valkyrie-radar.asi")) return module;
    return GetModuleHandleA("sprp-radar3d.asi"); // legacy package name
}
using DisplayThisBlipFn = bool(__cdecl*)(int8_t, int8_t);
DisplayThisBlipFn g_originalDisplayThisBlip = nullptr;
using DrawRotatingRadarSpriteFn = void(__cdecl*)(Sprite*, float, float, float,
                                                  uint32_t, uint32_t, Colour);
DrawRotatingRadarSpriteFn g_originalDrawRotatingRadarSprite = nullptr;
bool g_pauseMapPlayerMarkerActive = false;
using DrawAreaOnRadarFn = void(__cdecl*)(const Rect*, const Colour*, bool);
DrawAreaOnRadarFn g_originalDrawAreaOnRadar = nullptr;

bool PlayerIsInVehicle() {
    return reinterpret_cast<void* (__cdecl*)(int, bool)>(
        kFindPlayerVehicle)(-1, false) != nullptr;
}

// Whether the Valkyrie panel is the radar right now, rather than GTA's own.
//
// Not the same question as "is the player driving". radar3d.cpp's Sample()
// lets ShowOnFoot keep the 3D capture running outside a vehicle, but every
// gate over here tested PlayerIsInVehicle() directly and went on suppressing
// the map pass, the blips and the arrow regardless - so the capture was taken
// and then thrown away, which is what the setting appearing to do nothing
// looked like. Both sides now read the same setting.
bool PanelShowsHere() {
    return PlayerIsInVehicle() || radarcfg::Current().showOnFoot;
}

// Set for exactly one CSprite2d::Draw call - see DisplayThisBlipReplacement.
bool g_northDrawPending = false;

// The objective marker - see game.h, ObjectiveBlip.
//
// Armed by DrawCoordBlipReplacement for the spriteless pass only, and read by
// the TransformRadarPointToScreenHelper call that CRadar::DrawCoordBlip makes
// from inside that very call, a few frames of stack below. The handoff is the
// same one g_blipTintPending already relies on, and it is safe for the same
// reason: it is all one synchronous call, and nothing else can transform a
// point in the middle of it.
//
// `g_objectiveSeen` is what says the transform actually happened. DrawCoordBlip
// gives up before reaching it in several ordinary cases - a contact point
// while a mission is running, a blip whose display flag says map-only - and
// without this the position from some earlier blip would be read as though it
// belonged to this one.
// Whether a marker draw is in progress, for the panel-edge clamp at the end of
// TransformRadarPointToScreenHelper.
//
// Set from DisplayThisBlipReplacement rather than from DrawCoordBlipReplacement
// because CRadar::DrawBlips calls DisplayThisBlip immediately before BOTH of
// its draw paths - DrawCoordBlip for a coord blip and DrawEntityBlip for a car,
// a ped, an object or a pickup. Arming it in DrawCoordBlipReplacement covered
// only the first, so every entity blip missed the clamp and ran off the panel
// edge to be sliced by the scissor, which is exactly what a row of parked cars
// looked like.
//
// The map tiles never call DisplayThisBlip, so they stay out of this on their
// own - which is the whole reason the clamp is gated at all.
bool g_blipClampArmed = false;

// Set for the one CSprite2d::Draw that is a map icon, so its rect can be
// resized by distance - see ReshapeBlipSpriteRect.
bool g_blipRectPending = false;

// The blip currently being drawn, in world coordinates, taken before the
// perimeter clamp destroys it - see LimitRadarPointToBoxHelper. This is what
// the panel projection needs, and the clamped vector the screen transform
// receives can no longer supply.
bool g_blipWorldValid = false;
float g_blipWorldX = 0.0f, g_blipWorldY = 0.0f;

bool g_objectiveArmed = false;
bool g_objectiveSeen = false;
float g_objectiveX = 0.0f, g_objectiveY = 0.0f;
DWORD g_objectiveAt = 0;
// The colour GTA draws that marker in, 0x00RRGGBB, or zero until one has been
// seen. The mission script chooses it, so it is read rather than assumed.
uint32_t g_objectiveColour = 0;

using ShowRadarTraceWithHeightFn = void(__cdecl*)(float, float, uint32_t, uint32_t,
                                                  uint32_t, uint32_t, uint32_t,
                                                  uint8_t);
ShowRadarTraceWithHeightFn g_originalShowRadarTraceWithHeight = nullptr;

void __cdecl ShowRadarTraceWithHeightReplacement(float x, float y, uint32_t size,
                                                 uint32_t r, uint32_t g,
                                                 uint32_t b, uint32_t a,
                                                 uint8_t height) {
    // Only while a spriteless coord blip is being drawn, which is the same
    // window the position is captured in - so the colour taken here belongs to
    // the marker whose position went out with it.
    if (g_objectiveArmed) {
        g_objectiveColour = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    }
    if (g_originalShowRadarTraceWithHeight) {
        g_originalShowRadarTraceWithHeight(x, y, size, r, g, b, a, height);
    }
}

// Recover a blip's world position from the radar-space vector CRadar itself
// handed to TransformRadarPointToScreenHelper, with no read of ms_RadarTrace.
//
// CRadar::TransformRealWorldPointToRadarSpace (0x583530) builds that vector as
// CachedRotateClockwise((world - origin) / range); CRadar's own inverse,
// TransformRadarPointToRealWorldSpace (0x5835A0, present in the reversed
// source but never called by the shipped game - "unused" there does not mean
// absent, but this reimplements the four multiplies and two adds rather than
// call an address nothing here has confirmed against this executable), is
// CachedRotateCounterclockwise(in) * range + origin. `in` has to be the
// vector exactly as CRadar produced it - before this file's own vehicle-
// heading re-rotation touches it - which is why the caller passes the
// function's own `in`, not the `radarX`/`radarY` locals.
//
// origin (vec2DRadarOrigin) is the local player's own position for the
// driving HUD, which is what makes it a ready sanity check: a genuine blip
// recovers to somewhere within the radar's range of it, and a wrong constant
// or a wrong sign recovers to something wildly outside that - the failure
// mode this stays a log line for until one has been read against a real
// mission and confirmed to look right.
bool RecoverBlipWorldPosition(const RadarPoint& in, float& worldX, float& worldY) {
    const float cachedCos = *reinterpret_cast<const float*>(kRadarCachedCos);
    const float cachedSin = *reinterpret_cast<const float*>(kRadarCachedSin);
    const float range = *reinterpret_cast<const float*>(kRadarRange);
    const float* origin = reinterpret_cast<const float*>(kRadarOrigin);
    if (!std::isfinite(cachedCos) || !std::isfinite(cachedSin) ||
        !std::isfinite(range) || range <= 0.0f ||
        !std::isfinite(origin[0]) || !std::isfinite(origin[1]) ||
        !std::isfinite(in.x) || !std::isfinite(in.y)) {
        return false;
    }
    worldX = (cachedCos * in.x - cachedSin * in.y) * range + origin[0];
    worldY = (cachedSin * in.x + cachedCos * in.y) * range + origin[1];
    return std::isfinite(worldX) && std::isfinite(worldY);
}

// Painting a marker in whose it is.
//
// The game will not do this for us. A blip carrying a sprite is drawn by
// CRadar::DrawRadarSprite as `RadarBlipSprites[id].Draw(rect, {255,255,255,
// alpha})` - flat white, every time - and the colour the blip was created with
// is only ever read on the plain square markers that have no sprite. So the
// colour we hand SetCoordBlip reaches the game and then goes nowhere.
//
// Rather than draw the sprites ourselves, the colour is put back at the last
// possible moment: CRadar::DrawCoordBlip says which blip is about to be drawn,
// and the CSprite2d::Draw that follows is that blip's. This is the same trick
// DisplayThisBlipReplacement uses for North, for the same reason - the sprite
// objects cannot be recognised by address, so the only way to know what a draw
// belongs to is to have been told on the way in.
//
// Keyed by the blip's index in the game's trace array rather than by its
// handle, because the index is what DrawCoordBlip is given. Nothing here reads
// that array: the limit adjuster relocates it, so a hardcoded read would be a
// read of dead memory. The index comes from the game's own accessor.
std::unordered_map<int, uint32_t> g_blipTints;
uint32_t g_blipTintPending = 0;

using DrawCoordBlipFn = void(__cdecl*)(int32_t, bool);
DrawCoordBlipFn g_originalDrawCoordBlip = nullptr;

int BlipArrayIndex(int blip) {
    if (!g_ready || blip == 0) return -1;
    using Fn = int(__cdecl*)(int);
    return reinterpret_cast<Fn>(kGetActualBlipArrayIndex)(blip);
}

// How far towards the edge of the radar the icon about to be drawn is, 0 at
// the player's own position and 1 at maximum range - the Euclidean length of
// the same radar-space vector TransformRadarPointToScreenHelper receives,
// which is already (worldPos - origin) / range before anything rotates it.
// Set there, on every call while the driving HUD is up, and read here a few
// instructions later for whichever sprite the transform was for: the same
// one-call handoff g_blipTintPending already relies on, just fed from the
// transform instead of from the blip index.
//
// Unlike the tint map this needs no per-blip identity at all - distance from
// the player is true of any point, looked up or not - so nothing has to
// resolve a blip index to use it.
float g_lastBlipRangeFraction = 0.0f;

// Whether the sprite currently between DrawCoordBlipReplacement and its
// matching CSprite2d::Draw should fade by distance at all. False for the
// plain square markers (a separate pass, never sprited) and for whichever
// blip DrawCoordBlipReplacement decides is exempt.
bool g_blipFadeArmed = false;

void __cdecl DrawCoordBlipReplacement(int32_t blipIndex, bool isSprite) {
    // Only the sprite pass draws a picture; the other pass draws the plain
    // square markers, which the game already colours by itself.
    if (isSprite) {
        const auto found = g_blipTints.find(static_cast<int>(blipIndex));
        g_blipTintPending = found == g_blipTints.end() ? 0 : found->second;
    }
    const bool drawingMap = *reinterpret_cast<const bool*>(kDrawingMap);
    const bool drivingHud = g_radarBoxHudActive && !drawingMap;
    // The fade is for icons. The objective marker carries no sprite, so it
    // takes the spriteless branch inside DrawCoordBlip and is drawn by
    // ShowRadarTraceWithHeight through CSprite2d::Draw2DPolygon - which is not
    // the hook the fade rides on. It is left alone here by construction rather
    // than by an exception, and it keeps the gentle distance fade CRadar's own
    // CalculateBlipAlpha already gives it.
    g_blipFadeArmed = isSprite && drivingHud && radarcfg::Current().fadeMapIcons;
    // That same spriteless pass is where the objective marker can be caught.
    g_objectiveArmed = !isSprite && drivingHud;
    g_objectiveSeen = false;
    if (g_originalDrawCoordBlip) {
        g_originalDrawCoordBlip(blipIndex, isSprite);
    }
    if (g_objectiveArmed && g_objectiveSeen) {
        g_objectiveAt = GetTickCount();
    }
    // Cleared unconditionally on the way out. A blip filtered away by
    // DisplayThisBlip never reaches a draw, and a tint or a fade left armed
    // would land on whatever sprite the game drew next.
    g_blipTintPending = 0;
    g_blipFadeArmed = false;
    g_objectiveArmed = false;
    g_blipClampArmed = false;
    g_blipWorldValid = false;
}

// Called for every CSprite2d::Draw while a tint or a fade is armed - see
// above. A tint replaces the colour outright; a fade only ever pulls alpha
// down from whatever the game (or the tint) already set, never up, so a blip
// already faint at range stays faint rather than being brightened back out.
void __cdecl TintBlipSpriteColour(Colour* colour) {
    if (!colour || (!g_blipTintPending && !g_blipFadeArmed)) return;
    if (g_blipTintPending) {
        const uint32_t tint = g_blipTintPending;
        g_blipTintPending = 0;
        colour->r = static_cast<uint8_t>((tint >> 16) & 0xFF);
        colour->g = static_cast<uint8_t>((tint >> 8) & 0xFF);
        colour->b = static_cast<uint8_t>(tint & 0xFF);
    }
    if (g_blipFadeArmed) {
        g_blipFadeArmed = false;
        // The rect rewrite runs after this in the same CSprite2d::Draw hook -
        // see SpriteDrawReplacement - and wants the same blip. Hand it on
        // rather than re-deriving which draw this is.
        g_blipRectPending = true;
        float fraction = g_lastBlipRangeFraction;
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 1.0f) fraction = 1.0f;
        const float minAlpha = static_cast<float>(radarcfg::Current().iconFadeMinAlpha);
        const float faded = colour->a + (minAlpha - colour->a) * fraction;
        colour->a = static_cast<uint8_t>(faded < 0.0f ? 0.0f
                                        : faded > 255.0f ? 255.0f : faded);
    }
}

bool __cdecl DisplayThisBlipReplacement(int8_t spriteId, int8_t priority) {
    // Keep the complete pause-map legend. What the driving HUD keeps is a
    // setting - see radarcfg.h, ShowMapIcons.
    //
    // With icons on, which is the default, the panel shows the same legend the
    // pause map does and the game's own visibility rules decide the rest: a
    // shop, a job, a property, a mission marker all appear on the panel as you
    // drive past them. With icons off the panel keeps only the north compass,
    // which is the minimal navigation display this started as.
    //
    // The waypoint sprite is left out either way. The 3D radar draws the
    // destination itself (DrawDestMarker in radar3d.cpp) as a dot pinned to
    // the panel edge, in the panel's own style and in the player's own colour,
    // so the stock diamond on top of it is the same marker drawn twice.
    //
    // The player arrow uses DrawRotatingRadarSprite and does not pass through
    // this filter.
    const bool drawingMap = *reinterpret_cast<const bool*>(kDrawingMap);
    const bool drivingHud = g_radarBoxHudActive && !drawingMap;
    const bool inVehicle = PanelShowsHere();
    if (drivingHud) {
        const bool keep = inVehicle &&
                          (spriteId == kRadarSpriteNorth ||
                           (radarcfg::Current().showMapIcons &&
                            spriteId != kRadarSpriteWaypoint));
        if (!keep) {
            g_northDrawPending = false;
            return false;
        }
    }
    const bool visible = g_originalDisplayThisBlip &&
                         g_originalDisplayThisBlip(spriteId, priority);
    // CRadar::DrawRadarSprite (0x585FF0) is
    //     if (DisplayThisBlip(spriteId, -99))
    //         RadarBlipSprites[spriteId].Draw({x-w, y-h, x+w, y+h}, ...)
    // so a true result here for North, at that function's distinctive -99
    // priority, means the very next CSprite2d::Draw is North's. Flagging it
    // here is how the sprite gets identified at draw time: the blip sprite
    // objects cannot be recognised by address (matching against
    // kRadarBlipSprites never fired - the live sprite pointers are heap
    // addresses, nothing like that static array), and DrawRadarSprite
    // hardcodes its own 8-unit half-size, so neither the size nor the
    // position can be changed anywhere but at the final draw.
    g_northDrawPending = visible && spriteId == kRadarSpriteNorth &&
                         priority == -99 && g_radarBoxHudActive &&
                         !drawingMap && inVehicle;
    // Arm the panel-edge clamp for the draw this call is about to allow. North
    // is left out by its distinctive -99 priority: ReshapeNorthSpriteRect
    // already places the compass by hand, and clamping it here as well would
    // be two things moving the same marker.
    g_blipClampArmed = visible && drivingHud && inVehicle && priority != -99;
    return visible;
}

void __cdecl DrawRotatingRadarSpriteReplacement(Sprite* sprite, float x,
                                                 float y, float angle,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 Colour colour) {
    // GTA normally uses `playerHeading - m_fRadarOrientation - PI` for this
    // sprite, which is camera-relative, so the arrow turns whenever the
    // camera orbits. Our map is vehicle-heading-up, so the arrow must stay
    // pointing up instead: -PI is that same formula with the heading already
    // aligned to the map's orientation.
    //
    // This deliberately does NOT test which sprite it is. An earlier version
    // gated on `sprite == &RadarBlipSprites[RADAR_SPRITE_CENTRE]`, which
    // never once matched - the live sprite pointers are heap addresses with
    // no relation to that static array (see RADAR3D-STATUS.md), so the clamp
    // silently never applied and the arrow kept following the camera. No test
    // is needed: CRadar has exactly two DrawRotatingRadarSprite call sites,
    // the player arrow (RADAR_SPRITE_CENTRE) and the pause map's blinking
    // "you are here" marker (RADAR_SPRITE_MAP_HERE), and the pause map is
    // already excluded by the kDrawingMap check below - so in the driving HUD
    // the only rotating sprite is the player arrow.
    const bool drawingMap = *reinterpret_cast<const bool*>(kDrawingMap);
    const bool drivingHud = g_radarBoxHudActive && !drawingMap;
    const bool inVehicle = PanelShowsHere();
    if (drivingHud && !inVehicle) {
        return;
    }
    // Locked pointing up only while the map is vehicle-heading-up. On foot
    // Sample() leaves the view on the gameplay camera, which is what GTA's own
    // camera-relative formula already assumes, so the arrow is left alone and
    // turns to show which way the player is facing.
    const bool inHudRadar = drivingHud && PlayerIsInVehicle();
    if (inHudRadar) {
        angle = -3.14159265358979323846f;
    }
    // The pause-map marker is projected from the same live view and the same
    // expanded-map transform as the map texture. GTA calculated the supplied
    // x/y before our smooth zoom/origin update, which is why it drifts.
    if (drawingMap && g_pauseMapPlayerMarkerActive) {
        Point player{};
        if (PlayerPosition(player)) {
            const Point fixed = WorldToMapScreen(player.x, player.y);
            const Point screen = ScreenSize();
            if (std::isfinite(fixed.x) && std::isfinite(fixed.y) &&
                screen.x > 0.0f && screen.y > 0.0f) {
                // DrawRotatingRadarSprite receives frontend reference-space
                // coordinates. WorldToMapScreen intentionally returns real
                // pixels for overlay drawing, so convert back exactly once.
                x = fixed.x * 640.0f / screen.x;
                y = fixed.y * 448.0f / screen.y;
            }
        }
    }
    // North's radar-space position comes from the same generic transform as
    // every blip (TransformRadarPointToScreenHelper), which places it
    // wherever the game's own north-direction coordinate normalises to -
    // that can land well inside the panel, not pinned to an edge, and it
    // still draws at the native circular radar's sprite size. The reference
    // layout wants it small and fixed at bottom-centre regardless of where
    // the generic transform put it, so it is special-cased here rather than
    // trying to steer the transform into producing that one fixed spot for
    // one specific sprite.
    if (g_originalDrawRotatingRadarSprite) {
        g_originalDrawRotatingRadarSprite(sprite, x, y, angle, width, height,
                                          colour);
    }
}

void __cdecl DrawAreaOnRadarReplacement(const Rect* rect,
                                         const Colour* colour, bool inMenu) {
    // Do not paint territory/gang-zone planes over the 3D navigation display.
    // The full pause map keeps its native overlays.
    if (g_radarBoxHudActive && !inMenu &&
        !*reinterpret_cast<const bool*>(kDrawingMap)) {
        return;
    }
    if (g_originalDrawAreaOnRadar) {
        g_originalDrawAreaOnRadar(rect, colour, inMenu);
    }
}

bool __cdecl ShouldHideRadarDisc(Sprite* sprite) {
    return g_radarBoxHudActive &&
           reinterpret_cast<uintptr_t>(sprite) == kRadarDiscSprite;
}

// North passes DisplayThisBlipReplacement's filter as an ordinary blip (see
// that function's comment: only the player arrow uses
// DrawRotatingRadarSprite), so it draws through this same CSprite2d::Draw
// call every other non-rotating blip does - not the rotating-sprite hook,
// which was the wrong target initially. The reference layout wants North
// small and fixed at bottom-centre regardless of whatever the generic
// TransformRadarPointToScreenHelper transform put in its Rect, so it is
// special-cased here by sprite pointer, the same way ShouldHideRadarDisc
// singles out the disc sprite.
void __cdecl ReshapeNorthSpriteRect(Sprite* sprite, Rect* rect) {
    (void)sprite;
    if (!rect || !g_northDrawPending) return;
    g_northDrawPending = false;
    const float screenW = static_cast<float>(*reinterpret_cast<const int*>(kScreenWidth));
    const float screenH = static_cast<float>(*reinterpret_cast<const int*>(kScreenHeight));
    if (screenW <= 0.0f || screenH <= 0.0f) return;
    const float sx = screenW / 640.0f;
    const float sy = screenH / 448.0f;

    // North is a *directional* indicator, not a world-anchored blip: GTA
    // feeds it a point far to the north and relies on the perimeter clamp to
    // slide it around the radar's edge as the vehicle turns. That is the
    // behaviour to keep - it must not be pinned anywhere.
    //
    // What it must not get is TransformRadarPointToScreenHelper's constant
    // kPlayerDropFraction offset. That offset exists to keep world-anchored
    // markers sitting on the road under the shifted 3D camera; applying it to
    // an already-perimeter-clamped point just pushes it down off the edge and
    // into the panel interior, which is exactly the "N is not at the edge"
    // symptom. Undo it here, then clamp back to the panel box so the marker
    // rides the perimeter.
    const float left = radarbox::Left() * sx;
    const float right = (radarbox::Left() + radarbox::Width()) * sx;
    const float bottom = screenH - radarbox::BottomMargin() * sy;
    const float top = bottom - radarbox::Height() * sy;
    const float drop = radarbox::Height() * radarbox::kPlayerDropFraction * sy;

    float cx = (rect->left + rect->right) * 0.5f;
    float cy = (rect->top + rect->bottom) * 0.5f - drop;
    if (cx < left) cx = left;
    if (cx > right) cx = right;
    if (cy < top) cy = top;
    if (cy > bottom) cy = bottom;

    // Fixed half-size in HUD units against DrawRadarSprite's hardcoded 8.
    // Both axes use the same pixel radius on purpose: GTA's own
    // SCREEN_STRETCH_X/Y pair gives a circle different horizontal and
    // vertical radii (on 1920x1080 that is 640ths vs 448ths, 3.0 against
    // 2.41), which is what made the round N sprite render as a squashed
    // oval. sy is the smaller scale, so this keeps it inside the box.
    // 3.5 matches the reference layout where N is the same apparent size
    // as a blip dot, not a large sprite.
    const float radius = 3.5f * sy;
    rect->left = cx - radius;
    rect->right = cx + radius;
    rect->top = cy - radius;
    rect->bottom = cy + radius;
}

// Draw a map icon smaller the further away it is.
//
// An icon's size is hardcoded inside CRadar::DrawRadarSprite as a
// floor(SCREEN_STRETCH_X(8.0f)) half-extent, so nothing upstream of the draw
// can change it - not the position transform, not the blip itself. The only
// place left is the rect the sprite is about to be drawn into, which is what
// the north compass is already resized through.
//
// Distance is the same fraction the fade uses, so an icon shrinks and dims
// together and reads as one thing receding rather than two effects.
void __cdecl ReshapeBlipSpriteRect(Sprite* sprite, Rect* rect) {
    (void)sprite;
    if (!rect || !g_blipRectPending) return;
    g_blipRectPending = false;
    const radarcfg::Settings& cfg = radarcfg::Current();
    if (cfg.iconMinScale >= 1.0f) return;
    float fraction = g_lastBlipRangeFraction;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    const float scale = 1.0f + (cfg.iconMinScale - 1.0f) * fraction;
    if (!(scale > 0.0f) || scale >= 1.0f) return;
    // Scale about the centre, so the icon shrinks towards the place it marks
    // rather than drifting off it.
    const float cx = (rect->left + rect->right) * 0.5f;
    const float cy = (rect->top + rect->bottom) * 0.5f;
    const float halfW = (rect->right - rect->left) * 0.5f * scale;
    const float halfH = (rect->bottom - rect->top) * 0.5f * scale;
    rect->left = cx - halfW;
    rect->right = cx + halfW;
    rect->top = cy - halfH;
    rect->bottom = cy + halfH;
}

// CSprite2d::Draw is a thiscall with two stack arguments. This catches the
// radar disc even when another HUD ASI copied CHud::DrawRadar into its own
// trampoline, while leaving every blip sprite and all unrelated HUD sprites
// alone.
__declspec(naked) void SpriteDrawReplacement() {
    __asm {
        push ecx
        push ecx
        call ShouldHideRadarDisc
        add esp, 4
        pop ecx
        test al, al
        jz notHidden
        ret 8
    notHidden:
        push ecx
        push dword ptr [esp + 12]
        call TintBlipSpriteColour
        add esp, 4
        pop ecx
        push ecx
        push dword ptr [esp + 8]
        push ecx
        call ReshapeNorthSpriteRect
        add esp, 8
        pop ecx
        push ecx
        push dword ptr [esp + 8]
        push ecx
        call ReshapeBlipSpriteRect
        add esp, 8
        pop ecx
        jmp dword ptr [g_originalSpriteDraw]
    }
}

bool Draw3DRadarBackground() {
    if (!g_radarBoxHudActive) return false;
    using Draw3DRadarFn = BOOL(__cdecl*)();
    static Draw3DRadarFn draw3D = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE module = ValkyrieRadarModule()) {
            draw3D = reinterpret_cast<Draw3DRadarFn>(
                GetProcAddress(module, "SprpRadar3DDrawBackground"));
        }
    }
    // A waypoint setter used to be resolved here as well, and never called.
    // The radar reads the waypoint from CMenuManager itself, on its own
    // schedule; being told about it a second time only gave that state a
    // second writer to disagree with.
    return draw3D && draw3D();
}

void DrawRouteDistance();

// Scan the radar blip table for an active GPS waypoint blip and write it to
// the 3D radar, which renders a flat ribbon in its perspective scene instead
// of another ASI's 2D overlay. Returns true if the waypoint was found and fed to
// the 3D radar (caller should suppress another ASI's own route draw).
// Whether the 3D radar holds a server-sent GPS route it can draw itself.
//
// This replaced a scan of the game's radar trace array. That scan could never
// have worked here: the limit adjuster raises the radar trace limit and moves
// the array, so the stock address holds dead memory and no waypoint was ever
// found.
//
// The waypoint is found again now, but from CMenuManager rather than from the
// trace array - that is a static object, so its fields do not move. See the
// GPS section of README.md and PollWaypoint in radar3d.cpp. Routing happens in
// Valkyrie Radar against its own copy of the road graph; the server supplies
// destinations the game cannot express, and no longer supplies routes.
void __cdecl TransformRadarPointToScreenHelper(RadarPoint* out,
                                                const RadarPoint* in) {
    if (!out || !in) return;
    if (!g_radarBoxHudActive || *reinterpret_cast<const bool*>(kDrawingMap)) {
        if (g_originalTransformRadarPointToScreen) {
            g_originalTransformRadarPointToScreen(out, in);
        }
        return;
    }
    // CHud draws blips after CRadar::DrawMap, so suppressing the map alone is
    // insufficient. Put every gameplay-radar marker outside the viewport
    // while on foot; the pause map took the native branch above and remains
    // fully functional.
    if (!PanelShowsHere()) {
        out->x = -10000.0f;
        out->y = -10000.0f;
        return;
    }
    // Feeds TintBlipSpriteColour, a few instructions and one function call
    // away from here - see g_lastBlipRangeFraction. `in` is (worldPos -
    // origin) / range already rotated, and rotation does not change a
    // vector's length, so its own length is that fraction with no further
    // work: 0 at the player, 1 at the edge of native radar range. The native
    // perimeter clamp keeps most points inside that, but a diagonal one can
    // still land fractionally past 1 before the consumer clamps it again.
    g_lastBlipRangeFraction = std::sqrt(in->x * in->x + in->y * in->y);
    const float screenW = static_cast<float>(*reinterpret_cast<const int*>(kScreenWidth));
    const float screenH = static_cast<float>(*reinterpret_cast<const int*>(kScreenHeight));
    if (screenW <= 0.0f || screenH <= 0.0f) return;
    const float sx = screenW / 640.0f;
    const float sy = screenH / 448.0f;
    float radarX = in->x;
    float radarY = in->y;

    // Native blip coordinates have already been rotated by GTA's cached
    // camera heading. The 3D capture uses vehicle heading while driving, so
    // rotate those coordinates by the difference or markers orbit away from
    // their roads whenever the player looks around the car.
    auto* vehicle = reinterpret_cast<uint8_t* (__cdecl*)(int, bool)>(
        kFindPlayerVehicle)(-1, false);
    if (vehicle) {
        const auto matrix = *reinterpret_cast<uintptr_t*>(vehicle + 0x14);
        if (matrix) {
            const float forwardX = *reinterpret_cast<const float*>(matrix + 0x10);
            const float forwardY = *reinterpret_cast<const float*>(matrix + 0x14);
            if (std::isfinite(forwardX) && std::isfinite(forwardY) &&
                forwardX * forwardX + forwardY * forwardY >= .0001f) {
                const float vehicleHeading = std::atan2(-forwardX, forwardY);
                const float nativeHeading = *reinterpret_cast<const float*>(kRadarAngle);
                const float delta = vehicleHeading - nativeHeading;
                const float sine = std::sin(delta);
                const float cosine = std::cos(delta);
                radarX = cosine * in->x + sine * in->y;
                radarY = cosine * in->y - sine * in->x;
            }
        }
    }

    // `in` was normalised by GTA using its raw speed-dependent range. Undo
    // the excess portion so markers, zones and route points remain aligned
    // with the capped 3D camera range.
    const float rawRange = *reinterpret_cast<const float*>(kRadarRange);
    const float effectiveRange = radarbox::EffectiveDrivingRange(rawRange);
    if (std::isfinite(rawRange) && std::isfinite(effectiveRange) &&
        rawRange > 0.0f && effectiveRange > 0.0f) {
        const float rangeCorrection = rawRange / effectiveRange;
        radarX *= rangeCorrection;
        radarY *= rangeCorrection;
    }

    // `edge > 1.0f` below is the only thing keeping any point inside the
    // panel. A NaN/Inf edge (from a NaN/Inf radarX/radarY that reached here
    // - a native sprite in a degenerate state, or a division upstream by a
    // near-zero range) makes every comparison against it false, so the
    // clamp silently does not fire and whatever garbage coordinate resulted
    // gets scaled straight into screen space: precisely a "renders way
    // outside the box" symptom with no obvious cause in this function's own
    // logic. Reject non-finite input the same way the on-foot path already
    // rejects everything - off-screen, not garbage-screen.
    if (!std::isfinite(radarX) || !std::isfinite(radarY)) {
        out->x = -10000.0f;
        out->y = -10000.0f;
        return;
    }
    float px = radarX / radarbox::kViewScale;
    float py = radarY / radarbox::kViewScale;
    const float ax = std::fabs(px), ay = std::fabs(py);
    const float edge = ax > ay ? ax : ay;
    if (edge > 1.0f) { px /= edge; py /= edge; }
    out->x = (radarbox::Left() + radarbox::Width() * 0.5f +
              radarbox::Width() * px * 0.5f) * sx;
    // The constant term shifts every point - player marker, blips, route -
    // down by the same fraction of panel height that valkyrie-radar.asi's 3D
    // camera shifts its captured background by (radarbox::kPlayerDropFraction).
    // It has to be a flat addition here, not scaled by py, because the 3D
    // side applies a view-space offset that is likewise constant regardless
    // of where in the world a point sits.
    out->y = screenH - radarbox::BottomOffset() * sy +
             radarbox::Height() * 0.5f * sy -
             radarbox::Height() * py * 0.5f * sy +
             radarbox::Height() * radarbox::kPlayerDropFraction * sy;

    // Keep a marker whole, and inside.
    //
    // Two things put one outside the panel. The perimeter clamp above works on
    // the point, so a marker beyond radar range gets its CENTRE placed exactly
    // on the boundary and half of it hangs over the edge, where the scissor
    // cuts it - a row of markers sliced down the middle along the rim. And the
    // player-drop term on the line above is added after that clamp, so it
    // shifts an already-clamped point off the edge entirely; a marker due
    // south ends up below the panel and is never seen at all.
    //
    // Both are fixed the same way, by clamping where it actually matters: in
    // panel space, after everything that moves the point, to the box pulled in
    // by the marker's own half-size so the whole marker fits. That is what
    // ReshapeNorthSpriteRect already does by hand for the compass, which is
    // why North is the one marker that has always ridden the rim properly.
    //
    // Only for blip draws. DrawCoordBlipReplacement arms these around exactly
    // the calls that draw a marker, and nothing else - the map tiles come
    // through this same transform, and clamping a tile corner into the panel
    // rect would fold the map up.
    if (!g_blipClampArmed) return;
    g_blipClampArmed = false;
    const bool haveWorld = g_blipWorldValid;
    const float blipWorldX = g_blipWorldX, blipWorldY = g_blipWorldY;
    g_blipWorldValid = false;

    const float inset = radarcfg::Current().iconEdgeInset;
    const float insetX = inset * sx, insetY = inset * sy;
    const float left = radarbox::Left() * sx;
    const float right = (radarbox::Left() + radarbox::Width()) * sx;
    const float bottom = screenH - radarbox::BottomMargin() * sy;
    const float top = bottom - radarbox::Height() * sy;
    const float halfW = (right - left) * 0.5f - insetX;
    const float halfH = (bottom - top) * 0.5f - insetY;
    if (!(halfW > 0.0f) || !(halfH > 0.0f)) return;
    const float cx = (left + right) * 0.5f;
    const float cy = (top + bottom) * 0.5f;

    // Slide the marker back along the line from the panel's centre until it
    // sits on the boundary, rather than clamping each axis on its own.
    //
    // Clamping x and y separately pins a marker to an edge but slides it along
    // that edge, so it stops pointing at the thing it marks - a place off to
    // the north-east ends up somewhere along the top with nothing to say which
    // way it really is. Scaling the whole offset keeps the bearing exact, which
    // is what makes a marker on the rim mean anything. It is the same
    // projection DrawDestMarker uses for the destination dot and the one the
    // game's own perimeter clamp does for North, which is why those two have
    // always ridden the edge properly and nothing else has.
    // Place the icon where the 3D panel actually shows that place.
    //
    // Everything above this point put it there with GTA's flat, top-down radar
    // transform, and the panel is not a flat top-down radar - it is a forward
    // perspective view. The two only agree approximately for what is ahead of
    // the player, and not at all for what is behind: a shop just driven past
    // is well inside radar range, so the flat transform drops it somewhere in
    // the middle of the panel, over ground it is nowhere near, while the scene
    // underneath does not show that place at all. No edge rule can rescue that,
    // because by the flat transform's reckoning the icon is not near an edge.
    //
    // The panel's own projection is the one that knows. It is the same one the
    // destination dot has always used, which is the whole reason that dot has
    // ridden the rim correctly while every native icon did not, and asking it
    // here is what finally puts the two on the same footing.
    if (haveWorld) {
        using ProjectFn = int(__cdecl*)(float, float, float*, float*);
        static ProjectFn project = nullptr;
        static bool resolved = false;
        if (!resolved) {
            resolved = true;
            if (HMODULE module = ValkyrieRadarModule()) {
                project = reinterpret_cast<ProjectFn>(
                    GetProcAddress(module, "SprpRadar3DProjectWorld"));
            }
        }
        float u = 0.0f, v = 0.0f;
        if (project && project(blipWorldX, blipWorldY, &u, &v)) {
            out->x = left + u * (right - left);
            out->y = top + v * (bottom - top);
        }
    }

    // Then hold it inside the panel, whole. A pinned icon's centre lands on
    // the boundary, so without this the half of it past the edge is cut off by
    // the scissor.
    const float ex = out->x - cx, ey = out->y - cy;
    const float reachX = std::fabs(ex) / halfW, reachY = std::fabs(ey) / halfH;
    const float reach = reachX > reachY ? reachX : reachY;
    if (reach > 1.0f) {
        out->x = cx + ex / reach;
        out->y = cy + ey / reach;
    }
}

float __cdecl LimitRadarPointToBoxHelper(RadarPoint* point) {
    if (!point) return 0.0f;
    // The pause map remains GTA's native map. The HUD uses a rectangle, so
    // limit directional points to a square in radar space before its separate
    // X/Y screen scales turn that square into the shared HUD rectangle.
    if (*reinterpret_cast<const bool*>(kDrawingMap)) {
        return g_originalLimitRadarPoint ? g_originalLimitRadarPoint(point) : 0.0f;
    }
    // Same non-finite-input gap as TransformRadarPointToScreenHelper: a NaN
    // or Inf point->x/y makes `perimeter > 1.0f` false, so the clamp below
    // never fires and the point is left wherever the game's own degenerate
    // value put it - unbounded, past this function's only safeguard.
    if (!std::isfinite(point->x) || !std::isfinite(point->y)) {
        point->x = 0.0f;
        point->y = 0.0f;
        return 0.0f;
    }
    // The objective marker's true position - see game.h, ObjectiveBlip.
    //
    // This is the only place it can be taken. CRadar::LimitRadarPoint clamps
    // the vector in place and tRadarTrace::GetRadarAndScreenPos calls it before
    // the screen transform, so by the time the transform sees the point, a
    // marker beyond radar range has already been pulled onto the perimeter -
    // and recovering a world position from that gives the point on the rim,
    // not the objective. That point slides along as the player drives, which
    // read as the destination creeping a few units every frame, a full route
    // rebuild for each creep, and a ribbon that flickered the whole way.
    //
    // Here the vector is still the real one, however far away it is.
    // Every blip's true position, for the panel projection - see
    // ProjectBlipThroughPanel. Taken here for the same reason the objective's
    // is: this is the last point at which the vector is still the real one.
    g_blipWorldValid = false;
    if (g_blipClampArmed) {
        float bx = 0.0f, by = 0.0f;
        if (RecoverBlipWorldPosition(*point, bx, by)) {
            g_blipWorldValid = true;
            g_blipWorldX = bx;
            g_blipWorldY = by;
        }
    }
    if (g_objectiveArmed && !g_objectiveSeen) {
        float worldX = 0.0f, worldY = 0.0f;
        if (RecoverBlipWorldPosition(*point, worldX, worldY)) {
            g_objectiveSeen = true;
            const bool moved = fabsf(worldX - g_objectiveX) > 1.0f ||
                               fabsf(worldY - g_objectiveY) > 1.0f;
            g_objectiveX = worldX;
            g_objectiveY = worldY;
            if (moved) {
                logfile::Line("radar: objective marker at %.1f, %.1f", worldX, worldY);
            }
        }
    }
    const float magnitude = std::sqrt(point->x * point->x + point->y * point->y);
    const float absX = std::fabs(point->x);
    const float absY = std::fabs(point->y);
    const float perimeter = absX > absY ? absX : absY;
    if (perimeter > 1.0f) {
        point->x /= perimeter;
        point->y /= perimeter;
    }

    // Let an icon that is outside the panel still be drawn, so it can ride the
    // rim pointing at where it is.
    //
    // This return value is what decides that. CRadar::DrawCoordBlip takes it as
    // the blip's distance and drops a short-range blip outright when it is over
    // 1 - `canBeDrawn = !m_bShortRange || zoomedDist <= 1.0f` - so a shop that
    // has gone out of range is never positioned at all, and no clamp further
    // down can bring it back. Nearly every icon on the map is short-range, and
    // that cull is the whole reason they vanish at the edge instead of wrapping
    // it the way North does.
    //
    // Reporting them as in range is what keeps them alive, and it has to be
    // bounded: every short-range blip in San Andreas answering at once is the
    // "ruinous for four hundred" this suite has already learned about. Inside
    // IconWrapRange an icon wraps; past it the game's own cull stands.
    const float wrapRange = radarcfg::Current().iconWrapRange;
    if (wrapRange > 0.0f && magnitude > 1.0f) {
        const float range = *reinterpret_cast<const float*>(kRadarRange);
        // `magnitude` is a share of the radar's own range, so the world
        // distance it stands for is that share of it.
        if (std::isfinite(range) && range > 0.0f &&
            magnitude * range <= wrapRange) {
            return 1.0f;
        }
    }
    return magnitude;
}

__declspec(naked) void __cdecl LimitRadarPointToBoxReplacement() {
    __asm {
        push dword ptr [esp + 4]
        call LimitRadarPointToBoxHelper
        add esp, 4
        ret
    }
}

// DrawRadarSection relies on the original transform preserving EDX.
__declspec(naked) void __cdecl TransformRadarPointToScreenReplacement() {
    __asm {
        push edx
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        call TransformRadarPointToScreenHelper
        add esp, 8
        pop edx
        ret
    }
}

void* LoadOverviewTexture() {
    // Never put an Eagle atlas over the stock San Andreas map, even if a
    // stale expanded-map overview happens to remain beside the executable.
    if (!ExpandedMapActive()) return nullptr;
    if (g_overviewLoadTried) {
        return g_overviewTexture;
    }
    g_overviewLoadTried = true;

    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) {
        logfile::Line("map overview: cannot locate gta_sa.exe");
        return nullptr;
    }
    char* slash = strrchr(path, '\\');
    if (!slash) {
        logfile::Line("map overview: game path has no directory");
        return nullptr;
    }
    slash[1] = '\0';
    char gameDirectory[MAX_PATH]{};
    strcpy_s(gameDirectory, path);
    char radarPath[MAX_PATH]{};
    strcpy_s(radarPath, path);
    strncat_s(radarPath, "ProperRadarSA.txd", _TRUNCATE);

    // Mod Loader redirects GTA's own texture read, not this ASI's Win32 file
    // read. Detect any immediate Mod Loader pack containing ProperRadarSA.txd;
    // users do not have to rename third-party packs for Valkyrie to recognise
    // their selected artwork.
    char modPattern[MAX_PATH]{};
    strcpy_s(modPattern, path);
    strncat_s(modPattern, "modloader\\*", _TRUNCATE);
    WIN32_FIND_DATAA found{};
    HANDLE search = FindFirstFileA(modPattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                found.cFileName[0] == '.') continue;
            char candidate[MAX_PATH]{};
            strcpy_s(candidate, path);
            strncat_s(candidate, "modloader\\", _TRUNCATE);
            strncat_s(candidate, found.cFileName, _TRUNCATE);
            strncat_s(candidate, "\\ProperRadarSA.txd", _TRUNCATE);
            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                strcpy_s(radarPath, candidate);
                logfile::Line("map overview: using Mod Loader atlas %s",
                              found.cFileName);
                break;
            }
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }

    // Every supported visual pack uses the same 96x96 layout but different
    // pixels. Select the composed overview by the installed atlas checksum so
    // changing Blue/Red/Dark/Cool/etc. cannot leave a mismatched distant map.
    uint32_t crc = 0xFFFFFFFFu;
    if (FILE* file = nullptr; fopen_s(&file, radarPath, "rb") == 0 && file) {
        unsigned char buffer[16384];
        size_t read = 0;
        while ((read = fread(buffer, 1, sizeof(buffer), file)) != 0) {
            for (size_t i = 0; i < read; ++i) {
                crc ^= buffer[i];
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xEDB88320u &
                          (0u - (crc & 1u)));
            }
        }
        fclose(file);
        crc ^= 0xFFFFFFFFu;
    } else {
        crc = 0;
    }

    // The composed map cannot cover a widescreen frontend at the furthest
    // zoom without distorting the atlas. Paint its own edge-water colour
    // underneath it instead of exposing the frontend theme on either side.
    // Values are decoded from the top-left DXT1 texel of each supported pack.
    switch (crc) {
        case 0x16D3DE74u: g_overviewWaterArgb = 0xFF3A4967u; break; // HD
        case 0x1E397573u: g_overviewWaterArgb = 0xFF7435D6u; break; // Cool 1
        case 0x410E105Fu: g_overviewWaterArgb = 0xFF000000u; break; // Dark 2
        case 0x4698FD25u: g_overviewWaterArgb = 0xFF000000u; break; // Dark 1
        case 0x5E4BD7B8u: g_overviewWaterArgb = 0xFF424284u; break; // Cool 2
        case 0xA479F6BBu: g_overviewWaterArgb = 0xFF7486ADu; break; // Ingame
        case 0xBEACCBD4u: g_overviewWaterArgb = 0xFF000000u; break; // Lock
        case 0xC755F44Cu: g_overviewWaterArgb = 0xFF7C1100u; break; // Red
        case 0xE22CFD2Au: g_overviewWaterArgb = 0xFF000464u; break; // Blue
        default:          g_overviewWaterArgb = 0xFF7486ADu; break;
    }

    // Public asset names describe the map pack they belong to. The CRC is an
    // internal discriminator, not useful information to someone installing or
    // reporting a problem with the release.
    const char* overviewStyle = nullptr;
    switch (crc) {
        case 0x16D3DE74u: overviewStyle = "hd"; break;
        case 0x1E397573u: overviewStyle = "cool-1"; break;
        case 0x410E105Fu: overviewStyle = "dark-2"; break;
        case 0x4698FD25u: overviewStyle = "dark-1"; break;
        case 0x5E4BD7B8u: overviewStyle = "cool-2"; break;
        case 0xA479F6BBu: overviewStyle = "ingame"; break;
        case 0xBEACCBD4u: overviewStyle = "lock"; break;
        case 0xC755F44Cu: overviewStyle = "red"; break;
        case 0xE22CFD2Au: overviewStyle = "blue"; break;
    }

    char overviewName[64]{};
    bool highResolutionOverview = false;
    if (auto* device = *reinterpret_cast<IDirect3DDevice9**>(kMainD3DDevice)) {
        D3DCAPS9 caps{};
        if (SUCCEEDED(device->GetDeviceCaps(&caps)) &&
            caps.MaxTextureWidth >= 12288 && caps.MaxTextureHeight >= 12288) {
            _snprintf_s(overviewName, sizeof(overviewName), _TRUNCATE,
                        "Valkyrie-map-overview-hi-%08X.txd", crc);
            highResolutionOverview = true;
        }
    }
    strcpy_s(path, radarPath);
    slash = strrchr(path, '\\');
    slash[1] = '\0';
    strncat_s(path, overviewName, _TRUNCATE);
    if (!highResolutionOverview ||
        GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        highResolutionOverview = false;
        if (overviewStyle)
            _snprintf_s(overviewName, sizeof(overviewName), _TRUNCATE,
                        "Valkyrie-map-overview-%s.txd", overviewStyle);
        else
            _snprintf_s(overviewName, sizeof(overviewName), _TRUNCATE,
                        "Valkyrie-map-overview-%08X.txd", crc);
        slash[1] = '\0';
        strncat_s(path, overviewName, _TRUNCATE);
    }
    // Accept the CRC names used by the earlier test build.
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES && overviewStyle) {
        _snprintf_s(overviewName, sizeof(overviewName), _TRUNCATE,
                    "Valkyrie-map-overview-%08X.txd", crc);
        slash[1] = '\0';
        strncat_s(path, overviewName, _TRUNCATE);
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        slash[1] = '\0';
        strncat_s(path, "Valkyrie-map-overview.txd", _TRUNCATE);
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        slash[1] = '\0';
        strncat_s(path, "sprp-map-overview.txd", _TRUNCATE);
    }
    // Matching overview assets ship beside valkyrie-map.asi. A Mod Loader
    // radar pack normally contains only the original pack files, so fall back
    // to the game directory after using its ProperRadarSA.txd for detection.
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        if (highResolutionOverview) {
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "%sValkyrie-map-overview-hi-%08X.txd",
                        gameDirectory, crc);
        }
        if (!highResolutionOverview ||
            GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            if (overviewStyle)
                _snprintf_s(path, sizeof(path), _TRUNCATE,
                            "%sValkyrie-map-overview-%s.txd",
                            gameDirectory, overviewStyle);
            else
                _snprintf_s(path, sizeof(path), _TRUNCATE,
                            "%sValkyrie-map-overview-%08X.txd",
                            gameDirectory, crc);
        }
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES && overviewStyle)
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "%sValkyrie-map-overview-%08X.txd",
                        gameDirectory, crc);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "%sValkyrie-map-overview.txd", gameDirectory);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "%ssprp-map-overview.txd", gameDirectory);
    }

    using FindSlot = int(__cdecl*)(const char*);
    using AddSlot = int(__cdecl*)(const char*);
    using LoadFile = bool(__cdecl*)(int, const char*);
    using SlotFn = void(__cdecl*)(int);
    using ReadTexture = void*(__cdecl*)(const char*, const char*);

    // The full expanded map is 24576 square, larger than one D3D9 texture.
    // Prefer four lossless 12288 quadrants when present. They preserve every
    // source DXT block; the older single 12288 atlas remains a compatibility
    // fallback for packages or hardware without the quadrant set.
    if (highResolutionOverview) {
        char quadrantDirectory[MAX_PATH]{};
        strcpy_s(quadrantDirectory, radarPath);
        char* quadrantSlash = strrchr(quadrantDirectory, '\\');
        if (quadrantSlash) quadrantSlash[1] = '\0';
        char probe[MAX_PATH]{};
        _snprintf_s(probe, sizeof(probe), _TRUNCATE,
                    "%sValkyrie-map-overview-full-%08X-q0.txd",
                    quadrantDirectory, crc);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
            strcpy_s(quadrantDirectory, gameDirectory);
            _snprintf_s(probe, sizeof(probe), _TRUNCATE,
                        "%sValkyrie-map-overview-full-%08X-q0.txd",
                        quadrantDirectory, crc);
        }

        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            bool complete = true;
            for (int q = 0; q < 4; ++q) {
                char slotName[32]{};
                char textureName[32]{};
                char quadrantPath[MAX_PATH]{};
                _snprintf_s(slotName, sizeof(slotName), _TRUNCATE,
                            "sprp_map_overview_q%d", q);
                _snprintf_s(textureName, sizeof(textureName), _TRUNCATE,
                            "sprp_overview_q%d", q);
                _snprintf_s(quadrantPath, sizeof(quadrantPath), _TRUNCATE,
                            "%sValkyrie-map-overview-full-%08X-q%d.txd",
                            quadrantDirectory, crc, q);
                int quadrantSlot = reinterpret_cast<FindSlot>(kTxdFindSlot)(slotName);
                const bool quadrantAdded = quadrantSlot < 0;
                if (quadrantAdded)
                    quadrantSlot = reinterpret_cast<AddSlot>(kTxdAddSlot)(slotName);
                if (quadrantSlot < 0 ||
                    (quadrantAdded && !reinterpret_cast<LoadFile>(kTxdLoadFile)(quadrantSlot, quadrantPath))) {
                    complete = false;
                    break;
                }
                reinterpret_cast<SlotFn>(kTxdAddRef)(quadrantSlot);
                reinterpret_cast<void(__cdecl*)()>(kTxdPushCurrent)();
                reinterpret_cast<SlotFn>(kTxdSetCurrent)(quadrantSlot);
                g_overviewQuadrants[q] =
                    reinterpret_cast<ReadTexture>(kRwTextureRead)(textureName, nullptr);
                reinterpret_cast<void(__cdecl*)()>(kTxdPopCurrent)();
                if (!g_overviewQuadrants[q]) {
                    complete = false;
                    break;
                }
            }
            if (complete) {
                g_overviewTexture = g_overviewQuadrants[0];
                g_overviewFullDetail = true;
                logfile::Line("map overview: four full-detail 12288x12288 quadrants loaded; no zoom-out quality reduction");
                return g_overviewTexture;
            }
            logfile::Line("map overview: full-detail quadrant set incomplete; using single-texture fallback");
        }
    }

    int slot = reinterpret_cast<FindSlot>(kTxdFindSlot)("sprp_map_overview");
    const bool added = slot < 0;
    if (added) {
        slot = reinterpret_cast<AddSlot>(kTxdAddSlot)("sprp_map_overview");
    }
    if (slot < 0 ||
        (added && !reinterpret_cast<LoadFile>(kTxdLoadFile)(slot, path))) {
        if (slot >= 0 && added) {
            reinterpret_cast<SlotFn>(kTxdRemoveSlot)(slot);
        }
        logfile::Line("map overview: could not load %s - retaining ProperRadar tiles", path);
        return nullptr;
    }

    reinterpret_cast<SlotFn>(kTxdAddRef)(slot);
    reinterpret_cast<void(__cdecl*)()>(kTxdPushCurrent)();
    reinterpret_cast<SlotFn>(kTxdSetCurrent)(slot);
    g_overviewTexture =
        reinterpret_cast<ReadTexture>(kRwTextureRead)("sprp_overview", nullptr);
    reinterpret_cast<void(__cdecl*)()>(kTxdPopCurrent)();

    if (!g_overviewTexture) {
        logfile::Line("map overview: TXD loaded but sprp_overview was not readable");
        return nullptr;
    }
    logfile::Line("map overview: single %s texture loaded; ProperRadar tile storm bypassed below %.0f zoom",
                  highResolutionOverview ? "12288x12288 HD" : "3072x3072 fallback",
                  kOverviewTextureMaxZoom);
    return g_overviewTexture;
}

void DrawOverview() {
    void* texture = LoadOverviewTexture();
    if (!texture) {
        return;
    }

    const MapView view = Map();
    const Point screen = ScreenSize();
    if (!std::isfinite(view.zoom) || !std::isfinite(view.baseX) ||
        !std::isfinite(view.baseY) || screen.x <= 0.0f || screen.y <= 0.0f) {
        return;
    }

    DrawRect(0.0f, 0.0f, screen.x, screen.y, g_overviewWaterArgb);

    const float sx = screen.x / 640.0f;
    const float sy = screen.y / 448.0f;
    const Rect rect{
        (view.baseX - view.zoom) * sx,
        (view.baseY + view.zoom) * sy,
        (view.baseX + view.zoom) * sx,
        (view.baseY - view.zoom) * sy,
    };
    const Colour white{255, 255, 255, 255};
    if (g_overviewFullDetail) {
        const float middleX = (rect.left + rect.right) * 0.5f;
        const float middleY = (rect.top + rect.bottom) * 0.5f;
        const Rect quadrants[4]{
            {rect.left,  middleY,     middleX,    rect.top},
            {middleX,    middleY,     rect.right, rect.top},
            {rect.left,  rect.bottom, middleX,    middleY},
            {middleX,    rect.bottom, rect.right, middleY},
        };
        using Draw = void(__thiscall*)(Sprite*, const Rect&, const Colour&);
        for (int q = 0; q < 4; ++q) {
            Sprite quadrant{g_overviewQuadrants[q]};
            reinterpret_cast<Draw>(kSpriteDraw)(&quadrant, quadrants[q], white);
        }
        return;
    }
    Sprite sprite{texture};
    using Draw = void(__thiscall*)(Sprite*, const Rect&, const Colour&);
    reinterpret_cast<Draw>(kSpriteDraw)(&sprite, rect, white);
}

void __cdecl DrawRadarSectionMapReplacement(int x, int y, Rect rect) {
    if (Map().zoom <= kOverviewTextureMaxZoom && LoadOverviewTexture()) {
        if (!g_overviewDrawn) {
            DrawOverview();
            g_overviewDrawn = true;
        }
        return;
    }
    // The native tiled renderer is only safe while looking closely at the
    // map.  It may expose black out-of-atlas columns on an expanded atlas,
    // so record that transition explicitly for diagnostics.
    static bool loggedDetailedFallback = false;
    if (!loggedDetailedFallback) {
        logfile::Line("map overview: native detailed tiles at zoom %.1f (overview cutoff %.1f)",
                      Map().zoom, kOverviewTextureMaxZoom);
        loggedDetailedFallback = true;
    }
    if (g_originalDrawRadarSectionMap) {
        g_originalDrawRadarSectionMap(x, y, rect);
    }
}

using DrawRadarMaskFn = void(__cdecl*)();
DrawRadarMaskFn g_originalDrawRadarMask = nullptr;
using DrawRadarMapFn = void(__cdecl*)();
DrawRadarMapFn g_originalDrawRadarMap = nullptr;
DWORD g_savedScissorEnable = 0;
RECT g_savedScissor{};
bool g_scissorHeld = false;
bool RadarBoxPixels(RECT& out);
void __cdecl DrawRadarMaskReplacement() {
    // Only the in-game HUD radar goes square. Anything drawn while a menu is
    // up - the pause map, the join-sequence map preview - keeps the mask it
    // relies on to stay inside its own box.
    //
    // Skipping it during gameplay is safe only because DrawRadarMapReplacement
    // has put a scissor rectangle up first; without that, everything this was
    // clipping draws unbounded.
    if ((!g_radarBoxHudActive || MenuIsOpen()) && g_originalDrawRadarMask) {
        g_originalDrawRadarMask();
    }
}

// The radar box in real pixels. Must stay the same box Valkyrie Radar draws its
// rounded rectangle into, or the 3D image and the things GTA paints on top of
// it would be clipped to different shapes.
bool RadarBoxPixels(RECT& out) {
    if (!g_radarBoxHudActive) return false;
    const int screenW = *reinterpret_cast<const int*>(kScreenWidth);
    const int screenH = *reinterpret_cast<const int*>(kScreenHeight);
    if (screenW <= 0 || screenH <= 0) {
        return false;
    }
    const float sx = static_cast<float>(screenW) / 640.0f;
    const float sy = static_cast<float>(screenH) / 448.0f;
    const float left = radarbox::Left() * sx;
    const float width = radarbox::Width() * sx;
    const float height = radarbox::Height() * sy;
    const float bottom = static_cast<float>(screenH) - radarbox::BottomMargin() * sy;
    out.left = static_cast<LONG>(left);
    out.right = static_cast<LONG>(left + width);
    out.top = static_cast<LONG>(bottom - height);
    out.bottom = static_cast<LONG>(bottom);
    if (out.left < 0) out.left = 0;
    if (out.top < 0) out.top = 0;
    if (out.right > screenW) out.right = screenW;
    if (out.bottom > screenH) out.bottom = screenH;
    return out.right > out.left && out.bottom > out.top;
}

void __cdecl DrawRadarMapReplacement() {
    // Run before the on-foot return and during the pause map too. Present is
    // bypassed by some HUD mods; navigation must not depend on that callback.
    using TickRadarFn = void(__cdecl*)();
    static TickRadarFn tickRadar = nullptr;
    if (!tickRadar) {
        if (HMODULE module = ValkyrieRadarModule())
            tickRadar = reinterpret_cast<TickRadarFn>(
                GetProcAddress(module, "SprpRadar3DTickNavigation"));
    }
    if (tickRadar) tickRadar();
    // The GTA VI-style navigation panel is a driving aid. On foot suppress
    // the whole map pass (terrain, zones and route); DrawBlips is handled by
    // the gameplay transform above because CHud invokes it separately.
    if (!MenuIsOpen() && !PanelShowsHere()) {
        return;
    }
    g_scissorHeld = false;
    auto* device = *reinterpret_cast<IDirect3DDevice9**>(kMainD3DDevice);
    RECT box{};
    if (device && !MenuIsOpen() && RadarBoxPixels(box)) {
        // Whatever the game had is put back afterwards - this is inside its
        // own frame, not a place to leave render state altered.
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &g_savedScissorEnable);
        device->GetScissorRect(&g_savedScissor);
        if (SUCCEEDED(device->SetScissorRect(&box))) {
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
            g_scissorHeld = true;
        }
    }

    const bool vehicleHud = !MenuIsOpen() && PanelShowsHere();

    // Suppress native radar sections during the driving pass. The Valkyrie
    // composite must be the last background painted: drawing it from a section
    // callback (or before DrawRadarMap completes) lets ProperRadar paint over
    // it later in the same pass, producing a one-frame flash and then an empty
    // panel.
    if (vehicleHud) {
        g_overviewDrawn = true;
    }

    if (g_originalDrawRadarMap) {
        g_originalDrawRadarMap();
    }

    if (vehicleHud) {
        g_overviewDrawn = false;
        if (Draw3DRadarBackground()) {
            g_overviewDrawn = true;
            DrawRouteDistance();
        }
    }

    if (g_scissorHeld && device) {
        device->SetScissorRect(&g_savedScissor);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, g_savedScissorEnable);
        g_scissorHeld = false;
    }
}

// How far is left to drive, printed in the panel's bottom-left corner.
//
// This lives in the GTA-integration half of Valkyrie Radar because text drawn
// directly from the isolated renderer corrupted the game's render state.
// badly enough to flash the whole screen in random colours. CFont::PrintString
// only adds to a batch; the game flushes that at the end of its own HUD pass.
// Calling it from inside the HUD draw is therefore exactly what the game does
// with its own text, and it is the reason this one is safe where that was not.
//
// It replaces another ASI's "1.9 km", which is silenced in
// Native radar clip setup.
void DrawRouteDistance() {
    using RemainingFn = float(__cdecl*)();
    static RemainingFn remaining = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE module = ValkyrieRadarModule()) {
            remaining = reinterpret_cast<RemainingFn>(
                GetProcAddress(module, "SprpRadar3DGetRouteRemaining"));
        }
    }
    if (!remaining) return;
    const float units = remaining();
    if (units < 0.0f) return;  // no route to measure

    // GTA world units are metres, so the metric reading is the direct one.
    //
    // Either way it drops to the smaller unit once the larger one stops saying
    // anything - a car's own display does the same, and "0.04 mi" tells a
    // driver nothing that "210 ft" does not tell them better. The changeover
    // is a kilometre for metric and a tenth of a mile for imperial, which is
    // where each of them conventionally switches.
    char text[32];
    if (radarcfg::Current().metricDistance) {
        if (units >= 1000.0f) {
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f km", units / 1000.0f);
        } else {
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%.0f m", units);
        }
    } else {
        // 1609.344 metres to the mile.
        const float miles = units / 1609.344f;
        if (miles >= 0.1f) {
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%.2f mi", miles);
        } else {
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%.0f ft", units * 3.28084f);
        }
    }

    const Point screen = ScreenSize();
    const float sx = screen.x / 640.0f, sy = screen.y / 448.0f;
    // The distance shares the top-left status slot with the loading indicator
    // and iFruit mark. It is drawn only while a finished route is available.
    const float panelTop =
        screen.y - (radarbox::BottomMargin() + radarbox::Height()) * sy;
    const float tx = (radarbox::Left() + 5.0f) * sx;
    const float ty = panelTop + 4.0f * sy;
    PrintText(tx, ty, text, 0xFFFFFFFF, 0.62f);
}

void __cdecl DrawRadarSectionReplacement(int x, int y) {
    if (!g_overviewDrawn && Draw3DRadarBackground()) {
        g_overviewDrawn = true;
        DrawRouteDistance();
    }
    if (!g_overviewDrawn && g_originalDrawRadarSection) {
        g_originalDrawRadarSection(x, y);
    }
}

// The frame hook. The game's call is rewritten to land here instead; we run our
// own work and then go where it was going.
using VoidFn = void (*)();
VoidFn g_originalFrame = nullptr;
FrameFn g_beforeFrameFn = nullptr;
FrameFn g_afterFrameFn = nullptr;
int g_frameHookId = instrument::kNoHook;

void __cdecl FrameTrampoline() {
    instrument::HookFired(g_frameHookId);
    if (g_beforeFrameFn) {
        g_beforeFrameFn();
    }
    if (g_originalFrame) {
        g_originalFrame();
    }
    if (g_afterFrameFn) {
        g_afterFrameFn();
    }
}

// The menu draw is a thiscall - the menu manager arrives in ECX. __fastcall is
// the only way to get at a register argument from C++, and it lines up: the
// function takes no stack arguments, so there is nothing to clean up either
// way. The second __fastcall register is whatever happened to be in EDX and is
// ignored.
//
// It is declared as returning int even though the reference calls it void, and
// that is the whole point. A void trampoline leaves EAX holding whatever our
// own last call put there, and if the game reads a return value from this the
// answer is then garbage every single frame. That is not theoretical: the first
// version of this was void, and the result was a game where the menu never
// closed - no HUD, no control of the character or camera, while the map itself
// worked perfectly. Capturing the value and handing it straight back costs
// nothing and cannot be wrong either way.
using MenuDrawFn = int(__fastcall*)(void*, void*);
MenuDrawFn g_originalMenuDraw = nullptr;
FrameFn g_menuDrawFn = nullptr;
int g_menuHookId = instrument::kNoHook;

int __fastcall MenuDrawTrampoline(void* self, void* edx) {
    instrument::HookFired(g_menuHookId);
    int result = 0;
    if (g_originalMenuDraw) {
        result = g_originalMenuDraw(self, edx);
    }
    // After, so we are drawing over a finished map rather than under it.
    if (g_menuDrawFn) {
        g_menuDrawFn();
    }
    return result;
}

// The in-game HUD hook. Same shape as the frame hook - a plain cdecl call with
// no arguments - and chained the same cooperative way, so a second ASI hooking
// the same site later still reaches this one.
VoidFn g_originalHudAfterFade = nullptr;
FrameFn g_hudDrawFn = nullptr;
int g_hudHookId = instrument::kNoHook;

void __cdecl HudAfterFadeTrampoline() {
    instrument::HookFired(g_hudHookId);
    if (g_originalHudAfterFade) {
        g_originalHudAfterFade();
    }
    // After, so the HUD is finished AND its queued text has been flushed. Both
    // halves matter - see kHudAfterFadeCallSite.
    if (g_hudDrawFn) {
        g_hudDrawFn();
    }
}

// Make a page writable, do something, put it back. Leaving the game's code
// segment writable would be its own kind of bug.
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

// Split ASIs share documented GTA call sites. Their startup threads can wake
// together, so serialize the read-chain-write sequence across DLL boundaries.
class HookLock {
public:
    explicit HookLock(const char* name) {
        handle_ = CreateMutexA(nullptr, FALSE, name);
        if (handle_) {
            const DWORD wait = WaitForSingleObject(handle_, 5000);
            held_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }

    ~HookLock() {
        if (held_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }

    bool Held() const { return held_; }

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

// Which module an address belongs to, for saying who took a call site off us.
// Returns the bare file name, or "the game" for the executable itself, or
// "unknown" for an address in memory no module claims - which is what a
// trampoline allocated by another ASI looks like.
const char* OwningModule(const void* address, char* out, size_t len) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        module) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(module, path, sizeof path)) {
            const char* slash = strrchr(path, '\\');
            strncpy_s(out, len, slash ? slash + 1 : path, _TRUNCATE);
            return out;
        }
    }
    strncpy_s(out, len, "unknown (an allocated trampoline)", _TRUNCATE);
    return out;
}

bool Executable(const void* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(address, &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) {
        return false;
    }
    const DWORD protection = mbi.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

}  // namespace

bool LuaRadarLayout(float* xywh) {
    RECT box{};
    if(!xywh || !g_ready || MenuIsOpen() || !PanelShowsHere() || !RadarBoxPixels(box))return false;
    xywh[0]=static_cast<float>(box.left);xywh[1]=static_cast<float>(box.top);
    xywh[2]=static_cast<float>(box.right-box.left);xywh[3]=static_cast<float>(box.bottom-box.top);
    return true;
}
bool LuaRadarBackground() {
    float bounds[4];
    if(!LuaRadarLayout(bounds) || !Draw3DRadarBackground())return false;
    DrawRouteDistance();
    return true;
}
bool Init() {
    if (g_ready) {
        return true;
    }

    // Reading through a pointer into another module's image can fault if the
    // game is not what we think it is, so ask before looking.
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(kVersionProbe), &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT) {
        logfile::Line("game: cannot read the entry point - not going to guess at what this is");
        return false;
    }

    const uint32_t probe = *reinterpret_cast<uint32_t*>(kVersionProbe);
    if (probe == kHoodlum) {
        g_version = "1.0 US HOODLUM";
    } else if (probe == kCompact) {
        g_version = "1.0 US Compact";
    } else if (valkyrie_layout::Compatible10()) {
        g_version = "1.0-compatible layout (validated anchors)";
    } else {
        logfile::Line("game: this is not San Andreas 1.0 US (probe %08X) - standing down, nothing patched",
                  probe);
        return false;
    }

    logfile::Line("game: San Andreas %s", g_version);
    g_ready = true;
    return true;
}

const char* VersionName() {
    return g_version;
}

int SetCoordBlip(BlipType type, Vector at, uint32_t colour, BlipDisplay display,
                 bool longRange) {
    if (!g_ready) {
        return 0;
    }
    // The real signature takes the vector by value and a script name it ignores
    // for coordinate blips. Both functions take the same arguments.
    using Fn = int(__cdecl*)(BlipType, Vector, uint32_t, BlipDisplay, const char*);
    const uintptr_t at_fn = longRange ? kSetCoordBlip : kSetShortRangeCoordBlip;
    return reinterpret_cast<Fn>(at_fn)(type, at, colour, display, nullptr);
}

void SetBlipSprite(int blip, int sprite) {
    if (!g_ready || blip == 0) {
        return;
    }
    using Fn = void(__cdecl*)(int, int);
    reinterpret_cast<Fn>(kSetBlipSprite)(blip, sprite);
}

void ChangeBlipScale(int blip, int size) {
    if (!g_ready || blip == 0) {
        return;
    }
    using Fn = void(__cdecl*)(int, int);
    reinterpret_cast<Fn>(kChangeBlipScale)(blip, size);
}

void SetBlipTint(int blip, uint32_t argb) {
    const int index = BlipArrayIndex(blip);
    if (index < 0) return;
    if ((argb & 0x00FFFFFFu) == 0x00FFFFFFu) {
        // White is what the game already draws. Storing it would only make the
        // lookup do work to arrive back where it started.
        g_blipTints.erase(index);
        return;
    }
    g_blipTints[index] = argb;
}

void ClearBlipTint(int blip) {
    const int index = BlipArrayIndex(blip);
    if (index >= 0) {
        g_blipTints.erase(index);
    }
}

// Blip indices are reused as blips come and go, so a tint left behind by a
// blip that has been cleared would be inherited by whatever the game plants in
// that slot next. Dropping the lot on a reconnect is the cheap way to be sure.
void ClearAllBlipTints() {
    g_blipTints.clear();
}

void ClearBlip(int blip) {
    if (!g_ready || blip == 0) {
        return;
    }
    // Resolve the index before GTA releases the handle. Individual removals
    // and range changes must not leave a tint on a slot another blip can reuse.
    ClearBlipTint(blip);
    using Fn = void(__cdecl*)(int);
    reinterpret_cast<Fn>(kClearBlip)(blip);
}

bool MapIsOpen() {
    if (!g_ready) {
        return false;
    }

    const bool menu = *reinterpret_cast<const bool*>(kMenuActive);
    const unsigned char page = *reinterpret_cast<const unsigned char*>(kCurrentMenuPage);
    const bool onMap = menu && page == kPageMap;

    // Log whenever this changes, so if it ever reads the game wrongly the log
    // says so plainly rather than the map just misbehaving.
    static bool last = false;
    static unsigned char lastPage = 0xFF;
    if (onMap != last || page != lastPage) {
        last = onMap;
        lastPage = page;
        logfile::Line("game: menu %s, page %u%s", menu ? "open" : "closed", page,
                      onMap ? " - the map" : "");
    }
    return onMap;
}

bool MenuIsOpen() {
    return g_ready && *reinterpret_cast<const bool*>(kMenuActive);
}

void FlushText() {
    if (g_ready) {
        reinterpret_cast<void(__cdecl*)()>(kFontDrawFonts)();
    }
}

bool HookFrame(FrameFn before, FrameFn after) {
    if (!g_ready) {
        return false;
    }
    if (g_originalFrame) {
        // Already hooked. Swapping the callback is fine; installing the patch
        // twice would chain the trampoline to itself and hang the game.
        g_beforeFrameFn = before;
        g_afterFrameFn = after;
        return true;
    }

    HookLock lock("Local\\SPRP.FrameHook");
    if (!lock.Held()) {
        logfile::Line("game: timed out waiting to install the cooperative frame hook");
        return false;
    }

    // The site remains a five-byte relative call after another split ASI has
    // installed itself. Its current destination becomes our original, forming
    // a chain instead of overwriting whichever component loaded first.
    auto* site = reinterpret_cast<uint8_t*>(kFrameCallSite);
    if (*site != 0xE8) {
        logfile::Line("game: the frame call site does not look like a call (%02X) - not hooking", *site);
        return false;
    }

    const int32_t rel = *reinterpret_cast<int32_t*>(kFrameCallSite + 1);
    g_originalFrame = reinterpret_cast<VoidFn>(kFrameCallSite + 5 + rel);
    if (g_originalFrame == &FrameTrampoline ||
        !Executable(reinterpret_cast<const void*>(g_originalFrame))) {
        logfile::Line("game: frame call target %p is not a safe chain target - not hooking",
                      reinterpret_cast<void*>(g_originalFrame));
        g_originalFrame = nullptr;
        return false;
    }
    g_beforeFrameFn = before;
    g_afterFrameFn = after;

    const int32_t patched = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&FrameTrampoline)) -
                            static_cast<int32_t>(kFrameCallSite + 5);

    const bool ok = WithWritable(kFrameCallSite + 1, sizeof(int32_t), [&] {
        *reinterpret_cast<int32_t*>(kFrameCallSite + 1) = patched;
    });

    if (!ok) {
        logfile::Line("game: could not make the frame call site writable");
        g_beforeFrameFn = nullptr;
        g_afterFrameFn = nullptr;
        return false;
    }

    g_frameHookId = instrument::RegisterHook("the frame (CGame::Process)",
                                            kFrameCallSite, &FrameTrampoline);
    logfile::Line("game: cooperative frame hook installed; previous target %p",
                  reinterpret_cast<void*>(g_originalFrame));
    return true;
}

bool HookMenuDraw(FrameFn fn) {
    if (!g_ready) {
        return false;
    }
    if (g_menuDrawFn) {
        g_menuDrawFn = fn;
        return true;
    }

    HookLock lock("Local\\SPRP.MenuDrawHook");
    if (!lock.Held()) {
        logfile::Line("game: timed out waiting to install the cooperative menu hook");
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(kMenuDrawCallSite);
    if (*site != 0xE8) {
        logfile::Line("game: the menu draw call site does not look like a call (%02X) - "
                      "no map overlay this session", *site);
        return false;
    }

    const int32_t rel = *reinterpret_cast<int32_t*>(kMenuDrawCallSite + 1);
    g_originalMenuDraw = reinterpret_cast<MenuDrawFn>(kMenuDrawCallSite + 5 + rel);
    if (g_originalMenuDraw == &MenuDrawTrampoline ||
        !Executable(reinterpret_cast<const void*>(g_originalMenuDraw))) {
        logfile::Line("game: menu call target %p is not a safe chain target - no overlay",
                      reinterpret_cast<void*>(g_originalMenuDraw));
        g_originalMenuDraw = nullptr;
        return false;
    }
    g_menuDrawFn = fn;

    const int32_t patched = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&MenuDrawTrampoline)) -
                            static_cast<int32_t>(kMenuDrawCallSite + 5);

    const bool ok = WithWritable(kMenuDrawCallSite + 1, sizeof(int32_t), [&] {
        *reinterpret_cast<int32_t*>(kMenuDrawCallSite + 1) = patched;
    });

    if (!ok) {
        logfile::Line("game: could not make the menu draw call site writable");
        g_menuDrawFn = nullptr;
        return false;
    }

    g_menuHookId = instrument::RegisterHook("the pause-map draw", kMenuDrawCallSite,
                                           &MenuDrawTrampoline);
    logfile::Line("game: cooperative menu hook installed; previous target %p",
                  reinterpret_cast<void*>(g_originalMenuDraw));
    return true;
}

bool HookHudDraw(FrameFn fn) {
    if (!g_ready) {
        return false;
    }
    if (g_hudDrawFn) {
        g_hudDrawFn = fn;
        return true;
    }

    HookLock lock("Local\\SPRP.HudDrawHook");
    if (!lock.Held()) {
        logfile::Line("game: timed out waiting to install the cooperative HUD hook");
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(kHudAfterFadeCallSite);
    if (*site != 0xE8) {
        logfile::Line("game: the HUD draw call site does not look like a call (%02X) - "
                      "nothing will be drawn over the HUD this session", *site);
        return false;
    }

    const int32_t rel = *reinterpret_cast<int32_t*>(kHudAfterFadeCallSite + 1);
    g_originalHudAfterFade = reinterpret_cast<VoidFn>(kHudAfterFadeCallSite + 5 + rel);
    // On a clean install this is CFont::DrawFonts itself. It is only checked,
    // not required: another component may legitimately have chained here
    // first, and refusing to join an existing chain would be worse than
    // joining one we did not expect.
    if (reinterpret_cast<uintptr_t>(g_originalHudAfterFade) !=
        kFontDrawFontsTarget) {
        logfile::Line("game: the HUD flush call points at %p rather than "
                      "CFont::DrawFonts - chaining onto it anyway",
                      reinterpret_cast<void*>(g_originalHudAfterFade));
    }
    if (g_originalHudAfterFade == &HudAfterFadeTrampoline ||
        !Executable(reinterpret_cast<const void*>(g_originalHudAfterFade))) {
        logfile::Line("game: HUD call target %p is not a safe chain target - no HUD overlay",
                      reinterpret_cast<void*>(g_originalHudAfterFade));
        g_originalHudAfterFade = nullptr;
        return false;
    }
    g_hudDrawFn = fn;

    const int32_t patched =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HudAfterFadeTrampoline)) -
        static_cast<int32_t>(kHudAfterFadeCallSite + 5);

    const bool ok = WithWritable(kHudAfterFadeCallSite + 1, sizeof(int32_t), [&] {
        *reinterpret_cast<int32_t*>(kHudAfterFadeCallSite + 1) = patched;
    });

    if (!ok) {
        logfile::Line("game: could not make the HUD draw call site writable");
        g_hudDrawFn = nullptr;
        return false;
    }

    g_hudHookId = instrument::RegisterHook("the HUD text flush",
                                          kHudAfterFadeCallSite,
                                          &HudAfterFadeTrampoline);
    logfile::Line("game: cooperative HUD hook installed; previous target %p",
                  reinterpret_cast<void*>(g_originalHudAfterFade));
    return true;
}

bool HudHookIntact() {
    if (!g_ready || !g_hudDrawFn) return false;
    const auto* site = reinterpret_cast<const uint8_t*>(kHudAfterFadeCallSite);
    if (*site != 0xE8) return false;
    const int32_t rel = *reinterpret_cast<const int32_t*>(kHudAfterFadeCallSite + 1);
    return reinterpret_cast<const void*>(kHudAfterFadeCallSite + 5 + rel) ==
           reinterpret_cast<const void*>(&HudAfterFadeTrampoline);
}

bool ReassertHudHook() {
    // Hooking a shared call site is first-come, and being first is the weak
    // position: a component that patches the same site later reads whatever is
    // there, chains onto it and wins. Ours is installed early, so an ASI that
    // starts later and does NOT chain - or that puts the original address back
    // - drops us with no error anywhere. The symptom is a hook that reported
    // success at startup and never runs.
    //
    // So the hook is checked rather than trusted, and re-taken when it has
    // gone. Re-taking chains onto whoever is there now, which leaves both
    // components running rather than taking the site back off them.
    const bool intact = HudHookIntact();
    instrument::SetHookIntact(g_hudHookId, intact);
    if (!g_ready || !g_hudDrawFn || intact) {
        return true;
    }

    HookLock lock("Local\\SPRP.HudDrawHook");
    if (!lock.Held()) return false;

    auto* site = reinterpret_cast<uint8_t*>(kHudAfterFadeCallSite);
    if (*site != 0xE8) {
        logfile::Line("game: the HUD call site is no longer a call (%02X) - "
                      "leaving it alone", *site);
        return false;
    }
    const int32_t rel = *reinterpret_cast<int32_t*>(kHudAfterFadeCallSite + 1);
    auto* current = reinterpret_cast<VoidFn>(kHudAfterFadeCallSite + 5 + rel);
    if (current == &HudAfterFadeTrampoline || !Executable(
            reinterpret_cast<const void*>(current))) {
        return false;
    }

    char owner[MAX_PATH]{};
    logfile::Line("game: the HUD call site was taken by %s (%p) - chaining "
                  "onto it and carrying on",
                  OwningModule(reinterpret_cast<const void*>(current), owner,
                               sizeof owner),
                  reinterpret_cast<void*>(current));

    g_originalHudAfterFade = current;
    const int32_t patched =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&HudAfterFadeTrampoline)) -
        static_cast<int32_t>(kHudAfterFadeCallSite + 5);
    return WithWritable(kHudAfterFadeCallSite + 1, sizeof(int32_t), [&] {
        *reinterpret_cast<int32_t*>(kHudAfterFadeCallSite + 1) = patched;
    });
}

Point WorldToMapRadar(float worldX, float worldY) {
    if (!g_ready || !std::isfinite(worldX) || !std::isfinite(worldY)) {
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        return {invalid, invalid};
    }

    // Expanded-map plugins patch CRadar's transform. Reconstructing it from
    // only the public range/origin pair can therefore put pause-map overlays
    // thousands of world units away from their actual position.
    float world[2] = {worldX, worldY};
    float radar[2] = {0.0f, 0.0f};
    using ToRadar = void(__cdecl*)(float*, const float*);
    reinterpret_cast<ToRadar>(kTransformRealWorldToRadar)(radar, world);
    if (!std::isfinite(radar[0]) || !std::isfinite(radar[1])) {
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        return {invalid, invalid};
    }
    return {radar[0], radar[1]};
}

Point WorldToScreen(float worldX, float worldY) {
    float world[2] = {worldX, worldY};
    float radar[2] = {0.0f, 0.0f};
    float screen[2] = {0.0f, 0.0f};

    using ToRadar = void(__cdecl*)(float*, const float*);
    using ToScreen = void(__cdecl*)(float*, const float*);
    reinterpret_cast<ToRadar>(kTransformRealWorldToRadar)(radar, world);
    reinterpret_cast<ToScreen>(kTransformRadarToScreen)(screen, radar);

    return {screen[0], screen[1]};
}

bool ExpandedMapActive() {
    // Which map this is, decided from the widest world range CRadar has been
    // seen holding rather than from whatever it holds right now.
    //
    // The value is live and it moves: the driving radar sets it to a few
    // hundred units, and only the pause map opens it out to the whole world -
    // about 2990 on the stock map, about 23920 with fastman92's 48,000-unit
    // patch, which is the gap this reads. A single live sample is therefore
    // only meaningful at certain moments in the frame, and every caller had to
    // know which. The widest range ever seen is not: the stock map never
    // reaches 10,000, so the answer moves from "stock" to "expanded" once,
    // early, and then holds - which is what a map variant does.
    if (!g_ready) return false;
    static float widest = 0.0f;
    const float range = *reinterpret_cast<const float*>(kRadarRange);
    if (std::isfinite(range) && range > widest) widest = range;
    return widest > 10000.0f;
}

Point WorldToMapScreen(float worldX, float worldY) {
    const Point radar = WorldToMapRadar(worldX, worldY);
    const MapView view = Map();
    if (!std::isfinite(radar.x) || !std::isfinite(radar.y) ||
        !std::isfinite(view.zoom) || !std::isfinite(view.baseX) ||
        !std::isfinite(view.baseY)) {
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        return {invalid, invalid};
    }
    const Point screen = ScreenSize();
    return {(view.baseX + view.zoom * radar.x) * screen.x / 640.0f,
            (view.baseY - view.zoom * radar.y) * screen.y / 448.0f};
}

bool PlayerPosition(Point& out) {
    if (!g_ready) return false;
    // Follow the vehicle while driving and the ped otherwise. Both helpers
    // return plain pointers, avoiding structure/reference-return ABI calls in
    // a hook that executes inside another ASI's frontend draw.
    auto* entity = reinterpret_cast<uint8_t* (__cdecl*)(int, bool)>(
        kFindPlayerVehicle)(-1, true);
    if (!entity) {
        entity = reinterpret_cast<uint8_t* (__cdecl*)(int)>(kFindPlayerPed)(-1);
    }
    if (!entity) return false;
    const uintptr_t matrix = *reinterpret_cast<const uintptr_t*>(entity + 0x14);
    if (!matrix) return false;
    const float x = *reinterpret_cast<const float*>(matrix + 0x30);
    const float y = *reinterpret_cast<const float*>(matrix + 0x34);
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    out = {x, y};
    return true;
}

int PlayerInterior() {
    // CGame::currArea, verified against the local gta-reversed Game.h.
    return g_ready ? *reinterpret_cast<const int*>(0xB72914) : -1;
}

bool PlayerVehicleState(VehicleState& out) {
    if (!g_ready) return false;
    auto* vehicle = reinterpret_cast<uint8_t*>(
        reinterpret_cast<void* (__cdecl*)(int, bool)>(kFindPlayerVehicle)(-1, false));
    if (!vehicle) return false;

    const uintptr_t matrix = *reinterpret_cast<const uintptr_t*>(vehicle + 0x14);
    if (!matrix) return false;
    const float x = *reinterpret_cast<const float*>(matrix + 0x30);
    const float y = *reinterpret_cast<const float*>(matrix + 0x34);
    const float z = *reinterpret_cast<const float*>(matrix + 0x38);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;

    const float* velocity =
        reinterpret_cast<const float*>(vehicle + kPhysicalMoveSpeed);
    float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] +
                            velocity[2] * velocity[2]);
    if (!std::isfinite(speed)) speed = 0.0f;

    const uint8_t* flags = vehicle + kVehicleFlags;
    const void* driver = *reinterpret_cast<void* const*>(vehicle + kVehicleDriver);
    const void* playerPed =
        reinterpret_cast<void* (__cdecl*)(int)>(kFindPlayerPed)(-1);

    out.handle = vehicle;
    out.model = *reinterpret_cast<const uint16_t*>(vehicle + kEntityModelIndex);
    out.createdAt =
        *reinterpret_cast<const uint32_t*>(vehicle + kVehicleCreationTime);
    // GTA moves an entity by m_vecMoveSpeed once per 50Hz step, so the raw
    // vector is metres per step and not per second. Multiplying by the game's
    // own step rate is what turns it into the speed a speedometer would read.
    out.speed = speed * 50.0f;
    out.engineOn = (flags[kFlagEngineOnByte] & kFlagEngineOnMask) != 0;
    out.engineBroken =
        (flags[kFlagEngineBrokenByte] & kFlagEngineBrokenMask) != 0;
    out.playerIsDriver = driver != nullptr && driver == playerPed;
    out.health = *reinterpret_cast<const float*>(vehicle + kVehicleHealth);
    out.x = x;
    out.y = y;
    out.z = z;
    return true;
}

void SetVehicleEngineBroken(void* handle, bool broken) {
    if (!g_ready || !handle) return;
    uint8_t* flags = static_cast<uint8_t*>(handle) + kVehicleFlags;
    if (broken) {
        flags[kFlagEngineBrokenByte] |= kFlagEngineBrokenMask;
    } else {
        flags[kFlagEngineBrokenByte] &= static_cast<uint8_t>(~kFlagEngineBrokenMask);
    }
}

void SetVehicleEngineOn(void* handle, bool on) {
    if (!g_ready || !handle) return;
    uint8_t* flags = static_cast<uint8_t*>(handle) + kVehicleFlags;
    if (on) {
        flags[kFlagEngineOnByte] |= kFlagEngineOnMask;
    } else {
        flags[kFlagEngineOnByte] &= static_cast<uint8_t>(~kFlagEngineOnMask);
    }
}

bool MapWaypoint(Point& out, int& handle) {
    if (!g_ready) return false;
    handle = *reinterpret_cast<const int*>(kTargetBlipIndex);
    if (!handle) return false;
    const float* cursor = reinterpret_cast<const float*>(kMapCursor);
    if (!std::isfinite(cursor[0]) || !std::isfinite(cursor[1])) return false;
    out = {cursor[0], cursor[1]};
    return true;
}

bool ObjectiveBlip(Point& out, unsigned long& ageMs, uint32_t& rgb) {
    if (!g_ready || !g_objectiveAt) return false;
    out = {g_objectiveX, g_objectiveY};
    const DWORD now = GetTickCount();
    ageMs = now >= g_objectiveAt ? now - g_objectiveAt : 0;
    rgb = g_objectiveColour;
    return true;
}

bool InstallObjectiveColourPatch() {
    if (!g_ready) return false;
    if (g_originalShowRadarTraceWithHeight) return true;

    auto* site = reinterpret_cast<uint8_t*>(kShowRadarTraceWithHeight);
    if (site[0] == 0xE9) {
        const int32_t oldRel = *reinterpret_cast<int32_t*>(site + 1);
        auto* previous = reinterpret_cast<ShowRadarTraceWithHeightFn>(
            kShowRadarTraceWithHeight + 5 + oldRel);
        if (!Executable(reinterpret_cast<const void*>(previous)) ||
            reinterpret_cast<void*>(previous) ==
                reinterpret_cast<void*>(&ShowRadarTraceWithHeightReplacement)) {
            return false;
        }
        const int32_t hookRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
            &ShowRadarTraceWithHeightReplacement)) -
            static_cast<int32_t>(kShowRadarTraceWithHeight + 5);
        if (!WithWritable(kShowRadarTraceWithHeight, 5, [&] {
                *reinterpret_cast<int32_t*>(site + 1) = hookRel;
            })) return false;
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        g_originalShowRadarTraceWithHeight = previous;
        logfile::Line("radar: objective marker colour is read from the game");
        return true;
    }

    // Checked before anything is moved. A different executable, or another ASI
    // that patched this without leaving a jump, and the whole thing stands
    // down - the ribbon keeps its configured colour, which is survivable, and
    // relocating half an instruction would not be.
    const uint8_t expected[kShowRadarTracePrologue] = {
        0xA0, 0xA1, 0x67, 0xBA, 0x00,
    };
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line("radar: objective colour hook stood down - "
                      "ShowRadarTraceWithHeight entry is %02X %02X %02X %02X %02X",
                      site[0], site[1], site[2], site[3], site[4]);
        return false;
    }

    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kShowRadarTracePrologue + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, site, kShowRadarTracePrologue);
    trampoline[kShowRadarTracePrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kShowRadarTracePrologue + 1) =
        static_cast<int32_t>(kShowRadarTraceWithHeight + kShowRadarTracePrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kShowRadarTracePrologue + 5);
    const int32_t hookRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
        &ShowRadarTraceWithHeightReplacement)) -
        static_cast<int32_t>(kShowRadarTraceWithHeight + 5);
    if (!WithWritable(kShowRadarTraceWithHeight, kShowRadarTracePrologue, [&] {
            site[0] = 0xE9;
            *reinterpret_cast<int32_t*>(site + 1) = hookRel;
        })) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), site, kShowRadarTracePrologue);
    g_originalShowRadarTraceWithHeight =
        reinterpret_cast<ShowRadarTraceWithHeightFn>(trampoline);
    logfile::Line("radar: objective marker colour is read from the game");
    return true;
}

Point MapCursor() {
    const float* p = reinterpret_cast<const float*>(kMapCursor);
    return {p[0], p[1]};
}

int MapZoomWheel() {
    const bool up = *reinterpret_cast<const bool*>(kMouseWheelUp);
    const bool down = *reinterpret_cast<const bool*>(kMouseWheelDown);
    return up == down ? 0 : (up ? 1 : -1);
}

bool MapLeftHeld() {
    return *reinterpret_cast<const bool*>(kMouseLeft);
}

bool InstallMapControlPatch() {
    auto* up = reinterpret_cast<uint8_t*>(kMapWheelUpBranch);
    auto* down = reinterpret_cast<uint8_t*>(kMapWheelDownBranch);
    auto* clamp = reinterpret_cast<uint8_t*>(kMapOriginClamp);

    const bool branchesOriginal = up[0] == 0x75 && up[1] == 0x1E &&
                                  down[0] == 0x75 && down[1] == 0x1E;
    const bool branchesPatched = up[0] == 0x90 && up[1] == 0x90 &&
                                 down[0] == 0x90 && down[1] == 0x90;
    const bool clampOriginal = clamp[0] == 0xD9 && clamp[1] == 0x46 &&
                               clamp[2] == 0x6C && clamp[3] == 0xD8 &&
                               clamp[4] == 0x66;
    const bool clampPatched = clamp[0] == 0xE9;

    if ((!branchesOriginal && !branchesPatched) ||
        (!clampOriginal && !clampPatched)) {
        logfile::Line(
            "game: map-control signatures changed (up %02X %02X, down %02X %02X, clamp %02X %02X %02X %02X %02X) - not patching",
            up[0], up[1], down[0], down[1], clamp[0], clamp[1], clamp[2],
            clamp[3], clamp[4]);
        return false;
    }

    bool ok = true;
    if (branchesOriginal) {
        ok = WithWritable(kMapWheelUpBranch, 2, [&] {
            up[0] = up[1] = 0x90;
        }) && ok;
        ok = WithWritable(kMapWheelDownBranch, 2, [&] {
            down[0] = down[1] = 0x90;
        }) && ok;
    }
    if (clampOriginal) {
        const int32_t relative = static_cast<int32_t>(kMapOriginClampEnd) -
                                 static_cast<int32_t>(kMapOriginClamp + 5);
        ok = WithWritable(kMapOriginClamp, 5, [&] {
            clamp[0] = 0xE9;
            *reinterpret_cast<int32_t*>(clamp + 1) = relative;
        }) && ok;
    }

    if (!ok) {
        logfile::Line("game: could not make the native map controls writable");
        return false;
    }

    logfile::Line(
        "game: native map wheel and 6000-unit origin clamp suppressed; custom map control owns them");
    return true;
}

bool InstallMapOverviewPatch() {
    if (!g_ready) {
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(kDrawRadarSectionMap);
    const uint8_t expected[kDrawRadarSectionMapPrologue] = {
        0x8B, 0x54, 0x24, 0x04, 0x83, 0xEC, 0x08,
    };
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line(
            "map overview: DrawRadarSectionMap entry changed (%02X %02X %02X %02X %02X %02X %02X) - leaving it alone",
            site[0], site[1], site[2], site[3], site[4], site[5], site[6]);
        return false;
    }

    auto* trampoline = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kDrawRadarSectionMapPrologue + 5,
                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        logfile::Line("map overview: could not allocate the detailed-map trampoline");
        return false;
    }

    memcpy(trampoline, site, kDrawRadarSectionMapPrologue);
    trampoline[kDrawRadarSectionMapPrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kDrawRadarSectionMapPrologue + 1) =
        static_cast<int32_t>(kDrawRadarSectionMap + kDrawRadarSectionMapPrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kDrawRadarSectionMapPrologue + 5);

    const int32_t replacement =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(
            &DrawRadarSectionMapReplacement)) -
        static_cast<int32_t>(kDrawRadarSectionMap + 5);
    const bool ok = WithWritable(
        kDrawRadarSectionMap, kDrawRadarSectionMapPrologue, [&] {
            site[0] = 0xE9;
            *reinterpret_cast<int32_t*>(site + 1) = replacement;
            site[5] = 0x90;
            site[6] = 0x90;
        });
    if (!ok) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        logfile::Line("map overview: could not make DrawRadarSectionMap writable");
        return false;
    }

    FlushInstructionCache(GetCurrentProcess(), site,
                          kDrawRadarSectionMapPrologue);
    g_originalDrawRadarSectionMap =
        reinterpret_cast<DrawRadarSectionMapFn>(trampoline);
    logfile::Line(
        "map overview: detailed tiles retained; distant full map uses one composed draw");
    return true;
}

bool InstallRadar3DBackgroundPatch() {
    if (!g_ready) {
        return false;
    }
    auto* site = reinterpret_cast<uint8_t*>(kDrawRadarSection);
    const uint8_t expected[kDrawRadarSectionPrologue] = {
        0x81, 0xEC, 0x30, 0x01, 0x00, 0x00,
    };
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line(
            "radar3d bridge: DrawRadarSection entry changed - leaving native radar alone");
        return false;
    }
    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kDrawRadarSectionPrologue + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    memcpy(trampoline, site, kDrawRadarSectionPrologue);
    trampoline[kDrawRadarSectionPrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kDrawRadarSectionPrologue + 1) =
        static_cast<int32_t>(kDrawRadarSection + kDrawRadarSectionPrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kDrawRadarSectionPrologue + 5);
    const int32_t replacement =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(
            &DrawRadarSectionReplacement)) -
        static_cast<int32_t>(kDrawRadarSection + 5);
    const bool ok = WithWritable(
        kDrawRadarSection, kDrawRadarSectionPrologue, [&] {
            site[0] = 0xE9;
            *reinterpret_cast<int32_t*>(site + 1) = replacement;
            site[5] = 0x90;
        });
    if (!ok) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), site,
                          kDrawRadarSectionPrologue);
    g_originalDrawRadarSection =
        reinterpret_cast<DrawRadarSectionFn>(trampoline);
    logfile::Line(
        "radar3d bridge: small radar background linked; native markers retained");
    return true;
}

bool InstallRadarBoxTransformPatch() {
    if (!g_ready || g_originalTransformRadarPointToScreen) return g_ready;
    auto* site = reinterpret_cast<uint8_t*>(kTransformRadarPointToScreen);
    // GTA SA may already own this entry with a normal five-byte JMP.
    // That is not a conflict: retain its destination as our pause-map path,
    // then put our gameplay-only transform in front of it. Rejecting a clean
    // chain here was what split the new 3D rectangle from the old marker ring.
    if (site[0] == 0xE9) {
        const int32_t oldRel = *reinterpret_cast<int32_t*>(site + 1);
        auto* previous = reinterpret_cast<TransformRadarPointToScreenFn>(
            kTransformRadarPointToScreen + 5 + oldRel);
        if (!Executable(reinterpret_cast<const void*>(previous)) ||
            reinterpret_cast<void*>(previous) ==
                reinterpret_cast<void*>(&TransformRadarPointToScreenReplacement)) {
            logfile::Line("radar: existing screen-transform chain is unsafe");
            return false;
        }
        const int32_t replacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
            &TransformRadarPointToScreenReplacement)) -
            static_cast<int32_t>(kTransformRadarPointToScreen + 5);
        if (!WithWritable(kTransformRadarPointToScreen, 5, [&] {
                *reinterpret_cast<int32_t*>(site + 1) = replacement;
            })) {
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        g_originalTransformRadarPointToScreen = previous;
        logfile::Line("radar: GTA VI box transform chained after %p",
                      reinterpret_cast<void*>(previous));
        return true;
    }
    // mov al,[m_bDrawingMap] (five bytes) / mov ecx,[esp+4] (four bytes)
    const uint8_t expected[kTransformRadarPointToScreenPrologue] = {
        0xA0, 0xA1, 0x67, 0xBA, 0x00, 0x8B, 0x4C, 0x24, 0x04,
    };
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line("radar: screen transform entry changed (%02X %02X %02X %02X "
                      "%02X %02X %02X %02X %02X) - retaining GTA layout",
                      site[0], site[1], site[2], site[3], site[4], site[5],
                      site[6], site[7], site[8]);
        return false;
    }
    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kTransformRadarPointToScreenPrologue + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, site, kTransformRadarPointToScreenPrologue);
    trampoline[kTransformRadarPointToScreenPrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kTransformRadarPointToScreenPrologue + 1) =
        static_cast<int32_t>(kTransformRadarPointToScreen + kTransformRadarPointToScreenPrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kTransformRadarPointToScreenPrologue + 5);
    const int32_t replacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
        &TransformRadarPointToScreenReplacement)) -
        static_cast<int32_t>(kTransformRadarPointToScreen + 5);
    const bool ok = WithWritable(kTransformRadarPointToScreen,
                                 kTransformRadarPointToScreenPrologue, [&] {
        site[0] = 0xE9;
        *reinterpret_cast<int32_t*>(site + 1) = replacement;
        memset(site + 5, 0x90, kTransformRadarPointToScreenPrologue - 5);
    });
    if (!ok) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), site,
                          kTransformRadarPointToScreenPrologue);
    g_originalTransformRadarPointToScreen =
        reinterpret_cast<TransformRadarPointToScreenFn>(trampoline);
    logfile::Line("radar: GTA VI box transform installed (%.0fx%.0f at %.0f, bottom %.0f)",
                  radarbox::Width(), radarbox::Height(), radarbox::Left(),
                  radarbox::BottomMargin());
    return true;
}

bool InstallRadarBoxPerimeterPatch() {
    if (!g_ready || g_originalLimitRadarPoint) return g_ready;
    auto* site = reinterpret_cast<uint8_t*>(kLimitRadarPoint);
    // This installation already redirects LimitRadarPoint through a stable
    // five-byte JMP. Preserve that chain for the pause map and replace only
    // its gameplay behaviour.
    if (site[0] != 0xE9) {
        logfile::Line("radar: perimeter entry changed (%02X %02X %02X %02X %02X)",
                      site[0], site[1], site[2], site[3], site[4]);
        return false;
    }
    const int32_t oldRel = *reinterpret_cast<int32_t*>(site + 1);
    auto* previous = reinterpret_cast<LimitRadarPointFn>(
        kLimitRadarPoint + 5 + oldRel);
    if (!Executable(reinterpret_cast<const void*>(previous)) ||
        reinterpret_cast<void*>(previous) ==
            reinterpret_cast<void*>(&LimitRadarPointToBoxReplacement)) {
        logfile::Line("radar: existing perimeter chain is unsafe");
        return false;
    }
    const int32_t replacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
        &LimitRadarPointToBoxReplacement)) -
        static_cast<int32_t>(kLimitRadarPoint + 5);
    if (!WithWritable(kLimitRadarPoint, kLimitRadarPointPrologue, [&] {
            *reinterpret_cast<int32_t*>(site + 1) = replacement;
        })) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), site, kLimitRadarPointPrologue);
    g_originalLimitRadarPoint = previous;
    logfile::Line("radar: directional markers now wrap the GTA VI box perimeter");
    return true;
}

bool InstallDrivingBlipFilterPatch() {
    if (!g_ready || g_originalDisplayThisBlip) return g_ready;
    auto* site = reinterpret_cast<uint8_t*>(kDisplayThisBlip);
    // GTA SA owns the function already, so preserve its reveal and
    // priority logic for the two sprites we allow and chain in front of it.
    if (site[0] != 0xE9) {
        logfile::Line("radar: DisplayThisBlip is not chainable (%02X)", site[0]);
        return false;
    }
    const int32_t oldRel = *reinterpret_cast<int32_t*>(site + 1);
    auto* previous = reinterpret_cast<DisplayThisBlipFn>(
        kDisplayThisBlip + 5 + oldRel);
    if (!Executable(reinterpret_cast<const void*>(previous)) ||
        reinterpret_cast<void*>(previous) ==
            reinterpret_cast<void*>(&DisplayThisBlipReplacement)) {
        logfile::Line("radar: existing blip visibility chain is unsafe");
        return false;
    }
    const int32_t replacement =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(
            &DisplayThisBlipReplacement)) -
        static_cast<int32_t>(kDisplayThisBlip + 5);
    if (!WithWritable(kDisplayThisBlip, 5, [&] {
            *reinterpret_cast<int32_t*>(site + 1) = replacement;
        })) return false;
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_originalDisplayThisBlip = previous;
    logfile::Line("radar: driving blips filtered to north and player waypoint");
    return true;
}

bool InstallPauseMapPlayerMarkerPatch() {
    g_pauseMapPlayerMarkerActive = true;
    if (!g_ready || g_originalDrawRotatingRadarSprite) return g_ready;
    auto* site = reinterpret_cast<uint8_t*>(kDrawRotatingRadarSprite);
    if (site[0] == 0xE9) {
        const int32_t oldRel = *reinterpret_cast<int32_t*>(site + 1);
        auto* previous = reinterpret_cast<DrawRotatingRadarSpriteFn>(
            kDrawRotatingRadarSprite + 5 + oldRel);
        if (!Executable(reinterpret_cast<const void*>(previous)) ||
            reinterpret_cast<void*>(previous) == reinterpret_cast<void*>(
                &DrawRotatingRadarSpriteReplacement)) {
            return false;
        }
        const int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
            &DrawRotatingRadarSpriteReplacement)) -
            static_cast<int32_t>(kDrawRotatingRadarSprite + 5);
        if (!WithWritable(kDrawRotatingRadarSprite, 5, [&] {
                *reinterpret_cast<int32_t*>(site + 1) = relative;
            })) return false;
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        g_originalDrawRotatingRadarSprite = previous;
        logfile::Line("map: player-marker hook joined existing ASI chain");
        return true;
    }
    const uint8_t expected[kDrawRotatingRadarSpritePrologue] = {
        0xA0, 0xA1, 0x67, 0xBA, 0x00,
    };
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line("map: rotating player-marker entry changed");
        return false;
    }
    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kDrawRotatingRadarSpritePrologue + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, site, kDrawRotatingRadarSpritePrologue);
    trampoline[kDrawRotatingRadarSpritePrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kDrawRotatingRadarSpritePrologue + 1) =
        static_cast<int32_t>(kDrawRotatingRadarSprite +
                             kDrawRotatingRadarSpritePrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kDrawRotatingRadarSpritePrologue + 5);
    const int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
        &DrawRotatingRadarSpriteReplacement)) -
        static_cast<int32_t>(kDrawRotatingRadarSprite + 5);
    if (!WithWritable(kDrawRotatingRadarSprite,
                      kDrawRotatingRadarSpritePrologue, [&] {
            site[0] = 0xE9;
            *reinterpret_cast<int32_t*>(site + 1) = relative;
        })) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), site,
                          kDrawRotatingRadarSpritePrologue);
    g_originalDrawRotatingRadarSprite =
        reinterpret_cast<DrawRotatingRadarSpriteFn>(trampoline);
    logfile::Line("map: pause-map player marker follows live zoom and origin");
    return true;
}

bool InstallDrivingRadarPresentationPatch() {
    if (!g_ready) return false;

    // Share/chains the rotating-sprite hook when Valkyrie Map is present, but
    // this DLL must never opt into pause-map positioning on its own.
    if (!InstallPauseMapPlayerMarkerPatch()) return false;
    g_pauseMapPlayerMarkerActive = false;

    auto install = [](uintptr_t address, size_t prologue, const uint8_t* expected,
                      const void* replacement, void** original) -> bool {
        if (*original) return true;
        auto* site = reinterpret_cast<uint8_t*>(address);
        if (memcmp(site, expected, prologue) != 0) return false;
        auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, prologue + 5, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
        if (!trampoline) return false;
        memcpy(trampoline, site, prologue);
        trampoline[prologue] = 0xE9;
        *reinterpret_cast<int32_t*>(trampoline + prologue + 1) =
            static_cast<int32_t>(address + prologue) -
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                                 prologue + 5);
        const int32_t relative =
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement)) -
            static_cast<int32_t>(address + 5);
        if (!WithWritable(address, prologue, [&] {
                site[0] = 0xE9;
                *reinterpret_cast<int32_t*>(site + 1) = relative;
                for (size_t i = 5; i < prologue; ++i) site[i] = 0x90;
            })) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), site, prologue);
        *original = trampoline;
        return true;
    };

    const uint8_t areaExpected[kDrawAreaOnRadarPrologue] = {
        0x8A, 0x44, 0x24, 0x0C, 0x8B, 0x4C, 0x24, 0x04,
    };
    if (!install(kDrawAreaOnRadar, kDrawAreaOnRadarPrologue, areaExpected,
                 reinterpret_cast<const void*>(&DrawAreaOnRadarReplacement),
                 reinterpret_cast<void**>(&g_originalDrawAreaOnRadar))) {
        logfile::Line("radar: gang-zone suppression hook unavailable");
        return false;
    }
    logfile::Line("radar: player marker clamped to vehicle heading; driving gang zones hidden");
    return true;
}

// Chain into CSprite2d::Draw.
//
// Two separate things need it - the HUD frame, to swallow the stock circular
// bezel, and the blip tint, to repaint a marker the game hands out in flat
// white - and whichever one runs first should be the one that installs it.
// Doing it twice would put us in front of ourselves.
bool HookSpriteDraw() {
    if (g_originalSpriteDraw) return true;

    auto* spriteDraw = reinterpret_cast<uint8_t*>(kSpriteDraw);
    if (spriteDraw[0] == 0xE9) {
        const int32_t oldRel = *reinterpret_cast<int32_t*>(spriteDraw + 1);
        auto* previous = reinterpret_cast<SpriteDrawFn>(
            kSpriteDraw + 5 + oldRel);
        if (!Executable(reinterpret_cast<void*>(previous)) ||
            reinterpret_cast<void*>(previous) ==
                reinterpret_cast<void*>(&SpriteDrawReplacement)) {
            logfile::Line("radar: disc sprite chain is unsafe");
            return false;
        }
        const int32_t hookRel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&SpriteDrawReplacement)) -
            static_cast<int32_t>(kSpriteDraw + 5);
        if (!WithWritable(kSpriteDraw, 5, [&] {
                *reinterpret_cast<int32_t*>(spriteDraw + 1) = hookRel;
            })) {
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), spriteDraw, 5);
        g_originalSpriteDraw = previous;
    } else {
        const uint8_t expected[kSpriteDrawPrologue] = {
            0x8B, 0x44, 0x24, 0x08, 0x56,
        };
        if (memcmp(spriteDraw, expected, sizeof(expected)) != 0) {
            logfile::Line("radar: disc sprite entry changed (%02X %02X %02X %02X %02X)",
                          spriteDraw[0], spriteDraw[1], spriteDraw[2],
                          spriteDraw[3], spriteDraw[4]);
            return false;
        }
        auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(
            nullptr, kSpriteDrawPrologue + 5,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline) return false;
        memcpy(trampoline, spriteDraw, kSpriteDrawPrologue);
        trampoline[kSpriteDrawPrologue] = 0xE9;
        *reinterpret_cast<int32_t*>(trampoline + kSpriteDrawPrologue + 1) =
            static_cast<int32_t>(kSpriteDraw + kSpriteDrawPrologue) -
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                                 kSpriteDrawPrologue + 5);
        const int32_t hookRel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&SpriteDrawReplacement)) -
            static_cast<int32_t>(kSpriteDraw + 5);
        if (!WithWritable(kSpriteDraw, kSpriteDrawPrologue, [&] {
                spriteDraw[0] = 0xE9;
                *reinterpret_cast<int32_t*>(spriteDraw + 1) = hookRel;
            })) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), spriteDraw,
                              kSpriteDrawPrologue);
        g_originalSpriteDraw = reinterpret_cast<SpriteDrawFn>(trampoline);
    }
    return g_originalSpriteDraw != nullptr;
}

// The blip sprites are drawn in whoever holds the place's colour.
//
// Installed by the blips ASI rather than the map: this is about what the server
// says, and a game with no server has nothing to tint.
bool InstallBlipTintPatch() {
    if (!g_ready) return false;
    if (g_originalDrawCoordBlip) return true;

    if (!HookSpriteDraw()) {
        logfile::Line("blips: sprite draw unavailable - markers stay white");
        return false;
    }

    auto* site = reinterpret_cast<uint8_t*>(kDrawCoordBlip);
    if (site[0] == 0xE9) {
        // Somebody is already there - GTA SA, or another ASI. Chain in
        // front of them and keep whatever they do.
        const int32_t oldRel = *reinterpret_cast<int32_t*>(site + 1);
        auto* previous = reinterpret_cast<DrawCoordBlipFn>(
            kDrawCoordBlip + 5 + oldRel);
        if (!Executable(reinterpret_cast<const void*>(previous)) ||
            reinterpret_cast<void*>(previous) ==
                reinterpret_cast<void*>(&DrawCoordBlipReplacement)) {
            logfile::Line("blips: blip draw chain is unsafe - markers stay white");
            return false;
        }
        const int32_t hookRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
            &DrawCoordBlipReplacement)) - static_cast<int32_t>(kDrawCoordBlip + 5);
        if (!WithWritable(kDrawCoordBlip, 5, [&] {
                *reinterpret_cast<int32_t*>(site + 1) = hookRel;
            })) {
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        g_originalDrawCoordBlip = previous;
        logfile::Line("blips: markers now carry the holder's colour");
        return true;
    }

    // sub esp,24 / push ebx / push esi - the stock entry, read out of the
    // shipped executable rather than guessed at. It comes to exactly the five
    // bytes a jump needs, so nothing is split. Checked before patching,
    // because relocating half of somebody else's instruction into our
    // trampoline is a crash on the next frame rather than a bad colour.
    const uint8_t expected[kDrawCoordBlipPrologue] = {
        0x83, 0xEC, 0x24, 0x53, 0x56,
    };
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line("blips: blip draw entry changed (%02X %02X %02X %02X %02X) - markers stay white",
                      site[0], site[1], site[2], site[3], site[4]);
        return false;
    }

    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kDrawCoordBlipPrologue + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, site, kDrawCoordBlipPrologue);
    trampoline[kDrawCoordBlipPrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kDrawCoordBlipPrologue + 1) =
        static_cast<int32_t>(kDrawCoordBlip + kDrawCoordBlipPrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kDrawCoordBlipPrologue + 5);
    const int32_t hookRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(
        &DrawCoordBlipReplacement)) - static_cast<int32_t>(kDrawCoordBlip + 5);
    if (!WithWritable(kDrawCoordBlip, kDrawCoordBlipPrologue, [&] {
            site[0] = 0xE9;
            *reinterpret_cast<int32_t*>(site + 1) = hookRel;
        })) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), site, kDrawCoordBlipPrologue);
    g_originalDrawCoordBlip = reinterpret_cast<DrawCoordBlipFn>(trampoline);
    logfile::Line("blips: markers now carry the holder's colour");
    return true;
}

bool InstallRadarBoxHudFramePatch() {
    if (!g_ready || !g_originalTransformRadarPointToScreen) return false;

    // Validate all four sites before touching any. ProperRadar and HUD packs
    // legitimately redirect the CSprite2d draw target, so only the call shape
    // itself is stable across this installation. Removing a thiscall without
    // replacing its callee cleanup would leave its two arguments on the stack,
    // so each E8 becomes `add esp,8; nop; nop`, not five NOPs.
    for (const uintptr_t callAddress : kRadarDiscDrawCalls) {
        auto* call = reinterpret_cast<const uint8_t*>(callAddress);
        if (call[0] != 0xE8) {
            logfile::Line("radar: bezel call missing at %p (%02X %02X %02X %02X %02X); layout kept inactive",
                          reinterpret_cast<void*>(callAddress), call[0], call[1],
                          call[2], call[3], call[4]);
            return false;
        }
    }

    const uint8_t replacement[] = {0x83, 0xC4, 0x08, 0x90, 0x90};
    for (const uintptr_t callAddress : kRadarDiscDrawCalls) {
        if (!WithWritable(callAddress, sizeof(replacement), [&] {
                memcpy(reinterpret_cast<void*>(callAddress), replacement,
                       sizeof(replacement));
            })) {
            logfile::Line("radar: could not replace stock bezel at %p",
                          reinterpret_cast<void*>(callAddress));
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(callAddress),
                              sizeof(replacement));
    }

    // The stock sites are only one path to the bezel. HUD ASIs commonly replay
    // a copied DrawRadar body, so also intercept the disc sprite itself.
    if (!HookSpriteDraw()) {
        return false;
    }

    g_radarBoxHudActive = true;
    logfile::Line("radar: GTA VI HUD frame active; stock circular bezel removed");
    return true;
}

bool InstallRadarMapScissorPatch() {
    if (g_originalDrawRadarMap) {
        return true;
    }
    auto* site = reinterpret_cast<uint8_t*>(kDrawRadarMap);
    const uint8_t expected[kDrawRadarMapPrologue] = {0x83, 0xEC, 0x2C, 0x53, 0x55};
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line("radar: DrawRadarMap entry changed - no scissor clip");
        return false;
    }
    auto* trampoline = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kDrawRadarMapPrologue + 5,
                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    memcpy(trampoline, site, kDrawRadarMapPrologue);
    trampoline[kDrawRadarMapPrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kDrawRadarMapPrologue + 1) =
        static_cast<int32_t>(kDrawRadarMap + kDrawRadarMapPrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kDrawRadarMapPrologue + 5);
    const int32_t replacement =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&DrawRadarMapReplacement)) -
        static_cast<int32_t>(kDrawRadarMap + 5);
    const bool ok = WithWritable(kDrawRadarMap, kDrawRadarMapPrologue, [&] {
        site[0] = 0xE9;
        *reinterpret_cast<int32_t*>(site + 1) = replacement;
    });
    if (!ok) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), site, kDrawRadarMapPrologue);
    g_originalDrawRadarMap = reinterpret_cast<DrawRadarMapFn>(trampoline);
    return true;
}

bool InstallSquareRadarPatch() {
    if (!g_ready) {
        return false;
    }
    if (g_originalDrawRadarMask) {
        return true;
    }

    // The first attempt at this simply wrote a `ret` over DrawRadarMask, which
    // squared the radar and also broke things that had nothing to do with it:
    // a join-sequence screen's own small map preview rendered full-screen and
    // unclipped, because that mask is what was keeping it inside its box.
    //
    // So this hooks the function rather than deleting it, and only skips the
    // mask for the in-game HUD radar. Every frontend screen - the pause map
    // included - still gets its corners cut exactly as before.
    auto* site = reinterpret_cast<uint8_t*>(kDrawRadarMask);
    const uint8_t expected[kDrawRadarMaskPrologue] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
    };
    if (memcmp(site, expected, sizeof(expected)) != 0) {
        logfile::Line("radar: DrawRadarMask entry changed (%02X %02X %02X %02X "
                      "%02X %02X) - leaving the round mask alone",
                      site[0], site[1], site[2], site[3], site[4], site[5]);
        return false;
    }

    auto* trampoline = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kDrawRadarMaskPrologue + 5,
                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    memcpy(trampoline, site, kDrawRadarMaskPrologue);
    trampoline[kDrawRadarMaskPrologue] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + kDrawRadarMaskPrologue + 1) =
        static_cast<int32_t>(kDrawRadarMask + kDrawRadarMaskPrologue) -
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(trampoline) +
                             kDrawRadarMaskPrologue + 5);

    const int32_t replacement =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(&DrawRadarMaskReplacement)) -
        static_cast<int32_t>(kDrawRadarMask + 5);
    const bool ok = WithWritable(kDrawRadarMask, kDrawRadarMaskPrologue, [&] {
        site[0] = 0xE9;
        *reinterpret_cast<int32_t*>(site + 1) = replacement;
        site[5] = 0x90;
    });
    if (!ok) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        logfile::Line("radar: could not make DrawRadarMask writable");
        return false;
    }

    FlushInstructionCache(GetCurrentProcess(), site, kDrawRadarMaskPrologue);
    g_originalDrawRadarMask = reinterpret_cast<DrawRadarMaskFn>(trampoline);

    // The mask hook alone is not safe to ship: skipping the circular mask
    // removes the clip that the gang overlay and blips are drawn against, so
    // the scissor that replaces it has to go in at the same time.
    if (!InstallRadarMapScissorPatch()) {
        logfile::Line("radar: scissor clip unavailable - restoring the round "
                      "mask rather than letting the overlay draw unbounded");
        WithWritable(kDrawRadarMask, kDrawRadarMaskPrologue, [&] {
            memcpy(site, expected, sizeof(expected));
        });
        FlushInstructionCache(GetCurrentProcess(), site, kDrawRadarMaskPrologue);
        g_originalDrawRadarMask = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    logfile::Line("radar: HUD radar corners uncut, clipped by scissor; "
                  "frontend mask retained");
    return true;
}

void BeginMapFrame() {
    g_overviewDrawn = false;
    if (g_ready && MapIsOpen() && !g_overviewLoadTried) {
        // Loading during the first section draw would stall in the middle of
        // the renderer. Do it at the start of that map frame instead.
        LoadOverviewTexture();
    }
}

Point MousePos() {
    // These two are the real screen cursor, kept as whole pixels.
    return {static_cast<float>(*reinterpret_cast<const int*>(kMouseScreenX)),
            static_cast<float>(*reinterpret_cast<const int*>(kMouseScreenY))};
}

MapView Map() {
    return {*reinterpret_cast<const float*>(kMapZoom),
            *reinterpret_cast<const float*>(kMapBaseX),
            *reinterpret_cast<const float*>(kMapBaseY)};
}

void SetMapView(MapView view) {
    *reinterpret_cast<float*>(kMapZoom) = view.zoom;
    *reinterpret_cast<float*>(kMapBaseX) = view.baseX;
    *reinterpret_cast<float*>(kMapBaseY) = view.baseY;
}

Point ScreenSize() {
    return {static_cast<float>(*reinterpret_cast<const int*>(kScreenWidth)),
            static_cast<float>(*reinterpret_cast<const int*>(kScreenHeight))};
}

// How much bigger this screen is than the 448-line space the game's interface
// was drawn for. Everything we size goes through this.
float UiScale() {
    const float height = static_cast<float>(*reinterpret_cast<const int*>(kScreenHeight));
    return height > 0.0f ? height / 448.0f : 1.0f;
}

float LineHeight() {
    return kLineBase * UiScale();
}

void DrawRect(float left, float top, float right, float bottom, uint32_t argb) {
    // CRect holds x1, y1, x2, y2 in that order.
    const Rect rect{left, top, right, bottom};

    // CRGBA is four bytes, red first, passed by const reference.
    const uint8_t colour[4] = {
        static_cast<uint8_t>((argb >> 16) & 0xFF),
        static_cast<uint8_t>((argb >> 8) & 0xFF),
        static_cast<uint8_t>(argb & 0xFF),
        static_cast<uint8_t>((argb >> 24) & 0xFF),
    };

    using Fn = void(__cdecl*)(const Rect&, const uint8_t(&)[4]);
    reinterpret_cast<Fn>(kSpriteDrawRect)(rect, colour);
}

// Set the font up the same way every time.
//
// Every one of these matters. The menu draws its own text immediately before we
// run and leaves the font however it liked - a shadow, an edge, centre
// alignment, a wrap width halfway across the screen. Anything not set here is
// inherited, and inherited state is why text that works one frame looks wrong
// the next.
static void PrepareFont(uint32_t argb, float scale) {
    // CFont::SetColor takes the colour BY VALUE - a four byte struct, which on
    // this ABI is passed as a plain 32 bit word. CSprite2d::DrawRect takes the
    // same struct by const reference, which is a pointer.
    //
    // Getting those two the same way round is not cosmetic. Passing a pointer
    // where a value is wanted hands the game a stack address as a colour, and
    // the top byte of a stack address is zero - so the alpha is zero and the
    // text draws perfectly, completely invisibly. Rectangles kept working
    // because a pointer is what they actually wanted, which is why the panels
    // showed up with nothing written on them.
    const uint32_t rgba = ((argb & 0x00FF0000) >> 16)         // red
                        | (argb & 0x0000FF00)                  // green
                        | ((argb & 0x000000FF) << 16)          // blue
                        | (argb & 0xFF000000);                 // alpha
    const uint32_t shadow = 0xFF000000;

    reinterpret_cast<void(__cdecl*)(short)>(kFontSetFontStyle)(kFontStyle);
    reinterpret_cast<void(__cdecl*)(bool)>(kFontSetProportional)(true);
    reinterpret_cast<void(__cdecl*)(bool, bool)>(kFontSetBackground)(false, false);
    reinterpret_cast<void(__cdecl*)(bool)>(kFontSetJustify)(false);
    reinterpret_cast<void(__cdecl*)(unsigned char)>(kFontSetOrientation)(kAlignLeft);
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetDropShadow)(1);
    reinterpret_cast<void(__cdecl*)(short)>(kFontSetEdge)(0);
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kFontSetDropColor)(shadow);
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kFontSetColor)(rgba);

    const float ui = UiScale();
    reinterpret_cast<void(__cdecl*)(float, float)>(kFontSetScale)(
        kFontScaleX * scale * ui, kFontScaleY * scale * ui);

    // No wrapping. Ours are single lines that we have already measured.
    const Point screen = ScreenSize();
    reinterpret_cast<void(__cdecl*)(float)>(kFontSetWrapx)(screen.x * 2.0f);
    reinterpret_cast<void(__cdecl*)(float)>(kFontSetRightJustifyWrap)(0.0f);
}

void PrintText(float x, float y, const char* text, uint32_t argb, float scale) {
    PrepareFont(argb, scale);
    reinterpret_cast<void(__cdecl*)(float, float, const char*)>(kFontPrintString)(x, y, text);
}

// Note on making a marker bigger, so nobody tries this again.
//
// The game will not resize a blip: its size setting applies only to the plain
// square markers, and a picture is drawn at a fixed size whatever it says.
//
// There IS a call that draws a blip picture at any width and height - and it
// needs a pointer into the game's table of pictures. That table moves: allowing
// more blip pictures than the game shipped with means putting them somewhere
// with room, so the stock address is wrong here.
//
// Searching the game's own code for the new address looks promising and is not
// safe. A heuristic cannot tell a table of textures from any other run of
// plausible numbers; it settled on an address inside another mod entirely, and
// handing that to the sprite drawing killed the game inside d3d9.
//
// The only sound way would be to read the limit adjuster's own record of what
// it moved and where. That is a great deal of work for a slightly larger icon,
// so the overlay rings the marker instead, which asks the game for nothing.

float TextWidth(const char* text, float scale) {
    PrepareFont(0xFFFFFFFF, scale);
    using Fn = float(__cdecl*)(const char*, bool, bool);
    return reinterpret_cast<Fn>(kFontGetStringWidth)(text, true, false);
}

}  // namespace game
