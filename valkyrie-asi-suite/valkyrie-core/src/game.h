// The bits of San Andreas we talk to.
//
// Every address here is Grand Theft Auto: San Andreas v1.0 US, and they were
// read out of plugin-sdk (github.com/DK22Pac/plugin-sdk, the -sa fork the
// server owner pointed at) rather than written from memory. Our players run the
// HOODLUM 1.0 build, which the crash logs confirm and which Init() checks for
// before anything is called or patched.
//
// One rule matters more than the rest: CALL the radar functions, never touch
// CRadar::ms_RadarTrace directly. GTA SA raises the blip limit from the
// stock 175 to 3000 through fastman92's limit adjuster, and the way that works
// is by allocating a bigger array somewhere else and rewriting every reference
// to it. The functions have been patched to use the new array; the old address
// at 0xBA86F0 still exists and is now a lie. Reading it would appear to work
// and would corrupt memory under the adjuster.
#pragma once

#include <cstdint>

namespace game {

// The vector the game passes around. Laid out to match, because we hand these
// straight to a game function by value.
struct Vector {
    float x, y, z;
};

// CRadar::SetCoordBlip's first argument. We only ever use BLIP_COORD - a plain
// marker at a position, which is what a shop or an empire site is.
enum BlipType : int {
    BLIP_NONE = 0,
    BLIP_CAR = 1,
    BLIP_CHAR = 2,
    BLIP_OBJECT = 3,
    BLIP_COORD = 4,
    BLIP_CONTACTPOINT = 5,
    BLIP_SPOTLIGHT = 6,
    BLIP_PICKUP = 7,
    BLIP_AIRSTRIP = 8,
};

// Whether a blip shows on the radar, as a ground marker, both or neither.
// Businesses want BLIP_ONLY: a marker column standing in the street outside
// every shop in the state would be absurd.
enum BlipDisplay : int {
    BLIP_DISPLAY_NEITHER = 0,
    BLIP_DISPLAY_MARKER_ONLY = 1,
    BLIP_DISPLAY_BLIP_ONLY = 2,
    BLIP_DISPLAY_BOTH = 3,
};

// True once we have confirmed the game is the build these addresses belong to.
// Nothing in here does anything until it is.
bool Init();

// Which build we found, for the log and for the version we report to the
// server. Empty until Init() has run.
const char* VersionName();

// Plants a blip and returns its handle, or 0 if the game's blip table is full.
// The handle packs an index and a generation counter, so it is not an array
// index and must not be treated as one.
//
// `longRange` picks which of the game's two functions does it, and the
// difference is the whole point of this plugin:
//
//   short range  on the radar only when you are near it, on the full map
//                always. What San Andreas uses for shops - open the map and
//                every Binco in the state is there, but your radar stays
//                readable while you drive.
//   long range   on the radar from anywhere, clamped to the rim when it is
//                too far to draw properly. Right for a handful of things,
//                ruinous for four hundred.
//
// Short range is the default for everything, because "the radar shows what is
// nearby and the map shows all of it" is the behaviour we are after.
int SetCoordBlip(BlipType type, Vector at, uint32_t colour, BlipDisplay display,
                 bool longRange = false);

// CRadar::SetBlipSprite - gives a blip its picture. Sprite numbers come from
// the server, and Eagle's own table runs to 130 where San Andreas stopped at
// 63; see data/gtasa_radarBlipSpriteFilenames.dat in the game folder.
void SetBlipSprite(int blip, int sprite);

// CRadar::ChangeBlipScale - blip size in radar units. The game uses 1 for the
// small dots and 2 for most things worth looking at.
void ChangeBlipScale(int blip, int size);

// CRadar::ClearBlip - takes one away again.
void ClearBlip(int blip);

// Paint a blip's sprite in a colour of our own.
//
// The game will not do it: a blip with a sprite is always drawn flat white,
// and the colour it was created with is only read for the plain square
// markers. This records what the sprite should be tinted to instead, keyed by
// the blip, and the draw hook puts it back at the last moment. Alpha is the
// game's - a blip still fades at the edge of the radar.
//
// InstallBlipTintPatch has to have gone in first; without it these are
// recorded and never used, which is the old white behaviour and not a fault.
void SetBlipTint(int blip, uint32_t argb);
void ClearBlipTint(int blip);
void ClearAllBlipTints();

// Is the player looking at the full map right now?
//
// This is the difference between the two things we have been asked for at once:
// the radar should only show what is nearby, and the map should show everything
// in every city. A blip cannot do both - the game has one flag for it, and a
// short range blip is culled by distance on the map as well as on the radar,
// which is why most of Los Santos was missing.
//
// So the answer is to stop it being one choice. Blips live as short range, and
// while the map is open they are long range instead.
bool MapIsOpen();

// Runtime map profile. Fastman's expanded map changes CRadar's world range
// from about 2990 to about 23920, making this independent of build packaging.
bool ExpandedMapActive();

// Any menu page, not just the map. Used to hold off the blip range switch until
// the player actually leaves the menu.
bool MenuIsOpen();

// Paint the text queued so far.
//
// CFont::PrintString only adds to a batch; nothing appears until this runs. The
// menu flushes its own text before we get control, so anything we queue after
// that is ours to flush. Forgetting it is why the first panels came out blank.
void FlushText();

// Install a callback to run once per frame, on the game's own thread.
//
// This is the only safe place to touch the radar. Everything else we do -
// reading the socket, parsing what the server sent - happens on our own thread
// and leaves work in a queue for this to pick up.
using FrameFn = void (*)();
bool HookFrame(FrameFn before, FrameFn after);

// Install a callback that runs after the menu has drawn itself, so we can paint
// on top of the map. Same thread, same rules as the frame hook.
bool HookMenuDraw(FrameFn fn);

// Install a callback that runs after the in-game HUD has drawn itself AND
// after its text has been flushed to the screen, so we can paint on top of it
// while the player is driving rather than only on the pause map. Same thread,
// same rules as the frame hook.
//
// The flush is the half that is easy to miss. CFont::PrintString only queues
// into a buffer, so a callback that runs merely "after the HUD" is still
// inside the game's unfinished text pass: preparing the font for our own text
// restyles the game's queued strings, and flushing to get ours drawn renders
// the game's as well - which it then renders again itself. See
// kHudAfterFadeCallSite in game.cpp for what that looks like on screen.
//
// This is a different call site from HookMenuDraw and the two do not overlap:
// the menu one only runs with the game paused, and this one only runs with it
// playing. A panel that should appear in both has to ask for both.
bool HookHudDraw(FrameFn fn);

// Is our HUD hook still the one the game calls?
//
// Worth asking, because hooking a shared call site is first-come and being
// first is the weak position - a component that patches the same site later
// chains onto whatever it finds and wins, and one that does not chain drops us
// with no error anywhere. A hook that reported success at startup and never
// runs is what that looks like.
bool HudHookIntact();


// Take it back if it has gone, chaining onto whoever holds it now so both
// still run. Cheap enough to call once a second from the frame hook; it does
// nothing at all while the hook is intact.
bool ReassertHudHook();

// ---------------------------------------------------------------------------
// The vehicle the player is in.
// ---------------------------------------------------------------------------

// A reading of the player's vehicle, taken all at once.
//
// Returned by value rather than as a pointer into the game, because a caller
// that holds a CVehicle* across frames is holding a pointer the game is free
// to free - a car the player leaves behind is deleted when it streams out, and
// the next read of it is a read of somebody else's memory.
//
// `handle` is the exception and is deliberately typed as void*: it is an
// IDENTITY, for telling "still the same car" from "a different car", and for
// handing back to the two setters below in the same frame. It must not be
// dereferenced by a caller and must not be kept past the frame it was read in.
struct VehicleState {
    void* handle;
    // When the game created this vehicle, in its own milliseconds. Paired with
    // `handle` it is what tells one car from another: the game reuses freed
    // memory, so the same address really does come back as a different car,
    // and anything remembering per-vehicle state keyed on the address alone
    // hands the new car the old car's tank.
    unsigned int createdAt;
    int model;
    float speed;   // metres per second, the same figure a speedometer shows
    float health;  // 1000 is undamaged; the game explodes the car at 0
    bool engineOn;
    bool engineBroken;
    bool playerIsDriver;  // false in a passenger seat, and for a driverless car
    float x, y, z;
};

// Read it. False when the player is on foot, which is the common case.
bool PlayerVehicleState(VehicleState& out);

// Stop a vehicle running, or let it run again.
//
// This is CVehicle's bEngineBroken, whose own comment in the reference headers
// is "engine does not work. Player can get in but the vehicle will not drive" -
// which is what an empty tank is. bEngineOn is NOT the flag for this: the game
// sets that one back by itself from the accelerator, because it is what decides
// whether an engine sound plays.
void SetVehicleEngineBroken(void* handle, bool broken);

// bEngineOn itself, for the sound. Worth clearing alongside the one above so a
// car that has run dry also stops sounding like it is running.
void SetVehicleEngineOn(void* handle, bool on);

// ---------------------------------------------------------------------------
// Drawing. All of it only makes sense from inside the menu-draw callback.
// ---------------------------------------------------------------------------

struct Point {
    float x, y;
};

// Convert a world position into the pause map's unrotated radar coordinate.
// Calling CRadar's ordinary transform outside the draw phase is wrong because
// it uses the driving radar's cached heading. This reads the live origin and
// Fastman-patched map range but deliberately applies no rotation.
Point WorldToMapRadar(float worldX, float worldY);

// Where a world position lands on screen, using the game's own two-step radar
// transform. Going through the game rather than working out the map's maths
// ourselves means our idea of where a blip is can never drift from where the
// game actually painted it - which is the whole point when the job is deciding
// what the cursor is pointing at.
Point WorldToScreen(float worldX, float worldY);

// Project a world point from the pause map's live zoom/base values directly.
// Big-map mods update those values independently of CRadar's cached screen
// transform, so overlays and the player marker must use this path.
Point WorldToMapScreen(float worldX, float worldY);

// Local navigation inputs. These read GTA's own player matrix and menu
// waypoint state, so they work without SA-MP or an SP-RP server.
bool PlayerPosition(Point& out);
// Current area code (zero outdoors). Only call on the game thread.
int PlayerInterior();
bool MapWaypoint(Point& out, int& handle);

// Where the game's own objective marker is, in WORLD coordinates - the yellow
// triangle a single-player mission puts on the radar.
//
// That marker is not an icon. It is a coord blip carrying no sprite at all
// (RADAR_SPRITE_NONE), which CRadar::DrawCoordBlip sends down its spriteless
// branch to be drawn by ShowRadarTraceWithHeight as a triangle pointing up or
// down depending on whether the objective is above or below the player, in
// whatever colour the mission script gave the blip. That is why it cannot be
// recognised the way a shop or a save disc is, and why looking for a sprite id
// for it was looking for something that does not exist.
//
// The position is recovered from the radar-space vector CRadar itself hands to
// the box transform, never from ms_RadarTrace, which the limit adjuster moves.
// See RecoverBlipWorldPosition in game.cpp.
//
// False when the game is not showing one. True writes the position and says
// how long ago it was seen, in milliseconds, so a caller can tell a marker
// that is live from one whose last sighting is stale because it has gone off
// the radar's range entirely.
// `rgb` comes back as 0x00RRGGBB - the colour the mission script gave the
// marker, read from the draw itself, or zero if that could not be hooked.
bool ObjectiveBlip(Point& out, unsigned long& ageMs, uint32_t& rgb);

// Read the objective marker's colour from CRadar's own draw, so a ribbon can
// be the same colour as the marker it leads to rather than a fixed guess at
// it. Optional: without it the configured mission colour stands.
bool InstallObjectiveColourPatch();

// Where the map's crosshair is pointing, in WORLD coordinates.
//
// Not a screen position, despite living next to the menu's other mouse fields.
// The map tracks its cursor in game space, which is why a reading of it looks
// like "2467, -1664" on a 2560x1440 screen - that is a street in Los Santos,
// not a pixel.
//
// This is the whole reason the hover panel never found anything: it was being
// compared against screen positions worked out from the radar transform, and
// the two were never going to meet. Nothing needs transforming at all - the
// cursor and the markers are already in the same space.
Point MapCursor();

// The current pause-map wheel direction after CPad::UpdatePads: +1 zooms in,
// -1 zooms out, and 0 means no wheel input this frame. Reading the input itself
// is essential at GTA's native 300/1100 clamps, where the resulting zoom delta
// becomes zero even though the player is still turning the wheel.
int MapZoomWheel();

// True while the physical left mouse button is held. The map module uses the
// state after CPad::UpdatePads, then replaces only GTA's abrupt camera delta;
// menu clicks and marker selection still pass through the native frontend.
bool MapLeftHeld();

// Take the pause-map wheel and final origin clamp away from the stock frontend.
// GTA SA scales the native wheel jump to 224 and can transiently drive
// the 300 overview down to 76; ProperRadar observes that invalid intermediate
// view and corrupts/flickers its tile LOD. The mapzoom module owns both pieces
// after this succeeds. Exact instruction signatures are checked before patching.
bool InstallMapControlPatch();

// Replace ProperRadar's thousands-of-tiles overview path with one pre-composed
// texture at distant and intermediate zoom levels. Its 96x96 source atlas is
// sound, but the runtime map hook produces flickering black columns while too
// many tiles remain visible. The ordinary detailed renderer is used up close.
bool InstallMapOverviewPatch();
bool InstallRadar3DBackgroundPatch();
bool InstallRadarBoxTransformPatch();
bool InstallRadarBoxPerimeterPatch();
bool InstallDrivingBlipFilterPatch();
// Keep the player marker fixed relative to the vehicle-heading-up 3D map and
// suppress territory/gang-zone overlays on that driving HUD only.
bool InstallDrivingRadarPresentationPatch();
bool InstallPauseMapPlayerMarkerPatch();
bool InstallRadarBoxHudFramePatch();
// another ASI's green in-vehicle route has a square-only clip. Replace it only for
// that draw with the same rectangle used by the 3D radar, retaining another ASI's
// own safe fallback if its installed build is not the one we validated.

// The small always-on radar is square underneath - CRadar composites its
// tiles into a plain rectangle. What makes it look round is a second pass,
// CRadar::DrawRadarMask, which paints an alpha-blended quarter-circle over
// each of the four corners to cut them away. Disabling that one function -
// not touching the tile composition, the blips, or the pause map's own
// separate overview path at all - is the entire difference between a round
// radar and a square one. Affects both the driving HUD radar and the pause
// map, since both call the same masking routine.
bool InstallSquareRadarPatch();

// Draw server markers in the colour the server sent, rather than in white.
bool InstallBlipTintPatch();

// Called once before the game's map draw so the overview replacement knows it
// may issue its single draw again. Individual section calls after that are
// deliberately suppressed for the rest of the frame.
void BeginMapFrame();

// The mouse in actual screen pixels, for putting a panel next to it.
Point MousePos();

// Where the map is scrolled and zoomed to. The pause map has its own view,
// separate from the radar's, and these are what describe it.
struct MapView {
    float zoom, baseX, baseY;
};
MapView Map();

// Move the complete map view. Zoom and origin have to change together: changing
// only zoom makes the map scale around GTA's fixed screen centre, while changing
// the origin by the matching amount keeps the world point under the cursor put.
// See client/valkyrie-asi-suite/src/mapzoom.cpp.
void SetMapView(MapView view);

// The back buffer size, for keeping panels on screen.
Point ScreenSize();

// How much bigger this screen is than the space the game's interface was drawn
// for. Multiply anything measured in pixels by this so a panel is the same size
// on a 4K monitor as on a small one.
float UiScale();

// The spacing to use between lines of text, already scaled. Ask rather than
// assume - guessing this is what turned the first legend into a black smear.
float LineHeight();

// A flat coloured rectangle. Corners are screen pixels.
void DrawRect(float left, float top, float right, float bottom, uint32_t argb);

// One line of text at a screen position. Colour is ARGB.
void PrintText(float x, float y, const char* text, uint32_t argb, float scale = 1.0f);

// How wide that text would be, for sizing a panel around it.
float TextWidth(const char* text, float scale = 1.0f);

// Explicit Lua HUD integration; independent of Atmosphere's HUD-hide policy.
bool LuaRadarLayout(float* xywh);
bool LuaRadarBackground();
}  // namespace game
