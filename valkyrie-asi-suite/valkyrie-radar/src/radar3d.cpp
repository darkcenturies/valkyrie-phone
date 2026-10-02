#include "radar3d.h"
#include "game.h"
#include "log.h"
#include "radarbox.h"
#include "radarcfg.h"
#include "radar_logo.h"
#include "water_mesh.h"
#include "map_tile_cache.h"
#include "router.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <d3d9.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

static void ApplyGpsRoute(const float* points, int count);
static void ApplyMissionRoute(const float* points, int count);

namespace radar3d {

// What a destination is, which decides how it is drawn and which of the two
// destinations wins the ribbon.
//
// A mission objective is not a place the player chose to go, it is the thing
// the game is currently asking them to do, so it outranks their own GPS
// destination and it is drawn in the gamemode's mission yellow rather than in
// the GPS pink. See Arbitrate.
enum class DestKind { Gps, Mission };

// One vertex of the GPS route ribbon.
struct RouteVert { float x, y, z; uint32_t colour; };

// The route, already turned into the two triangle strips that draw it.
//
// This is built once, when the server sends a route, rather than every frame.
// Nothing in it depends on where the player is or which way they are facing:
// the points are world positions, the heights come from the road itself, and
// the fade along the ribbon is a function of distance travelled along the
// route. So the same geometry is valid until a different route arrives, and
// drawing a frame costs two DrawPrimitiveUP calls and nothing else.
//
// It is handed between threads by shared_ptr so the render thread can take a
// reference under the lock and then draw without holding it, while the network
// thread builds a replacement without disturbing the frame in progress.
struct RouteGeometry {
  std::vector<RouteVert> outline;
  std::vector<RouteVert> fill;
  // The route position each pair of ribbon vertices straddles, x/y per pair.
  // Both strips step along the route together, so one list indexes either.
  // Used to find where along the ribbon the player currently is.
  std::vector<float> centres;
  // Distance along the route to each centre, one entry per centre pair. This
  // is what lets the map matcher work in arc length rather than in node
  // indices, so it can be given a search window measured in metres.
  std::vector<float> arc;
  float totalArc = 0.f;
  // Identifies this route. The matcher keeps a position along the route
  // between frames and has to know when it is looking at a different one;
  // comparing pointers would not do, because a freed route can be replaced by
  // a new one at the same address.
  uint32_t id = 0;
  // Whether this route actually gets to where it was asked to go, and by how
  // far it misses when it does not. The roads need not go all the way: a
  // jetty, an island, a spot in a field all route to the nearest road that can
  // be reached, exactly as the pause map's own GPS does, and the destination
  // dot covers the last stretch. What is refused is a route that does not take
  // the player there at all - see RequestLocalRoute.
  bool  endsAtDest = true;
  float endGap = 0.f;
  // What this ribbon was built to be, and the settings it was built from.
  //
  // Both are baked into the vertices - the colour literally is the vertex
  // colour, and the width is where the vertices are - so neither can be
  // changed at draw time. Recording them is what lets an ini edit, or a
  // mission taking the ribbon over from the GPS, be noticed and rebuilt.
  DestKind kind = DestKind::Gps;
  uint32_t settings = 0;
};

// Guards route geometry, destination state, and their epoch. Held only long
// enough to take or publish a coherent snapshot, never across drawing or route
// construction.
CRITICAL_SECTION g_routeLock{};
std::shared_ptr<const RouteGeometry> g_routeGeom;
// Whether the ribbon actually reached the panel on the last capture.
//
// Holding geometry is not the same as showing a route, and the difference is
// a real state rather than a theoretical one: the strips are drawn from the
// matched point onward, so a match landing at the far end of a short route
// leaves nothing after it to draw. The panel used to read "route ready" from
// the geometry alone, which meant that state showed no ribbon, no loading and
// the idle mark - the display saying nothing at all was happening while it
// was still trying to navigate. Written and read on the game thread only.
bool g_routeOnScreen{};
// Set when the road graph cannot reach the destination at all - there is no
// road within reach of it, or the best route stops short. It does not change
// by waiting or by driving somewhere else, so it ends the search rather than
// backing it off, stands the loading dots down, and hands the route back to
// the game's own GPS, which can at least draw a line to where the player is
// going. Guarded by g_routeLock with the destination it refers to.
bool g_routeUnreachable{};

namespace {
constexpr uintptr_t kVersionProbe = 0x401000, kMainDevice = 0xC97C28,
                    kScreenW = 0xC17044, kScreenH = 0xC17048,
                    kRadarAngle = 0xBA8310, kRadarRange = 0xBA8314,
                    kRadarOrigin = 0xBAA248, kFindPlayerPed = 0x56E210,
                    kFindPlayerVehicle = 0x56E0D0;
constexpr uint32_t kHoodlum = 0x16197BE9, kCompact = 0x53EC8B55;
constexpr float kTile = 512.f;

// Corner rounding, as a fraction of the shorter half-side.
constexpr float kCornerRadius = .18f;
constexpr int kCornerSegments = 8;
// Centre, one run of points per rounded corner, then a repeat of the first
// perimeter point to close the fan.
constexpr int kFanVerts = 4 * (kCornerSegments + 1) + 2;
// Camera tilt: how far the camera leans from straight-down, in radians.
//
// 0.88 rad is 50 degrees from vertical, so the camera is pitched 40 degrees
// below horizontal and looks down onto rooftops.
//
// This was 1.25 - only 18 degrees below horizontal - and that is too flat to
// match the reference, for a reason that can be worked out rather than
// eyeballed. Half of kFovY is 30 degrees, so the top of the frame sits at
// (pitch - 30) degrees; at 18 degrees of pitch that is 11.6 degrees *above*
// the horizon and sky is unavoidable. The reference minimap shows no sky at
// all and reads as rooftops rather than facades, which puts its pitch past 30
// degrees. At 40 the top of the frame is 10 degrees below the horizon, so the
// ground plane fills the panel with margin to spare.
//
// Raising the pitch also moves the camera up and in: about 18 units above the
// car and 11 behind, where before it was 12 up and 17 back.
constexpr float kTilt = 0.88f;

// Perspective projection parameters. The isolated renderer uses a perspective
// frustum so nearby road geometry is large and detail falls off into the
// distance, matching a physical dashboard camera rather than an orthographic
// top-down chart.
constexpr float kCamDist = 20.f;  // view-space depth of the player
constexpr float kFovY = 1.0472f;  // 60 degrees vertical
constexpr float kNear = 1.f;
constexpr float kFar  = 300.f;
// How far below view-space centre the player marker sits, so more road ahead
// is visible than behind - matches the reference minimap's lower-third
// player position instead of a dead-centre one. Derived from
// radarbox::kPlayerDropFraction (shared with Valkyrie Radar's native marker,
// which needs the same fraction applied in screen space - see that
// constant's comment) by inverting the perspective projection: a view-space
// Y shift of d at depth kCamDist lands at NDC y = -d * yScale / kCamDist,
// and NDC -1..1 spans the full panel height, so half-height fraction f
// wants NDC -2f.
const float kPlayerDrop =
    radarbox::kPlayerDropFraction * 2.f * kCamDist / (1.f / tanf(kFovY * .5f));

// The capture follows the panel's shape, and so does the projection.
//
// Both used to be pinned to numbers that did not agree with each other. The
// capture was a fixed 1024 square, and the projection used
// the panel box - 106/82, or 1.293 - as its aspect. But those are
// HUD units in GTA's 640x448 space and a HUD unit is not square on screen: it
// is (screenW/640) wide by (screenH/448) tall, which at 1920x1080 turns the
// panel into 318x198 real pixels, an aspect of 1.61. DrawDestMarker had
// already worked that out for the dot and said so in a comment; the projection
// never got the same treatment.
//
// Rendering at 1.293 and displaying at 1.61 stretches the world horizontally
// by a quarter. Everything on this panel has been that much too wide.
//
// So the aspect is measured from the panel at runtime, the projection uses it,
// and the capture is shaped to match so the stretch onto the panel is 1:1 and
// no axis is sampled harder than the other. Resize the panel in the ini, or
// change the screen resolution, and all of it follows.
// Bounded supersampling for a small HUD panel. The budget is the square of
// the ini's CaptureResolution, spread over the panel's aspect, so raising it
// sharpens the panel and costs a larger render and readback every frame.
constexpr UINT kCapMin = 128u, kCapMax = 2048u;
UINT  g_capW = 1024, g_capH = 1024;
float g_panelAspect = radarbox::Width() / radarbox::Height();

// The panel's aspect in real screen pixels.
float PanelPixelAspect() {
  const int sw = *(const int *)kScreenW, sh = *(const int *)kScreenH;
  if (sw <= 0 || sh <= 0) return radarbox::Width() / radarbox::Height();
  return (radarbox::Width() * (float(sw) / 640.f)) /
         (radarbox::Height() * (float(sh) / 448.f));
}

// Spread the capture budget over that aspect, in multiples of eight.
void CaptureSizeFor(float aspect, UINT &w, UINT &h) {
  if (!(aspect > .05f && aspect < 20.f)) aspect = 1.f;
  const double budget = double(radarcfg::Current().captureResolution) *
                        double(radarcfg::Current().captureResolution);
  double hh = sqrt(budget / aspect);
  double ww = hh * aspect;
  auto snap = [](double v) {
    UINT u = UINT((v + 7.0) / 8.0) * 8u;
    return u < kCapMin ? kCapMin : (u > kCapMax ? kCapMax : u);
  };
  w = snap(ww); h = snap(hh);
}

// The destination marker, in the square capture's own pixels.
//
// It is sized to match the N compass exactly, so the two markers riding the
// panel edge read as a pair rather than as two unrelated dots. game.cpp's
// ReshapeNorthSpriteRect gives N a half-size of 3.5 in HUD-Y units; the
// capture spans the panel height in those same units, so the
// identical radius here is simply that ratio. Deriving it rather than writing
// the number out means the two cannot drift apart if either is retuned.
// The dot's fill as a proportion of its outer radius. Was a 6-texel ring on a
// 1024 capture spanning the panel height in HUD units; kept as the proportion so
// it reads the same at any resolution now the dot is drawn on the HUD.
// Was the dot's fill as a share of its outer radius. The stroke is specified
// as a width now, so this is gone - see MarkerOutlineWidth in radarcfg.h.
// The dot's colours, its size and the ribbon's are all in valkyrie-radar.ini
// now - see radar_config.h. The values that used to be written here are the
// defaults there, so an install with no ini is unchanged.

// GTA's stock radar orientation follows the gameplay camera.  In a vehicle
// the navigation view instead needs to be heading-up: looking left/right must
// not swivel the map while the car keeps travelling in the same direction.
// CPlaceable owns a CMatrixLink pointer at +0x14. GTA's CMatrix layout is
// right/+0x00, forward/+0x10, up/+0x20, position/+0x30. Reading +0x20 here
// uses the car's vertical axis, whose XY projection is nearly zero; that was
// the cause of the unstable rotation and rejected/disappearing captures.
bool VehicleHeading(float &heading) {
  auto *vehicle = reinterpret_cast<uint8_t *(__cdecl *)(int, bool)>(
      kFindPlayerVehicle)(-1, false);
  if (!vehicle)
    return false;
  const auto matrix = *reinterpret_cast<uintptr_t *>(vehicle + 0x14);
  if (!matrix)
    return false;
  const float forwardX = *reinterpret_cast<const float *>(matrix + 0x10);
  const float forwardY = *reinterpret_cast<const float *>(matrix + 0x14);
  if (!std::isfinite(forwardX) || !std::isfinite(forwardY) ||
      forwardX * forwardX + forwardY * forwardY < .0001f)
    return false;
  heading = std::atan2(-forwardX, forwardY);
  return true;
}

// How often a fresh 3D capture is taken, and how far the view has to drift
// before one is considered due.
//
// Between captures the last image is reprojected every frame, so panning and
// rotation are already smooth; what only arrives with a new capture is
// parallax and anything that has actually moved. That makes these the
// numbers that decide whether the radar feels as live as GTA's own 2D one.
//
// While anything is moving, capture every frame: CaptureMovingMs is zero.
//
// Spacing captures out does not make motion smooth, it makes it jagged. The
// reprojection between captures is a flat 2D warp, and a tilted 3D view has
// parallax a 2D warp cannot reproduce, so error builds up over each gap and
// then snaps to zero the instant a real capture replaces it. At the previous
// 33 ms that was roughly thirty small pops a second, which is exactly what
// rotating on the spot looked like - and rotating loads no tiles, so nothing
// else could have caused it.
//
// Capturing every frame at the live position and heading means the warp is
// always an identity and there is no error to pop. The thresholds are near
// zero for the same reason: any movement at all should take a fresh capture
// rather than lean on the warp. Standing still is the one case that can be
// spaced out, because then nothing in the image is changing.
//
// The cost is one render of a few cached tiles plus a readback of the capture
// surface per frame, on the isolated device rather than GTA's. If that turns
// out to be too expensive on lower-end machines, CaptureMovingMs in the ini
// is the dial:
// raising it trades smoothness back for frame time.
// Both are settings - see radarcfg.h - and these are their defaults.
inline DWORD MovingWaitMs() { return radarcfg::Current().captureMovingMs; }
inline DWORD SettledWaitMs() { return radarcfg::Current().captureSettledMs; }
constexpr float kMoveFraction = .002f;
constexpr float kAngleTolerance = .002f;
constexpr float kRangeFraction = .01f;
constexpr UINT kValidateStride = 2;

// How far a corner is pulled toward the line between its neighbours, at the
// sharpest possible turn. Raise it to cut corners harder; too high and the
// route visibly leaves the road on tight bends.
constexpr float kCornerCut = .45f;

// World-space extent each capture must cover so the tile loader fetches enough
// geometry to fill the perspective frustum. Derived from FOV and far clip so
// changing kFovY or kFar updates this automatically.
inline float CaptureRange(float /*radarRange*/) {
  return kFar * tanf(kFovY * .5f) * g_panelAspect + kCamDist;
}
std::string g_root;
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *,
                                               const RECT *, HWND,
                                               const RGNDATA *);
PresentFn g_originalPresent{};
using ResetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
ResetFn g_originalReset{};
std::atomic<bool> g_navigationActive{false};
// Started for the phone's Maps app only (InitForMaps).
bool g_mapsOnly = false;
#pragma pack(push, 1)
struct GHeader {
  char magic[4];
  uint32_t nv, ni;
};
struct GroupHeader {
  float minx, miny, minz, maxx, maxy, maxz;
  uint32_t flags, ni;
};
struct AHeader {
  char magic[4];
  uint32_t w, h;
};
struct Vertex {
  float x, y, z;
  uint32_t colour;
  float u, v;
};
#pragma pack(pop)
struct DrawGroup {
  IDirect3DIndexBuffer9 *ib{};
  uint32_t ni{}, flags{};
  float minx{}, miny{}, minz{}, maxx{}, maxy{}, maxz{};
  float opacity{1.f};
  bool occludedNow{};
  // Retained only for occluder groups. Bounds identify cheap candidates; the
  // indices then let us test the real triangles instead of guessing from the
  // centre of a 128-unit cell.
  std::vector<uint32_t> collisionIndices;
  ~DrawGroup() { if (ib) ib->Release(); }
};
struct Position { float x, y, z; };
struct Tile {
  IDirect3DVertexBuffer9 *vb{};
  IDirect3DTexture9 *tex{};
  uint32_t nv{};
  std::vector<DrawGroup *> groups;
  std::vector<Position> collisionVertices;
  ~Tile() {
    if (tex)
      tex->Release();
    for (auto *g : groups) delete g;
    if (vb)
      vb->Release();
  }
};
std::map<std::pair<int, int>, Tile *> g_tiles;
std::map<std::pair<int, int>, DWORD> g_tileRetry;
IDirect3D9 *g_d3d{};
IDirect3DDevice9 *g_iso{};
HWND g_isoWindow{};
// g_rtMS is the multisampled surface the scene is actually drawn into, when
// the hardware offers multisampling. It cannot be read back directly - a
// multisampled surface has several samples per pixel and no single value to
// hand over - so it is resolved into g_rt, which is what the readback then
// takes. Without multisampling g_rtMS stays null and the scene is drawn
// straight into g_rt, exactly as before.
IDirect3DSurface9 *g_rt{}, *g_rtMS{}, *g_depth{}, *g_read{};
D3DMULTISAMPLE_TYPE g_samples = D3DMULTISAMPLE_NONE;
// Anisotropy actually granted by the device, clamped to what it reports.
DWORD g_anisotropy = 1;
// Device can blend against a standalone blend factor, so a building can fade
// out without its colour being dragged toward the fog at the same time.
bool g_blendFactor = false;
// Device can bias a primitive's depth, so the route ribbon can win against the
// road it lies on without being lifted off it. See DrawGpsRoute.
bool g_depthBias = false;
// Whether the device offers a second fixed-function blend stage.
//
// The world needs two: one to put the surface's own colour onto its texture,
// and one to blend the result toward the horizon for the navigation grade.
// Two stages is the floor for any card that ever ran San Andreas, so the
// single-stage path below is a formality rather than a real expectation.
bool g_twoStage = false;
// Anisotropy the game's own device grants for the composite draw. Zero until
// asked; see Circle.
DWORD g_compositeAniso = 0;
// How much of the camera's full boom length is actually usable, 0..1.
//
// The camera sits about seventeen units behind the car and twelve above it,
// and nothing stopped that point ending up inside a hillside, a road
// embankment or a flyover deck. When it did, the panel filled with the inside
// of that surface and the car, the road and the route all vanished behind it.
// Fading cannot answer this: the offender is usually ground, and ground is
// drawn in 128-unit cells, so dissolving one would take the road out from
// under the player as well and show the void through it.
//
// So the camera is pulled in along its own boom until it can see the car
// again, exactly as a third-person camera does. See the boom test in
// RenderIso. Held here because it is measured after the tiles for the frame
// are loaded and applied to the next frame's matrix, which at one capture per
// frame is not a lag anyone can see.
float g_camScale = 1.f;
DWORD g_captureClearColor = 0;
IDirect3DTexture9 *g_mainTex{};

// The maker's mark in the panel corner, and the frosting behind the panel.
//
// The mark is built once from the alpha mask compiled into the ASI. Managed
// pool, so a device reset does not take it.
//
// The blur target is not managed - a render target cannot be - so it is
// dropped when the device is lost and made again afterwards. Losing it costs
// nothing: without it the frosting is skipped for a frame and the panel simply
// sits on the unblurred world, which is what it did before any of this.
IDirect3DTexture9 *g_logoTex{};
IDirect3DTexture9 *g_blurTex{};
IDirect3DSurface9 *g_blurSurf{};

// How much of the world shows through the panel, and what is done to it.
//
// The panel is drawn at 0xE6 - ninety percent - so a tenth of the game reads
// through it. Downsampling that tenth into a small target and stretching it
// back is a blur: the hardware's bilinear filter does the averaging, so it
// costs one copy and one quad rather than a shader and a dozen taps. Smaller
// is blurrier; 40 across the panel's width is heavy frosting, which is what
// suits something read at a glance while driving.
constexpr UINT kBlurSize = 40;

// The mark, as a fraction of the panel. Sized and placed off the reference:
// small, tucked into the corner, and faint enough to read as a moulding in the
// bezel rather than as anything to look at.
constexpr float kLogoHeightFraction = 0.13f;
constexpr float kLogoInsetFraction  = 0.045f;
constexpr uint8_t kLogoAlpha        = 0x3A;
// Idle branding shares the top-left status slot with route loading and route
// distance. Exactly one of those three states is visible at a time.
constexpr bool kLogoAtTop = true;
constexpr bool kLogoAtLeft = true;
DWORD g_last{}, g_composited{};
float g_capX{}, g_capY{}, g_capZ{}, g_capRange{}, g_capAngle{};
bool g_capValid{};

// A GPS route arrives one of two ways.
//
// The server sends one through SprpRadar3DSetGpsRoute when the gamemode sets a
// destination. Failing that we work one out ourselves, from the waypoint the
// player put on the pause map, so the GPS also works with no server at all.
// A server route always wins while it lasts: it is the destination the player
// actually asked the gamemode for, and it can be somewhere the game's own
// waypoint has no way to express.
float g_waypointX{}, g_waypointY{};
bool  g_waypointActive{};

// Where a destination came from. Only its own source may cancel it: clearing
// the map waypoint must not cancel a job's destination, a server route ending
// must not cancel a waypoint the player placed by hand, and neither may cancel
// the objective of a mission the game itself is running.
enum class DestSource { None, Waypoint, Server, Objective };


// Finding the game's own map waypoint.
//
// CMenuManager is a static object, so the two fields this needs are at fixed
// addresses. That is the whole point. The radar trace array is not at a fixed
// address - the limit adjuster relocates it - and an earlier attempt to find
// the waypoint by scanning that array at its stock address read dead memory
// and silently never found anything, which is why routing moved to the server
// in the first place.
//
// m_nTargetBlipIndex is the handle of the waypoint's blip, zero when there is
// none. m_vMousePos is the red map marker in world coordinates, which is the
// crosshair the waypoint gets placed under. Watching the handle for a change
// and reading the position when it changes gives us the destination without
// touching the relocated array and without hooking anything.
//
// The layout is confirmed against six offsets this suite already relies on:
// +0x59 drawing-map, +0x5C menu-active, +0x64 zoom, +0x68 and +0x6C map
// origin, +0x70 cursor all fall exactly where the reversed header puts them.
constexpr uintptr_t kMenuManagerBase = 0xBA6748;
constexpr uintptr_t kTargetBlipIndex = kMenuManagerBase + 0x2C; // int32 handle
constexpr uintptr_t kMapCursorWorld  = kMenuManagerBase + 0x70; // CVector2D

// How often to work a local route out again. Newson and Krumm measured only
// 0.11% route error at thirty-second sampling, so rerouting often buys far
// less than it looks like it should; the short interval is for the case that
// actually needs it, which is having left the route and wanting a new one.
constexpr DWORD kLocalRerouteMs  = 400;

// Close enough to the destination to call it reached. The gamemode uses a
// five unit arrival circle; this is wider to catch stops near a checkpoint
// even when the server's road-snap point is not exactly where the player parks.
constexpr float kArriveDist = 25.f;

// How far off the road network a destination may be and still be treated as a
// place you can drive to.
//
// This is the number that decides both halves of the same question. A route
// that stops this far short is still the trip - a jetty, a car park, a door on
// the far side of a building - so it is drawn. And arrival has to allow for
// it, because that shortfall is exactly how close a car can physically get: a
// destination 83 units off the road, judged at kArriveDist, means the player
// drives the whole route, parks at the end of it, and never arrives at all.
// The dot then stays up for the rest of the session with the journey over,
// which is what this is here to stop.
//
// It also caps how much slack arrival gets. A best-effort route across a bay
// can stop a kilometre short, and standing on the far shore is not arriving.
constexpr float kOffRoadReach = 150.f;

// How far a re-sent destination has to have moved to count as a different one.
// The server re-sends the destination it is already navigating to every few
// seconds; only a real change may disturb the ribbon. See
// SprpRadar3DSetGpsRoute.
constexpr float kSameDest = 2.f;

// How long everything has to stop asking for a finished destination before it
// may be set again. See PollArrival, where the guard is released.
constexpr DWORD kSpentQuietMs = 5000;

// How much of the way to the destination a route has to cover when the roads
// stop short of it.
//
// Routing to the nearest reachable road is right nearly always - it is what
// the pause map does, and the last stretch is the driver's. It is wrong in one
// case, and only a measurement tells the two apart: when the nearest reachable
// road is not on the way at all. Click an island from the far shore and the
// road that ends up closest to it can be the one behind the car, or across a
// bay in the opposite direction, and a ribbon drawn to it points the player
// confidently the wrong way. So a route that stops short has to have closed at
// least half the distance, or it is not taking them there and the destination
// dot alone is the honest answer.
constexpr float kMustClose = .5f;

// How close a destination has to be to the pause-map marker to be the marker's
// own destination rather than an unrelated one.
//
// Not kArriveDist, which is far too tight for the job. A server turns the map
// click into a destination by snapping it to the nearest road, and that road
// can be the better part of a hundred units from the click - so the marker the
// player had just placed came back as a destination this side read as
// somewhere else entirely, and clearing the marker was then judged not to
// concern it. The bound is the router's own snap radius, because the snap is
// what opens the gap.
constexpr float kMarkerSnap = 120.f;

// How long the player has to be off the route before a new one is worked out.
// See RequestLocalRoute: without it a cross-track distance sitting near
// kOffRouteDist reroutes on a loop and the ribbon blinks each time.
constexpr DWORD kOffRouteHold = 900;

int32_t g_lastBlipHandle{};

// The blip handle the remembered waypoint was read from.
//
// The gamemode can put its own scripted checkpoint behind the game's target
// blip, and when it does, the handle is the checkpoint's rather than the
// player's. Remembering which handle the coordinates came from is what
// separates "the player's waypoint is still there underneath" from "that
// handle now belongs to something else", and it is the difference between
// restoring the waypoint on cancel and resurrecting a ghost marker.
int32_t g_waypointHandle{};
std::mutex g_routeInputLock;
std::vector<std::vector<float>> g_routeInputs;
// The same queue again for mission objectives, which arrive on the same
// network thread through their own export and are applied to their own slot.
std::vector<std::vector<float>> g_missionInputs;

void DrainRouteInputs() {
  std::vector<std::vector<float>> inputs, missions;
  {
    std::lock_guard<std::mutex> guard(g_routeInputLock);
    inputs.swap(g_routeInputs);
    missions.swap(g_missionInputs);
  }
  for (const auto& input : inputs)
    ApplyGpsRoute(input.data(), static_cast<int>(input.size()));
  // Missions after the GPS destinations, so that a frame carrying both ends
  // with the mission holding the ribbon rather than depending on the order
  // two messages happened to arrive in.
  for (const auto& input : missions)
    ApplyMissionRoute(input.data(), static_cast<int>(input.size()));
}

// The destination, and where it came from, as one value.
//
// These are written from two threads - the network thread when the server
// sets or cancels a destination, and the present/render thread when the
// player places a waypoint or arrives at one - and they only mean anything
// together: a route is worked out from x and y, and neither says anything
// without `active`. As loose globals a search could run to a mixed old-x and
// new-y point, and `active` could be seen before the coordinates it refers
// to. So they live behind g_routeLock, which is recursive, is never held
// across a draw or a build, and is already taken on every path that touches
// them.
struct DestState {
  float x = 0.f, y = 0.f;
  bool  active = false;
  DestSource source = DestSource::None;
  DestKind kind = DestKind::Gps;
};

// The two destinations the player can have at once, and the one being
// navigated to.
//
// A mission objective and a GPS destination are separate things that exist
// independently: taking a job does not clear the marker somebody put on the
// map, and clearing that marker does not abandon the job. So both are held,
// and both are drawn as a dot on the panel.
//
// Only one of them gets a ribbon. Two ribbons leaving the player arrow in
// different directions is not navigation, it is a choice presented badly, and
// the mission is the one the game is actually asking for - so the mission wins
// it whenever there is one, and the GPS destination falls back to a dot alone
// until the mission is over. See Arbitrate, which is the only thing that
// writes g_dest.
//
// g_dest is a copy of whichever slot currently holds the ribbon, rather than a
// pointer into them, because every existing consumer - the matcher, arrival,
// the local road search, the marker - reads a destination as a plain value and
// none of them should have to learn where it came from.
DestState g_gpsDest, g_missionDest, g_dest;

// The colour the game draws the objective marker in, 0xAARRGGBB, or zero when
// it is not known - the hook that reads it is optional, and on an executable
// whose entry it cannot verify it stands down rather than guess.
//
// When it is known the mission ribbon uses it, so the line and the marker it
// leads to are the same colour by construction rather than by two settings
// being kept in step by hand. When it is not, the configured MissionRibbon
// stands, which is what it did before any of this.
std::atomic<uint32_t> g_objectiveColour{0};

DestState Dest() {
  EnterCriticalSection(&g_routeLock);
  const DestState d = g_dest;
  LeaveCriticalSection(&g_routeLock);
  return d;
}

// The destination that is not being navigated to, or an inactive one when
// there is only the one. This is what gets drawn as a dot with no ribbon.
DestState SecondaryDest() {
  EnterCriticalSection(&g_routeLock);
  const DestState d =
      (g_missionDest.active && g_gpsDest.active) ? g_gpsDest : DestState{};
  LeaveCriticalSection(&g_routeLock);
  return d;
}

// A destination this side has finished with, and the server has not.
//
// The server reroutes on a timer - every 500 ms in the gamemode - and it does
// not stop when the journey is over. It cannot: it does not know. Arriving is
// noticed here, several seconds before the server hears about it, and clearing
// the map marker is noticed ONLY here, because SA-MP has no callback for a
// player right-clicking their waypoint away. Either way the next re-send is a
// destination that is already dealt with, and taking it at face value puts the
// pink dot and the ribbon straight back up - which is exactly what it did.
//
// So a finished destination is remembered, and re-sends of it are refused
// until something says the player wants it again.
//
// One guard per kind of destination, because a mission objective and a GPS
// destination can be the same place - drive to a job's drop-off, then take a
// mission whose objective is the yard next door - and arriving at one of them
// must not refuse the other. The kind indexes these.
float g_spentX[2]{}, g_spentY[2]{};
bool  g_spentActive[2]{};
// When something last asked for the finished destination again.
//
// Silence is what releases the guard, and distance is not. Distance was the
// old rule and on a server it is not a rule at all: the gamemode's arrival
// circle is five units across and this one is twenty-five, so a player who
// parks a little short and drives on has arrived here and not there, and the
// reroute timer runs for ever. Three seconds later they were clear of the
// arrival circle, the guard lifted, the very next re-send landed, and the
// ribbon and the pink dot came back for a place already reached - with
// nothing on the pause map, because the marker had gone.
//
// A re-send refreshes this, so a timer that never stops can never lift the
// guard. A gamemode that has finished with the destination stops sending, and
// the guard lifts a few seconds later, which is what still lets the same job
// be taken a second time. A marker still sitting on the pause map counts as
// asking too - that is the map still holding the destination open.
DWORD g_spentSeenAt[2]{};

inline int SpentSlot(DestKind kind) { return kind == DestKind::Mission ? 1 : 0; }

// Whether (x, y) is the destination of this kind already finished with.
bool IsSpent(DestKind kind, float x, float y) {
  const int slot = SpentSlot(kind);
  if (!g_spentActive[slot]) return false;
  const float dx = x - g_spentX[slot], dy = y - g_spentY[slot];
  return dx * dx + dy * dy <= kArriveDist * kArriveDist;
}

void MarkSpent(DestKind kind, float x, float y) {
  const int slot = SpentSlot(kind);
  g_spentX[slot] = x;
  g_spentY[slot] = y;
  g_spentActive[slot] = true;
  g_spentSeenAt[slot] = GetTickCount();
}

// Something is still asking for the finished destination, so the guard stands.
void TouchSpent(DestKind kind) { g_spentSeenAt[SpentSlot(kind)] = GetTickCount(); }
// Where each destination dot goes, as a fraction of the panel box, worked out
// during the capture and drawn on the HUD afterwards. See DrawDestMarker.
//
// Two of them: the destination being navigated to, and the one that is not.
// Both are placed by the same projection and both ride the panel rim the same
// way; the only thing that separates them is the colour they are drawn in,
// which the kind decides.
struct MarkerPlacement {
  float u{}, v{}, fade{};
  bool on{};
  DestKind kind = DestKind::Gps;
};
MarkerPlacement g_markers[2];
std::atomic<bool> g_localRouteBusy{false};
std::atomic<uint32_t> g_routeEpoch{1};
DWORD   g_lastLocalRoute{};
// Where the last search was started from, and how far the player has to have
// moved for a failed one to be worth trying again straight away. A search that
// found nothing because there was no road under the player - parked in a yard,
// spawned in a lock-up, stopped on a rooftop - has a different answer from the
// next street, and it should not be sitting out a backoff meant for a
// destination the graph cannot reach at all. That was the first marker of a
// session appearing to hang on the loading dots long after the player had
// driven onto a road.
float   g_lastTryX{}, g_lastTryY{};
constexpr float kRetryMove = 50.f;
// And how far before a destination the graph would not route to is worth
// asking about again. Larger, because that answer only changes over distance.
constexpr float kUnreachableRetryMove = 500.f;
// When the player was first seen off the route, or zero while they are on it.
DWORD   g_offRouteSince{};
// Consecutive failed route searches. Backs the retry interval off so a
// destination the graph genuinely cannot reach does not spawn a worker thread
// and a log line several times a second for as long as it is set.
std::atomic<int> g_routeFailures{0};
int g_rejectLogged{}, g_updateLogged{};
int g_sampleRejectLogged{};
int g_lastMinX{}, g_lastMaxX{}, g_lastMinY{}, g_lastMaxY{}, g_lastTilesLoaded{},
    g_lastDraws{};
// The last capture drew the sea and nothing else. Out on the water that is the
// correct picture, not a failed render - see RenderIso's draw count and the
// luminance check in Update, both of which would otherwise throw it away.
bool g_lastSceneWaterOnly{};

// Point the ribbon at whichever destination currently outranks the other.
//
// Called after anything changes either slot, on the game thread, with
// g_routeLock NOT held - it takes the lock itself. When the winner changes the
// route built for the old one is thrown away and the epoch is bumped, so a
// road search already in flight for it cannot publish its answer over the top
// of the new one. When the winner has not changed this does nothing at all,
// which matters: the server re-sends its destination twice a second and every
// one of those comes through here.
void Arbitrate() {
  EnterCriticalSection(&g_routeLock);
  const DestState winner =
      g_missionDest.active ? g_missionDest : g_gpsDest;
  const bool changed = winner.active != g_dest.active ||
                       winner.kind != g_dest.kind ||
                       fabsf(winner.x - g_dest.x) >= kSameDest ||
                       fabsf(winner.y - g_dest.y) >= kSameDest;
  if (changed) {
    g_dest = winner;
    g_lastLocalRoute = 0;  // route to it on the next frame
    g_routeFailures = 0;
    g_offRouteSince = 0;
    g_routeGeom.reset();
    g_routeOnScreen = false;
    g_routeUnreachable = false;
    g_routeEpoch.fetch_add(1);
  } else {
    // The same place, but possibly now held by the other slot - a mission
    // ending on top of an identical GPS destination, say. The kind decides
    // the colour, so it is taken even when nothing has to be rebuilt.
    g_dest.source = winner.source;
  }
  LeaveCriticalSection(&g_routeLock);
  if (!changed) return;
  if (winner.active)
    logfile::Line("radar3d: the ribbon now leads to the %s destination "
                  "%.1f, %.1f",
                  winner.kind == DestKind::Mission ? "mission" : "gps",
                  winner.x, winner.y);
  else
    logfile::Line("radar3d: no destination left; the ribbon is down");
}

// Take the ribbon, the marker and the destination down together.
//
// Per slot, because the two destinations end separately: a mission objective
// is reached while the player still has somewhere marked on the map, and the
// map marker is cleared while the mission carries on. Ending one hands the
// ribbon to the other if there is one, which is what Arbitrate does.
void EndNavigation(DestKind kind) {
  EnterCriticalSection(&g_routeLock);
  (kind == DestKind::Mission ? g_missionDest : g_gpsDest) = {};
  LeaveCriticalSection(&g_routeLock);
  Arbitrate();
}

void EndGpsNavigation() { EndNavigation(DestKind::Gps); }

// Rebuild the ribbon when the settings it was built from have changed.
//
// Its colour and its width are vertex data, so an ini edit cannot be picked up
// at draw time - the geometry has to be made again. Throwing it away is enough
// to do that: the road search notices there is no route and runs, which is the
// same path a reroute takes, so the ribbon comes back a frame or two later in
// the new colour.
void PollSettings() {
  const uint32_t generation = radarcfg::Generation();
  bool stale = false;
  EnterCriticalSection(&g_routeLock);
  if (g_routeGeom && g_routeGeom->settings != generation) {
    g_routeGeom.reset();
    g_routeOnScreen = false;
    g_lastLocalRoute = 0;
    g_routeEpoch.fetch_add(1);
    stale = true;
  }
  LeaveCriticalSection(&g_routeLock);
  if (stale)
    logfile::Line("radar3d: settings changed; rebuilding the route ribbon");
}

bool Executable(const void *p) {
  MEMORY_BASIC_INFORMATION m{};
  if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT ||
      (m.Protect & PAGE_GUARD))
    return false;
  DWORD q = m.Protect & 0xff;
  return q == PAGE_EXECUTE || q == PAGE_EXECUTE_READ ||
         q == PAGE_EXECUTE_READWRITE || q == PAGE_EXECUTE_WRITECOPY;
}
bool FilePresent(const char *n) {
  char p[MAX_PATH]{};
  GetModuleFileNameA(nullptr, p, MAX_PATH);
  if (char *s = strrchr(p, '\\'))
    s[1] = 0;
  strncat_s(p, n, _TRUNCATE);
  return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
template <class F> bool Writable(uintptr_t p, size_t n, F &&f) {
  DWORD old;
  if (!VirtualProtect((void *)p, n, PAGE_EXECUTE_READWRITE, &old))
    return false;
  f();
  VirtualProtect((void *)p, n, old, &old);
  return true;
}
std::string Path(int x, int y, const char *ext) {
  return g_root + "\\" + std::to_string(x) + "_" + std::to_string(y) + ext;
}
void DropIso() {
  for (auto &p : g_tiles)
    delete p.second;
  g_tiles.clear();
  g_tileRetry.clear();
  if (g_read)
    g_read->Release();
  if (g_rtMS)
    g_rtMS->Release();
  if (g_depth)
    g_depth->Release();
  if (g_rt)
    g_rt->Release();
  if (g_iso)
    g_iso->Release();
  if (g_d3d)
    g_d3d->Release();
  if (g_isoWindow)
    DestroyWindow(g_isoWindow);
  g_read = nullptr;
  g_rtMS = nullptr;
  g_depth = nullptr;
  g_rt = nullptr;
  g_iso = nullptr;
  g_d3d = nullptr;
  g_isoWindow = nullptr;
}

// The isolated renderer must not use GTA's window as its focus window. D3D9
// watches that handle for foreground/background transitions, and attaching a
// second device to it made Alt+Tab pause the game without allowing Windows to
// bring the selected application forward. This private window is never shown
// or activated; it exists only to own the offscreen device.
LRESULT CALLBACK IsoWindowProc(HWND wnd, UINT message, WPARAM wparam,
                               LPARAM lparam) {
  return DefWindowProcA(wnd, message, wparam, lparam);
}

bool MakeIsoWindow() {
  if (g_isoWindow && IsWindow(g_isoWindow)) return true;
  static const char kClass[] = "ValkyrieRadarOffscreen";
  static ATOM atom{};
  HINSTANCE instance = GetModuleHandleA(nullptr);
  if (!atom) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = &IsoWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClass;
    atom = RegisterClassA(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  }
  g_isoWindow = CreateWindowExA(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, "", WS_POPUP,
      -32000, -32000, 1, 1, nullptr, nullptr, instance, nullptr);
  return g_isoWindow != nullptr;
}

bool MakeIso() {
  auto *main = *(IDirect3DDevice9 **)kMainDevice;
  if (!main || main->TestCooperativeLevel() != D3D_OK) return false;
  if (g_iso) {
    if (g_iso->TestCooperativeLevel() == D3D_OK) return true;
    DropIso();
    g_capValid = false;
  }
  g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!g_d3d)
    return false;
  if (!MakeIsoWindow()) {
    DropIso();
    return false;
  }
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_isoWindow;
  pp.BackBufferWidth = g_capW;
  pp.BackBufferHeight = g_capH;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  HRESULT hr =
      g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_isoWindow,
                          D3DCREATE_HARDWARE_VERTEXPROCESSING |
                              D3DCREATE_NOWINDOWCHANGES, &pp, &g_iso);
  if (FAILED(hr)) {
    DropIso();
    return false;
  }
  // Best multisampling the card will give us for both the colour and depth
  // formats. Asking for one without the other is not enough: the depth surface
  // has to carry the same sample count as the colour surface or the pair
  // cannot be bound together.
  g_samples = D3DMULTISAMPLE_NONE;
  DWORD msQuality = 0;
  for (D3DMULTISAMPLE_TYPE want :
       {D3DMULTISAMPLE_2_SAMPLES}) {
    DWORD colourQuality = 0, depthQuality = 0;
    if (SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, want,
            &colourQuality)) &&
        SUCCEEDED(g_d3d->CheckDeviceMultiSampleType(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, want,
            &depthQuality))) {
      g_samples = want;
      // The quality range is per-format; the level has to be legal for both.
      msQuality = (colourQuality < depthQuality ? colourQuality : depthQuality);
      if (msQuality) msQuality--;
      break;
    }
  }

  // The resolve target and readback source. Never multisampled: this is the
  // surface GetRenderTargetData copies from.
  hr = g_iso->CreateRenderTarget(g_capW, g_capH, D3DFMT_A8R8G8B8,
                                 D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, nullptr);
  if (SUCCEEDED(hr) && g_samples != D3DMULTISAMPLE_NONE) {
    // A multisampled render target cannot be lockable.
    if (FAILED(g_iso->CreateRenderTarget(g_capW, g_capH, D3DFMT_A8R8G8B8,
                                         g_samples, msQuality, FALSE, &g_rtMS,
                                         nullptr))) {
      g_rtMS = nullptr;
      g_samples = D3DMULTISAMPLE_NONE;
    }
  }
  if (SUCCEEDED(hr))
    hr = g_iso->CreateDepthStencilSurface(g_capW, g_capH, D3DFMT_D24S8,
                                          g_samples, msQuality, TRUE,
                                          &g_depth, nullptr);
  if (SUCCEEDED(hr))
    hr = g_iso->CreateOffscreenPlainSurface(
        g_capW, g_capH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_read, nullptr);
  if (FAILED(hr)) {
    DropIso();
    return false;
  }
  g_iso->SetRenderTarget(0, g_rtMS ? g_rtMS : g_rt);
  g_iso->SetDepthStencilSurface(g_depth);
  D3DVIEWPORT9 vp{0, 0, g_capW, g_capH, 0, 1};
  g_iso->SetViewport(&vp);

  // Anisotropic filtering matters more here than it usually would: the camera
  // sits 72 degrees off vertical, so the road runs away from it at a very
  // shallow angle, and that is exactly the case plain trilinear blurs into
  // mush. Take whatever the device offers, up to 16.
  D3DCAPS9 caps{};
  g_iso->GetDeviceCaps(&caps);
  if (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) {
    g_anisotropy = caps.MaxAnisotropy > 16 ? 16 : caps.MaxAnisotropy;
  }
  if (g_anisotropy < 1) g_anisotropy = 1;

  // Whether a fading building can be dissolved without also being bleached.
  // See the draw loop: the colour grade and the transparency both used to
  // come out of TEXTUREFACTOR's alpha, which welded them together. A separate
  // blend factor is what lets them move independently.
  // One cap covers D3DBLEND_BLENDFACTOR and D3DBLEND_INVBLENDFACTOR both.
  g_blendFactor = (caps.SrcBlendCaps  & D3DPBLENDCAPS_BLENDFACTOR) != 0 &&
                  (caps.DestBlendCaps & D3DPBLENDCAPS_BLENDFACTOR) != 0;
  g_depthBias = (caps.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS) != 0 &&
                (caps.RasterCaps & D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS) != 0;
  g_twoStage = caps.MaxTextureBlendStages >= 2;

  logfile::Line("radar3d: isolated D3D9 renderer created - %dx MSAA, %ux aniso,"
                " %s",
                g_samples == D3DMULTISAMPLE_NONE ? 1 : int(g_samples),
                unsigned(g_anisotropy),
                g_twoStage ? "surface colour and baked light in use"
                           : "one blend stage only - texture colour alone");
  return true;
}
// How many bytes are left in the stream from where it is now.
//
// Every count in these files is read out of the file itself, and a count is
// then used to size an allocation. A corrupt or truncated tile can therefore
// ask for hundreds of megabytes, and the allocation that fails does so by
// throwing - inside a Direct3D present hook, with nothing to catch it, which
// ends the process. The tile loader has a perfectly good answer for a bad
// tile (skip it, cache the refusal, carry on), so the counts are checked
// against what the file can actually hold before anything is reserved.
std::streamoff Remaining(std::ifstream &f) {
  const std::streamoff here = f.tellg();
  if (here < 0) return 0;
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  f.seekg(here, std::ios::beg);
  return end > here ? end - here : 0;
}

Tile *LoadTile(int x, int y) {
  auto key = std::make_pair(x, y);
  auto it = g_tiles.find(key);
  if (it != g_tiles.end())
    return it->second;
  const auto retry = g_tileRetry.find(key);
  if (retry != g_tileRetry.end() && GetTickCount()-retry->second < 1000) return nullptr;
  auto rejected = [&](const char* reason) -> Tile* {
    logfile::Line("radar3d: tile %d_%d rejected: %s",x,y,reason);
    return g_tiles[key] = nullptr;
  };
  std::ifstream f(Path(x, y, ".r3g"), std::ios::binary);
  GHeader h{};
  f.read((char *)&h, sizeof h);
  const bool v2 = !memcmp(h.magic, "R3G2", 4);
  // Proper UV-seam clipping in world3d-radar-3dpack.py duplicates vertices
  // for every triangle that crosses a texture-tile boundary, so a dense
  // tile's vertex count is routinely several times what it was before that
  // fix. The old 3,000,000 cap silently dropped the tile (Load returns
  // nullptr with no error surfaced anywhere) rather than failing loudly, so
  // a too-low cap here reads as "tiles randomly missing," not a crash.
  if (!f || (!v2 && memcmp(h.magic, "R3G1", 4)) || !h.nv || !h.ni ||
      h.nv > 10000000 || h.ni > 9000000)
    return rejected("geometry header/counts");
  // The vertices have to actually be in the file. R3G2 counts groups in `ni`
  // rather than indices, and each group carries its own header and index
  // block, so only the vertex block can be checked exactly here - the group
  // blocks are checked as they are read.
  if (std::streamoff(h.nv) * std::streamoff(sizeof(Vertex)) > Remaining(f))
    return rejected("truncated vertex block");
  std::vector<Vertex> v(h.nv);
  f.read((char *)v.data(), v.size() * sizeof(Vertex));
  struct CpuGroup { GroupHeader h; std::vector<uint32_t> i; };
  std::vector<CpuGroup> groups;
  if (v2) {
    if (std::streamoff(h.ni) * std::streamoff(sizeof(GroupHeader) + 12) > Remaining(f))
      return rejected("truncated group table");
    groups.resize(h.ni);
    for (auto &g : groups) {
      f.read((char *)&g.h, sizeof g.h);
      if (!f || !g.h.ni || g.h.ni % 3 || g.h.ni > 9000000 ||
          std::streamoff(g.h.ni) * 4 > Remaining(f))
        return rejected("invalid group indices/count");
      g.i.resize(g.h.ni); f.read((char *)g.i.data(), g.i.size()*4);
    }
  } else {
    if (std::streamoff(h.ni) * 4 > Remaining(f))
      return rejected("truncated legacy indices");
    groups.resize(1); groups[0].h.ni=h.ni; groups[0].i.resize(h.ni);
    f.read((char *)groups[0].i.data(), groups[0].i.size()*4);
  }
  if (!f)
    return rejected("geometry read failed");
  for (const auto &group : groups)
    for (uint32_t index : group.i)
      if (index >= h.nv) return rejected("vertex index out of bounds");
  std::ifstream a(Path(x, y, ".r3a"), std::ios::binary);
  AHeader ah{};
  a.read((char *)&ah, sizeof ah);
  if (!a || memcmp(ah.magic, "R3A1", 4) || !ah.w || !ah.h || ah.w > 2048 ||
      ah.h > 2048 ||
      std::streamoff(ah.w) * std::streamoff(ah.h) * 4 > Remaining(a))
    return rejected("invalid or truncated atlas");
  std::vector<uint32_t> pixels(size_t(ah.w) * ah.h);
  a.read((char *)pixels.data(), pixels.size() * 4);
  if (!a)
    return rejected("atlas read failed");
  auto ownedTile = std::make_unique<Tile>();
  Tile *t = ownedTile.get();
  t->nv = h.nv;
  t->collisionVertices.reserve(v.size());
  for (const auto &vertex : v)
    t->collisionVertices.push_back({vertex.x, vertex.y, vertex.z});
  HRESULT hr =
      g_iso->CreateVertexBuffer(UINT(v.size() * sizeof(Vertex)), 0,
                                D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                                D3DPOOL_MANAGED, &t->vb, nullptr);
  if (SUCCEEDED(hr))
    hr = g_iso->CreateTexture(ah.w, ah.h, 1, 0, D3DFMT_A8R8G8B8,
                              D3DPOOL_MANAGED, &t->tex, nullptr);
  void *p{};
  if (SUCCEEDED(hr))
    hr = t->vb->Lock(0, 0, &p, 0);
  if (SUCCEEDED(hr)) {
    memcpy(p, v.data(), v.size() * sizeof(Vertex));
    t->vb->Unlock();
  }
  for (auto &src : groups) {
    auto ownedGroup = std::make_unique<DrawGroup>();
    auto *g = ownedGroup.get();
    g->ni=src.h.ni; g->flags=src.h.flags;
    g->minx=src.h.minx;g->miny=src.h.miny;g->minz=src.h.minz;
    g->maxx=src.h.maxx;g->maxy=src.h.maxy;g->maxz=src.h.maxz;
    // Every group keeps its indices, ground included. Occlusion only ever
    // consults the object groups, but the camera boom has to collide with
    // terrain and road decks too - being buried in a hillside is exactly the
    // case it exists for.
    g->collisionIndices = src.i;
    if (SUCCEEDED(hr)) hr=g_iso->CreateIndexBuffer(UINT(src.i.size()*4),0,D3DFMT_INDEX32,D3DPOOL_MANAGED,&g->ib,nullptr);
    if (SUCCEEDED(hr)) hr=g->ib->Lock(0,0,&p,0);
    if (SUCCEEDED(hr)) { memcpy(p,src.i.data(),src.i.size()*4);g->ib->Unlock(); }
    t->groups.push_back(g);
    ownedGroup.release();
  }
  D3DLOCKED_RECT lr{};
  if (SUCCEEDED(hr))
    hr = t->tex->LockRect(0, &lr, nullptr, 0);
  if (SUCCEEDED(hr)) {
    for (UINT y0 = 0; y0 < ah.h; y0++)
      memcpy((uint8_t *)lr.pBits + y0 * lr.Pitch,
             pixels.data() + size_t(y0) * ah.w, ah.w * 4);
    t->tex->UnlockRect(0);
  }
  if (FAILED(hr)) {
    logfile::Line("radar3d: tile %d_%d GPU allocation/upload failed: 0x%08lx; retrying",x,y,static_cast<unsigned long>(hr));
    g_tileRetry[key]=GetTickCount();
    return nullptr;
  }
  g_tileRetry.erase(key);
  logfile::Line("radar3d: tile %d_%d loaded: %u vertices, %zu groups",x,y,h.nv,groups.size());
  g_tiles[key] = t;
  return ownedTile.release();
}
Tile *Load(int x, int y) {
  try { return LoadTile(x, y); }
  catch (const std::bad_alloc &) {
    logfile::Line("radar3d: tile %d_%d deferred: insufficient memory", x, y);
    return nullptr;
  }
}
D3DMATRIX Mat() {
  D3DMATRIX m{};
  m._11 = m._22 = m._33 = m._44 = 1;
  return m;
}
// The tiles the phone's map last showed, and when: kept while the map is
// open, so the radar's own trimming (every frame, to what is round CJ) does
// not take them away between two frames of it - a far view loading a tile
// and losing it again, over and over, flickered.
int g_phoneKeep[4] = {1, 0, 1, 0};
ULONGLONG g_phoneKeepAt = 0;

void Retire(int minx, int maxx, int miny, int maxy) {
  const maptiles::Rect radar{minx, maxx, miny, maxy};
  const maptiles::Rect phone{g_phoneKeep[0], g_phoneKeep[1], g_phoneKeep[2], g_phoneKeep[3]};
  const bool phoneActive = g_phoneKeepAt && GetTickCount64() - g_phoneKeepAt <= 1000;
  // Keep the two views, not the rectangle spanning every tile between them.
  const auto keep = [&](const std::pair<int, int>& key) {
    return maptiles::Retains(radar, phone, phoneActive, key.first, key.second);
  };
  for (auto it = g_tileRetry.begin(); it != g_tileRetry.end();) {
    if (!keep(it->first)) it = g_tileRetry.erase(it);
    else ++it;
  }
  for (auto it = g_tiles.begin(); it != g_tiles.end();) {
    if (!keep(it->first)) {
      delete it->second;
      it = g_tiles.erase(it);
    } else ++it;
  }
}
bool SegmentIntersectsBounds(const DrawGroup &g, float ax, float ay, float az,
                             float dx, float dy, float dz) {
  float low=.04f, high=.97f;
  const float origin[3]={ax,ay,az}, direction[3]={dx,dy,dz};
  const float minimum[3]={g.minx,g.miny,g.minz}, maximum[3]={g.maxx,g.maxy,g.maxz};
  for(int axis=0;axis<3;axis++) {
    if(fabsf(direction[axis])<1e-6f) {
      if(origin[axis]<minimum[axis]||origin[axis]>maximum[axis])return false;
      continue;
    }
    float a=(minimum[axis]-origin[axis])/direction[axis];
    float b=(maximum[axis]-origin[axis])/direction[axis];
    if(a>b){const float swap=a;a=b;b=swap;}
    low=a>low?a:low;high=b<high?b:high;
    if(low>high)return false;
  }
  return true;
}
// Returns t in (tMin, tMax) if the segment hits the triangle, else -1.
//
// The default window trims both ends: geometry right at the camera is not what
// is being asked about, and the .97 end keeps the ground the player is
// standing on from counting as something that hides them. The camera boom test
// overrides it, because there the far end is precisely the interesting part.
float TriangleHitT(const Position &a,const Position &b,const Position &c,
                   float ox,float oy,float oz,float dx,float dy,float dz,
                   float tMin=.04f,float tMax=.97f) {
  const float e1x=b.x-a.x,e1y=b.y-a.y,e1z=b.z-a.z;
  const float e2x=c.x-a.x,e2y=c.y-a.y,e2z=c.z-a.z;
  const float px=dy*e2z-dz*e2y,py=dz*e2x-dx*e2z,pz=dx*e2y-dy*e2x;
  const float det=e1x*px+e1y*py+e1z*pz;
  if(fabsf(det)<1e-7f)return -1.f;
  const float inv=1.f/det,tx=ox-a.x,ty=oy-a.y,tz=oz-a.z;
  const float u=(tx*px+ty*py+tz*pz)*inv;
  if(u<0.f||u>1.f)return -1.f;
  const float qx=ty*e1z-tz*e1y,qy=tz*e1x-tx*e1z,qz=tx*e1y-ty*e1x;
  const float v=(dx*qx+dy*qy+dz*qz)*inv;
  if(v<0.f||u+v>1.f)return -1.f;
  const float t=(e2x*qx+e2y*qy+e2z*qz)*inv;
  return (t>tMin&&t<tMax)?t:-1.f;
}
// Returns the nearest t along the segment (origin ax,ay,az → bx,by,bz) at
// which the group's collision geometry is hit, or 2.f if no hit. Lower t means
// closer to the origin, which is the camera; the caller keeps the group with
// the globally smallest t, so only the single nearest blocker fades.
//
// boundsShortcut reports 0.f when the origin is inside the group's bounds -
// the camera is buried in this building - without running the triangle loop.
// A segment beginning and ending inside the same room crosses no triangles at
// all, so without it the wall the camera sits in never fades. It is dependable
// because groups are per-building with tight bounds; see the caller.
float OcclusionHitT(const Tile &tile,const DrawGroup &g,
                    float ax,float ay,float az,
                    float bx,float by,float bz,bool boundsShortcut=true,
                    float tMin=.04f,float tMax=.97f) {
  constexpr float margin=.5f;
  if(boundsShortcut&&
     ax>=g.minx-margin&&ax<=g.maxx+margin&&
     ay>=g.miny-margin&&ay<=g.maxy+margin&&
     az>=g.minz-margin&&az<=g.maxz+margin)return 0.f;
  const float dx=bx-ax,dy=by-ay,dz=bz-az;
  if(!SegmentIntersectsBounds(g,ax,ay,az,dx,dy,dz))return 2.f;
  const auto &indices=g.collisionIndices;
  float nearest=2.f;
  for(size_t i=0;i+2<indices.size();i+=3) {
    const uint32_t ia=indices[i],ib=indices[i+1],ic=indices[i+2];
    if(ia>=tile.collisionVertices.size()||ib>=tile.collisionVertices.size()||
       ic>=tile.collisionVertices.size())continue;
    const float t=TriangleHitT(tile.collisionVertices[ia],tile.collisionVertices[ib],
                                tile.collisionVertices[ic],ax,ay,az,dx,dy,dz,
                                tMin,tMax);
    if(t>=0.f&&t<nearest)nearest=t;
  }
  return nearest;
}
// The world's two blend stages.
//
// Stage one puts the surface's own colour onto its texture; stage two blends
// that toward the horizon for the navigation grade. Both passes over the world
// use this, so they cannot drift apart.
//
// There used to be only the second of those. The vertex colour was uploaded on
// every vertex - the FVF has always declared D3DFVF_DIFFUSE - and then never
// referenced by any stage, so two things were quietly thrown away every frame:
// the baked sunlight and ambient occlusion, which is why the world looked flat
// and evenly lit; and the material's own colour, which is what a surface with
// no texture is supposed to be drawn in. world3d-atlas reserves a plain white
// cell for materials whose texture could not be placed, precisely because the
// colour was expected to carry them - so instead of a coloured surface those
// came out pure white, and no amount of correct texture data would have fixed
// it. Terrain is the visible case, being a few very large triangles.
void SetWorldStages() {
  if (g_twoStage) {
    g_iso->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_iso->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_iso->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_iso->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_iso->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    // No texture is bound or sampled here: the arguments are the running
    // result and the constant, so this stage only mixes what stage one made.
    g_iso->SetTexture(1, nullptr);
    g_iso->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_BLENDFACTORALPHA);
    g_iso->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_iso->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_iso->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_iso->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    g_iso->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_iso->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    return;
  }
  // One stage only: the grade has to be the thing that survives, because
  // without it every distance reads the same and the panel stops being a map.
  g_iso->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDFACTORALPHA);
  g_iso->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_iso->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  g_iso->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  g_iso->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

// Everything that is not the world draws through stage one alone. Device state
// persists between frames as well as between draws, so the second stage has to
// be put away or the water, the ribbon and the destination marker are all
// silently blended toward the horizon colour as well.
void OneStage() {
  g_iso->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  g_iso->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

bool DrawWaterPlane(float ox,float oy,float extent) {
  struct WaterVertex { float x,y,z; uint32_t colour; };
  constexpr uint32_t colour=0xffaecbd1;
  static const auto patches = [] {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr,path,MAX_PATH);
    std::string root(path);
    root.resize(root.find_last_of("\\/")+1);
    std::ifstream input(root+"data\\water.dat");
    return radar_water::Read(input);
  }();
  const auto mesh=radar_water::Mesh(patches,ox,oy,extent);
  if (mesh.empty()) return false;
  OneStage();
  g_iso->SetTexture(0,nullptr);
  g_iso->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE);
  g_iso->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
  g_iso->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
  g_iso->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
  g_iso->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
  g_iso->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
  g_iso->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);

  std::vector<WaterVertex> vertices;
  vertices.reserve(mesh.size());
  for (const auto& point : mesh) vertices.push_back({point.x,point.y,point.z,colour});
  D3DCAPS9 caps{};
  g_iso->GetDeviceCaps(&caps);
  const size_t budget=caps.MaxPrimitiveCount ? caps.MaxPrimitiveCount : 65535;
  bool drew=false;
  for (size_t start=0; start<vertices.size();) {
    const UINT count=static_cast<UINT>(std::min(budget,(vertices.size()-start)/3));
    if (SUCCEEDED(g_iso->DrawPrimitiveUP(D3DPT_TRIANGLELIST,count,vertices.data()+start,sizeof(WaterVertex)))) drew=true;
    start+=size_t(count)*3;
  }
  g_iso->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1);
  g_iso->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_BLENDFACTORALPHA);
  g_iso->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
  g_iso->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_TFACTOR);
  return drew;
}
// Turn the route the server sent into the two triangle strips that draw it.
//
// `raw` is x/y/z triples in travel order, already offset onto the correct side
// of the road by the server, which is the side that knows each road's real
// width and lane count. Nothing here second-guesses that placement.
//
// Called on the network thread when a route arrives, never per frame.
std::vector<float> ResampleRoute(const std::vector<float> &points, float spacing = .5f) {
  if (points.size() < 6) return points;
  std::vector<float> out(points.begin(), points.begin() + 3);
  float remaining = spacing;
  for (size_t i = 3; i + 2 < points.size(); i += 3) {
    const float *a = &points[i - 3], *b = &points[i];
    const float dx = b[0]-a[0], dy = b[1]-a[1];
    const float length = sqrtf(dx*dx + dy*dy);
    if (length < 1e-5f) continue;
    float consumed = 0.f;
    while (length - consumed >= remaining) {
      consumed += remaining;
      const float t = consumed / length;
      for (int k = 0; k < 3; ++k) out.push_back(a[k] + (b[k]-a[k])*t);
      remaining = spacing;
    }
    remaining -= length - consumed;
  }
  const size_t end = points.size()-3, last = out.size()-3;
  const float dx = out[last]-points[end], dy = out[last+1]-points[end+1];
  if (dx*dx + dy*dy > .01f*.01f)
    out.insert(out.end(), points.begin()+end, points.end());
  else
    for (int k = 0; k < 3; ++k) out[last+k] = points[end+k];
  return out;
}

std::shared_ptr<const RouteGeometry> BuildRouteGeometry(
    const std::vector<float> &raw, float destX, float destY, DestKind kind) {
  if (raw.size() < 6) return nullptr;

  // Measured on the raw road nodes, before any smoothing moves them. The slack
  // is generous because the route ends on a road node while the destination
  // can be a door, a car park or a field.
  constexpr float kRouteEndSlack = 80.f;
  const float endDx = raw[raw.size() - 3] - destX;
  const float endDy = raw[raw.size() - 2] - destY;
  const bool reaches =
      endDx * endDx + endDy * endDy <= kRouteEndSlack * kRouteEndSlack;

  std::vector<float> pts = raw;

  // Cut the corners before rounding them.
  //
  // The road graph gives the centre of the lane at every node, so a junction
  // arrives as a genuine right angle and the path runs all the way out to the
  // corner before turning. Chaikin rounds that off, but rounding a corner is
  // not the same as cutting one: the line still bulges out to where the two
  // roads actually meet, which is why the turns looked wider than they need
  // to be. No car drives that. A car cuts toward the inside of the bend and
  // rejoins the lane on the way out.
  //
  // Each interior point is pulled toward the straight line between its two
  // neighbours, by an amount that grows with how sharp the turn is. A
  // straight run has neighbours in line with it and does not move at all, so
  // this only ever touches actual corners. Two passes, because one leaves
  // right angles still reading as corners and three starts visibly clipping
  // the route off the road.
  for (int iter = 0; iter < 2; iter++) {
    const size_t m = pts.size() / 3;
    if (m < 3) break;
    std::vector<float> cut = pts;
    for (size_t i = 1; i + 1 < m; i++) {
      const float *prev = &pts[(i - 1) * 3];
      const float *here = &pts[i * 3];
      const float *next = &pts[(i + 1) * 3];
      const float d1x = here[0] - prev[0], d1y = here[1] - prev[1];
      const float d2x = next[0] - here[0], d2y = next[1] - here[1];
      const float l1 = sqrtf(d1x * d1x + d1y * d1y);
      const float l2 = sqrtf(d2x * d2x + d2y * d2y);
      if (l1 < 0.01f || l2 < 0.01f) continue;
      // 1 when the two segments are in line, -1 when the path doubles back.
      const float dot = (d1x * d2x + d1y * d2y) / (l1 * l2);
      // Nothing until the turn is worth calling a turn, then rising with it.
      float sharp = (1.f - dot) * 0.5f;   // 0 straight .. 1 reversal
      if (sharp < 0.02f) continue;
      if (sharp > 1.f) sharp = 1.f;
      const float amount = kCornerCut * sharp;
      for (int k = 0; k < 3; k++) {
        const float mid = (prev[k] + next[k]) * 0.5f;
        cut[i * 3 + k] = here[k] + (mid - here[k]) * amount;
      }
    }
    pts = std::move(cut);
  }

  // Smooth the corners with Chaikin's corner-cutting algorithm. Each pass
  // replaces every segment [A,B] with points at 0.75A+0.25B and 0.25A+0.75B,
  // keeping the two ends where they are. Three passes turn the road graph's
  // hard corners into arcs. Height is carried through the same interpolation,
  // so the ribbon stays on the road surface over a hill or a slip road.
  for (int iter = 0; iter < 6; iter++) {
    const size_t m = pts.size() / 3;
    if (m < 3) break;
    std::vector<float> s;
    s.reserve((m * 2) * 3);
    s.push_back(pts[0]); s.push_back(pts[1]); s.push_back(pts[2]);
    for (size_t i = 0; i + 1 < m; i++) {
      const float *a = &pts[i * 3], *b = &pts[(i + 1) * 3];
      for (int k = 0; k < 3; k++) s.push_back(0.75f * a[k] + 0.25f * b[k]);
      for (int k = 0; k < 3; k++) s.push_back(0.25f * a[k] + 0.75f * b[k]);
    }
    s.push_back(pts[(m - 1) * 3]);
    s.push_back(pts[(m - 1) * 3 + 1]);
    s.push_back(pts[(m - 1) * 3 + 2]);
    pts = std::move(s);
  }

  // Six subdivision passes create sub-millimetre segments at large world
  // coordinates. Keep the curve, but draw it with stable half-metre spacing.
  pts = ResampleRoute(pts);
  const size_t n = pts.size() / 3;
  if (n < 2) return nullptr;

  // Distance along the route, for the fade.
  float totalLen = 0.f;
  for (size_t i = 0; i + 1 < n; i++) {
    const float dx = pts[(i + 1) * 3] - pts[i * 3];
    const float dy = pts[(i + 1) * 3 + 1] - pts[i * 3 + 1];
    totalLen += sqrtf(dx * dx + dy * dy);
  }
  if (totalLen < 0.01f) return nullptr;

  // Half-widths in world units, and the lift off the road surface. The lift
  // has to clear the road mesh without floating: the tiles are the exported
  // world, so a few centimetres is enough to win the depth test on flat ground.
  constexpr float kLift = 0.25f;
  // Outline slightly wider than the fill, so what shows around the edge is a
  // thin border rather than a second band: at 0.72 against 0.60 the outline
  // reads as roughly a tenth of a unit on each side.
  //
  // Colours in ARGB. The fill is the same hot pink as the destination dot -
  // deliberately the same constant's value, so the line and the place it
  // leads to are plainly one thing - and the outline is white, which separates
  // the ribbon from dark road without tinting it the way the old deep pink
  // border did.
  const radarcfg::Settings &cfg = radarcfg::Current();
  const float widths[2] = {cfg.ribbonOutlineWidth, cfg.ribbonWidth};
  uint32_t fill = kind == DestKind::Mission ? cfg.missionRibbon : cfg.gpsRibbon;
  if (kind == DestKind::Mission) {
    if (const uint32_t seen = g_objectiveColour.load())
      fill = seen;  // the marker's own colour wins over the configured one
  }
  const uint32_t colors[2] = {cfg.ribbonOutline, fill};

  auto geom = std::make_shared<RouteGeometry>();
  geom->kind = kind;
  geom->settings = radarcfg::Generation();
  geom->totalArc = totalLen;
  geom->endsAtDest = reaches;
  geom->endGap = sqrtf(endDx * endDx + endDy * endDy);
  {
    static uint32_t s_nextRouteId = 1;
    geom->id = s_nextRouteId++;
  }

  for (int pass = 0; pass < 2; pass++) {
    const float    w        = widths[pass];
    const uint32_t base_col = colors[pass];
    std::vector<RouteVert> &strip = pass == 0 ? geom->outline : geom->fill;
    strip.reserve(n * 2);

    // Width is horizontal, so the miter is worked out in XY and the height is
    // simply carried across the ribbon.
    auto perp = [](float dx, float dy, float len, float &px, float &py) {
      px = -dy / len; py = dx / len;
    };

    float walked = 0.f;
    for (size_t i = 0; i < n; i++) {
      float mx, my;
      if (i == 0 || i == n - 1) {
        // An end has only one segment to be perpendicular to.
        const size_t a = (i == 0) ? 0 : i - 1;
        const size_t b = (i == 0) ? 1 : i;
        const float dx = pts[b * 3] - pts[a * 3];
        const float dy = pts[b * 3 + 1] - pts[a * 3 + 1];
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) { mx = 0.f; my = w; }
        else { perp(dx, dy, len, mx, my); mx *= w; my *= w; }
      } else {
        const float d1x = pts[i * 3] - pts[(i - 1) * 3];
        const float d1y = pts[i * 3 + 1] - pts[(i - 1) * 3 + 1];
        const float d2x = pts[(i + 1) * 3] - pts[i * 3];
        const float d2y = pts[(i + 1) * 3 + 1] - pts[i * 3 + 1];
        const float l1 = sqrtf(d1x * d1x + d1y * d1y);
        const float l2 = sqrtf(d2x * d2x + d2y * d2y);
        if (l1 < 0.001f || l2 < 0.001f) {
          const float sx = d1x + d2x, sy = d1y + d2y;
          perp(sx, sy, sqrtf(sx * sx + sy * sy) + 0.001f, mx, my);
          mx *= w; my *= w;
        } else {
          float p1x, p1y, p2x, p2y;
          perp(d1x, d1y, l1, p1x, p1y);
          perp(d2x, d2y, l2, p2x, p2y);
          // The bisector of the two perpendiculars, lengthened by 1/cos of the
          // half angle so the ribbon keeps its width around the corner instead
          // of pinching. Clamped so a hairpin cannot throw out a long spike.
          const float bix = p1x + p2x, biy = p1y + p2y;
          const float blen = sqrtf(bix * bix + biy * biy);
          if (blen < 0.001f) { mx = p1x * w; my = p1y * w; }
          else {
            const float mdx = bix / blen, mdy = biy / blen;
            const float dot = mdx * p1x + mdy * p1y;
            const float scale =
                w / (fabsf(dot) > 0.333f ? dot : copysignf(0.333f, dot));
            mx = mdx * scale; my = mdy * scale;
          }
        }
      }

      // One opacity the whole way along.
      //
      // This used to ramp alpha with walked/totalLen, which was wrong twice
      // over. The ribbon read as translucent along its entire length instead
      // of being a solid line on the map, and because the ramp was a fraction
      // of the total, every reroute changed totalLen and so restated the
      // alpha of every vertex - the ribbon visibly flickered as it was
      // driven. The only fade the reference has is where the ribbon comes out
      // from under the player arrow, and that one is applied at draw time in
      // DrawGpsRoute, against distance from the player rather than a fraction
      // of the trip.
      const uint32_t col = base_col;
      const float px = pts[i * 3], py = pts[i * 3 + 1];
      const float pz = pts[i * 3 + 2] + kLift;

      strip.push_back({px + mx, py + my, pz, col});
      strip.push_back({px - mx, py - my, pz, col});
      // walked is the distance along the route to this point: it is added to
      // at the end of the iteration, so here it still refers to point i.
      if (pass == 0) {
        geom->centres.push_back(px);
        geom->centres.push_back(py);
        geom->arc.push_back(walked);
      }

      if (i + 1 < n) {
        const float dx = pts[(i + 1) * 3] - px;
        const float dy = pts[(i + 1) * 3 + 1] - py;
        walked += sqrtf(dx * dx + dy * dy);
      }
    }
  }
  return geom;
}

// ---------------------------------------------------------------------------
// Map matching: where on the route the player currently is.
//
// Adapted from Newson and Krumm, "Hidden Markov Map Matching Through Noise and
// Sparseness" (ACM SIGSPATIAL 2009). That paper solves the hard version of the
// problem - noisy GPS at one sample per second, matched offline against a
// whole road network - with a Hidden Markov Model decoded by Viterbi. Two
// differences make nearly all of that unnecessary here:
//
//   - Our position is exact. We read the player's world coordinates out of the
//     game, so the measurement noise their emission probability exists to
//     model is simply zero.
//   - We sample every frame rather than once a second, and we match against
//     one known route rather than searching a network of them.
//
// What carries over is the point of the paper, and it is the part that
// matters: never match on distance alone. Matching each position to whatever
// is nearest is, in their words, prone to mistakes, because it ignores
// whether the vehicle could actually have got there. They enforce that with
// transition probabilities over route distance, and they note that a sliding
// window form of the same idea is what suits real-time in-vehicle navigation.
// With exact positions that window collapses to one hard constraint:
//
//     the new match must be within the distance actually travelled of the old
//     one, measured along the route.
//
// So we keep an arc-length position between frames and only ever search a
// window around it. That is both more correct than the global nearest-segment
// scan it replaces - it cannot jump to a parallel carriageway, to an overpass
// above or below, or back onto the part already driven - and cheaper, because
// it examines a handful of segments instead of every one.
//
// Two further details are taken from the paper. Section 3.1 drops any road
// beyond a fixed radius from consideration entirely; ours is far tighter than
// their 200 m because our positions carry no error, and past it we are not on
// this route at all. Section 4.1 ignores movements too small to be real;
// theirs is twice the GPS sigma, ours only has to clear float jitter.
constexpr float kOffRouteDist = 30.f; // cross-track distance meaning "not on it"
constexpr float kMatchBack    = 6.f;  // window behind, so reversing still matches
constexpr float kMatchSlack   = 12.f; // window ahead of the distance travelled
constexpr float kFirstMatch   = 60.f; // a fresh route begins at the player

struct MatchState {
  uint32_t routeId = 0;
  bool  valid = false;
  float s = 0.f;              // arc length along the route
  float px = 0.f, py = 0.f;   // the matched point itself
  float tx = 0.f, ty = 1.f;   // unit tangent there
  float cross = 0.f;          // cross-track distance to the player
  float lastX = 0.f, lastY = 0.f;
};
MatchState g_match;

// lower_bound over the ascending arc-length table.
size_t ArcIndex(const std::vector<float> &arc, float v) {
  size_t lo = 0, hi = arc.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (arc[mid] < v) lo = mid + 1; else hi = mid;
  }
  return lo;
}

bool MapMatch(const RouteGeometry &g, float x, float y) {
  const std::vector<float> &c = g.centres;
  const size_t nc = c.size() / 2;
  if (nc < 2 || g.arc.size() != nc) return false;

  // A route we have not seen before restarts the search at its beginning.
  const bool fresh = !g_match.valid || g_match.routeId != g.id;
  if (fresh) {
    g_match.routeId = g.id;
    g_match.valid   = false;
    g_match.s       = 0.f;
    g_match.lastX   = x;
    g_match.lastY   = y;
  }

  // The window. A fresh route is generated from the player's own position, so
  // arc zero is already under them and only its opening needs searching.
  float lo, hi;
  if (fresh) {
    lo = 0.f;
    hi = kFirstMatch;
  } else {
    const float dx = x - g_match.lastX, dy = y - g_match.lastY;
    const float moved = sqrtf(dx * dx + dy * dy);
    lo = g_match.s - kMatchBack;
    hi = g_match.s + moved + kMatchSlack;
  }
  if (lo < 0.f) lo = 0.f;
  if (hi > g.totalArc) hi = g.totalArc;

  size_t i0 = ArcIndex(g.arc, lo);
  size_t i1 = ArcIndex(g.arc, hi);
  if (i0 > 0) --i0;              // include the segment straddling the near edge
  if (i1 + 1 > nc - 1) i1 = nc - 1;
  if (i1 <= i0) i1 = (i0 + 1 < nc) ? i0 + 1 : nc - 1;

  float bestD2 = 3.4e38f, bestS = 0.f, bx = x, by = y, btx = 0.f, bty = 1.f;
  bool found = false;

  auto scan = [&](size_t from, size_t to) {
    for (size_t i = from; i < to && i + 1 < nc; ++i) {
      const float ax = c[i * 2],       ay = c[i * 2 + 1];
      const float ex = c[(i + 1) * 2], ey = c[(i + 1) * 2 + 1];
      const float sx = ex - ax, sy = ey - ay;
      const float len2 = sx * sx + sy * sy;
      if (len2 < 1e-6f) continue;
      float t = ((x - ax) * sx + (y - ay) * sy) / len2;
      if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
      const float qx = ax + t * sx, qy = ay + t * sy;
      const float dx = x - qx, dy = y - qy;
      const float d2 = dx * dx + dy * dy;
      if (d2 < bestD2) {
        bestD2 = d2;
        const float len = sqrtf(len2);
        bestS = g.arc[i] + t * len;
        bx = qx; by = qy;
        btx = sx / len; bty = sy / len;
        found = true;
      }
    }
  };
  scan(i0, i1);
  // A route the matcher has never seen gets one more chance, over the whole of
  // it, before it is declared unmatchable. Failing here leaves g_match.valid
  // false, and DrawGpsRoute draws nothing without a match - so a route whose
  // opening happens not to be under the player was a ribbon that vanished for
  // a frame rather than one drawn from slightly the wrong place.
  if (!found && fresh) scan(0, nc - 1);
  if (!found) return false;

  g_match.s     = bestS;
  g_match.px    = bx;  g_match.py = by;
  g_match.tx    = btx; g_match.ty = bty;
  g_match.cross = sqrtf(bestD2);
  g_match.lastX = x;   g_match.lastY = y;
  g_match.valid = true;
  return true;
}

// Draw the GPS route ribbon. Everything expensive already happened in
// BuildRouteGeometry when the route arrived, so this is two draw calls.
// `geom` is the caller's snapshot, taken once for the frame and shared with
// the match. Taking a second one here was a bug: the router replaces the route
// from a worker thread, and a swap landing between the two snapshots left this
// holding a route the matcher had never seen, whose id therefore did not match
// and which was silently not drawn. That was a one-frame hole in the ribbon
// every time a route was rebuilt.
void DrawGpsRoute(const std::shared_ptr<const RouteGeometry> &geom, float ox,
                  float oy, float oz, float heading) {
  g_routeOnScreen = false;
  if (!geom || geom->outline.size() < 4) return;

  // The match ran at the top of the frame, before the camera was placed,
  // because the camera is placed on its result. If it failed there is no
  // position on the route to draw from.
  const std::vector<float> &centres = geom->centres;
  const size_t nc = centres.size() / 2;
  if (nc < 2) return;
  if (!g_match.valid || g_match.routeId != geom->id) return;

  // The ribbon is drawn from the matched point onward. That point is on the
  // route by construction, so there is no gap to bridge and nothing to smooth:
  // the lead-in below is a continuation of the ribbon rather than a join to
  // it. Every version of the hook came from trying to bridge a gap that
  // matching properly removes instead.
  size_t next = ArcIndex(geom->arc, g_match.s);
  if (next >= nc) next = nc - 1;
  const size_t skip = next * 2;

  OneStage();
  g_iso->SetTexture(0, nullptr);
  g_iso->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  g_iso->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_iso->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  g_iso->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_iso->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_iso->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_iso->SetRenderState(D3DRS_FOGENABLE, TRUE);
  g_iso->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  g_iso->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

  // Pull the ribbon toward the camera in depth rather than lifting it higher.
  //
  // The route's heights come from the road graph and the ground comes from the
  // exported mesh, and the two do not agree everywhere: over a camber, a kerb,
  // a slip road or a bridge deck the tarmac can sit above the line by more
  // than the quarter unit of lift, and the ribbon then sinks into the ground
  // and surfaces again further along. Raising kLift far enough to cover the
  // worst of that would leave the ribbon visibly floating everywhere else.
  //
  // This was previously hidden rather than solved: the old propagation loop
  // faded ground groups, and a faded group did not write depth, so the road
  // frequently was not in the depth buffer to occlude anything. Now that the
  // ground is drawn properly the ribbon has to earn its place against it.
  //
  // A depth bias is what that case is for. The ribbon keeps its world
  // position and only its depth is nudged forward, by an amount that grows
  // with how steeply the surface recedes - and this camera looks down a road
  // running away at a very shallow angle, which is the slope-scaled term's
  // whole purpose.
  struct ResetRouteDepthBias {
    ~ResetRouteDepthBias() {
      if (g_depthBias) {
        g_iso->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
        g_iso->SetRenderState(D3DRS_DEPTHBIAS, 0);
      }
    }
  } resetRouteDepthBias;
  if (g_depthBias) {
    const float slopeBias = -2.f, constBias = -1e-5f;
    g_iso->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,
                          *reinterpret_cast<const DWORD *>(&slopeBias));
    g_iso->SetRenderState(D3DRS_DEPTHBIAS,
                          *reinterpret_cast<const DWORD *>(&constBias));
  }

  // Bend the ribbon onto the car over the first stretch of road.
  //
  // The route runs down the middle of its lane, which is not where the car is
  // - a lane over, or off the centre line, or both. Left alone the ribbon
  // runs alongside the arrow rather than under it. Moving the camera onto the
  // route cures that and is far worse: the arrow is fixed in the panel and
  // the world moves beneath it, so correcting the position throws the whole
  // world sideways.
  //
  // So the ribbon is bent instead of the world. Every vertex within kTaper of
  // the car is displaced by the car's own cross-track error, faded out with
  // distance, so the ribbon begins exactly under the arrow and has eased back
  // into its proper lane by the end of the taper. Nothing else moves, and
  // past the taper the cached geometry is drawn exactly as it was built.
  // How far ahead the bend takes to resolve.
  //
  // At 22 this was correct but read as a kink: the whole difference between
  // where the car is and where the lane is got taken up within a car's length
  // or two, so the ribbon visibly turned to meet the road instead of running
  // into it. Spread over 60 the same correction becomes a sweep - the ribbon
  // leaves the car pointing roughly where the car points and is back in its
  // lane by the time it is far enough away to read as "the road ahead".
  //
  // Raising this does not push the ribbon further off the road: the amount of
  // displacement is unchanged and still fades to nothing: only the distance it
  // is spread over grows. Too far and the ribbon stops tracking real bends in
  // the road, because the taper flattens them along with the error.
  constexpr float kTaper    = 60.f;
  // Ceiling on that, for a car a very long way off the route. Past this the
  // curve would take over the whole visible ribbon.
  constexpr float kTaperMax = 220.f;
  static const float kConnWidths[2] = {0.66f, 0.55f};
  // How far the ribbon takes to reach full opacity as it comes out from under
  // the player arrow. This is the only place the ribbon is anything but solid.
  // Kept well inside kTaper so the fade has finished by the time the bent lead
  // in rejoins the baked ribbon, and the two meet at the same opacity.
  constexpr float kFadeIn = 20.f;

  // Curve the ribbon back to the road, rather than dragging the road at it.
  //
  // The old lead-in translated the route's own nodes toward the car and faded
  // that displacement out over kTaper. It was fine while the car was a lane
  // off and fell apart the moment it was not: the first segment jumped
  // straight from the car to a node that could be behind it, so the ribbon
  // arrived at a hard V, and with the car pointing back down the route the
  // join folded through nearly 180 degrees and threw a spike across the panel.
  // Off the tarmac altogether it produced the kink that prompted this.
  //
  // What is wanted is a line that leaves the car pointing where the car is
  // pointing and arrives on the road pointing along the road. That is exactly
  // a cubic Hermite: two endpoints and a tangent at each. It is smooth by
  // construction at both ends, so there is no crease to smooth away
  // afterwards, no fold to blow the mitre up, and being off-road is no longer
  // a special case - it is just a longer curve.
  constexpr float kLift = 0.25f;

  // Smooth the correction, not the road.
  //
  // The first version of this drew one Hermite from the car to a point sixty
  // units along the route. It left the car cleanly and arrived on the road
  // cleanly, and in between it was a synthetic curve that knew nothing about
  // the road - so a corner inside the taper was simply erased and replaced by
  // a lazy arc that cut the bend. The route's own smoothing is what should
  // decide how a corner looks, and it already does that in BuildRouteGeometry.
  //
  // So the curve is applied to the offset instead. The path is the route's own
  // geometry, sampled densely, plus a displacement that starts as the vector
  // from the matched point to the car and decays to nothing by the rejoin:
  //
  //     path(u) = route(u) + D(u),  D(0) = car - matched,  D(taper) = 0
  //
  // Every corner the road makes inside the taper survives untouched, because
  // route(u) carries it and D only slides the result sideways by an amount
  // that is already shrinking. D is a cubic Hermite so that its derivative can
  // be set at both ends: zero at the rejoin, so the ribbon merges into the
  // baked strip with no kink at all, and at the car whatever makes the ribbon
  // leave along the car's own heading.
  const float offDx = ox - g_match.px, offDy = oy - g_match.py;
  const float offDist = sqrtf(offDx * offDx + offDy * offDy);
  float wantTaper = kTaper;
  if (offDist * 1.5f > wantTaper) wantTaper = offDist * 1.5f;
  if (wantTaper > kTaperMax) wantTaper = kTaperMax;

  // Rejoin on a node, and take the taper length from that node, so the last
  // sample lands exactly on the baked ribbon's own vertex pair.
  size_t tapEnd = next;
  while (tapEnd + 1 < nc && geom->arc[tapEnd] < g_match.s + wantTaper) ++tapEnd;
  while (tapEnd > next && tapEnd * 2 + 1 >= geom->outline.size()) --tapEnd;
  const float taperLen = geom->arc[tapEnd] - g_match.s;
  if (taperLen < 1.f) return; // nothing left of the route to lead into

  // route(u): position along the route at an absolute arc length.
  auto routeAt = [&](float sArc, float &rx, float &ry, float &rz) {
    size_t i = ArcIndex(geom->arc, sArc);
    if (i >= nc) i = nc - 1;
    if (i == 0) {
      rx = centres[0]; ry = centres[1]; rz = geom->outline[0].z; return;
    }
    const float a0 = geom->arc[i - 1], a1 = geom->arc[i];
    float f = (a1 - a0) > 1e-5f ? (sArc - a0) / (a1 - a0) : 0.f;
    if (f < 0.f) f = 0.f; else if (f > 1.f) f = 1.f;
    rx = centres[(i - 1) * 2] +
         (centres[i * 2] - centres[(i - 1) * 2]) * f;
    ry = centres[(i - 1) * 2 + 1] +
         (centres[i * 2 + 1] - centres[(i - 1) * 2 + 1]) * f;
    rz = geom->outline[(i - 1) * 2].z +
         (geom->outline[i * 2].z - geom->outline[(i - 1) * 2].z) * f;
  };

  float r0x, r0y, r0z;
  routeAt(g_match.s, r0x, r0y, r0z);
  const float d0X = ox - r0x, d0Y = oy - r0y, d0Z = (oz + kLift) - r0z;

  // The offset's derivative at the car. Adding it to the route's own tangent
  // has to give the car's heading, so it is the difference of the two, scaled
  // into the curve's parameter. GTA's heading has forward at (-sin, cos); see
  // VehicleHeading, which is where this angle comes from.
  const float carX = -sinf(heading), carY = cosf(heading);
  float m0x = (carX - g_match.tx) * taperLen;
  float m0y = (carY - g_match.ty) * taperLen;
  {
    // Bounded, because a car pointing back down the route makes this term
    // twice the taper long and the ribbon would loop out that far to honour
    // it. Clamped, the departure is still along the car's heading, it simply
    // gives that up sooner.
    const float mlen = sqrtf(m0x * m0x + m0y * m0y);
    const float mcap = .55f * taperLen;
    if (mlen > mcap && mlen > 1e-4f) {
      m0x *= mcap / mlen; m0y *= mcap / mlen;
    }
  }

  constexpr int kLeadSteps = 40;
  std::vector<float> line;
  line.reserve((kLeadSteps + 1) * 3);
  for (int i = 0; i <= kLeadSteps; i++) {
    const float t = float(i) / float(kLeadSteps);
    const float t2 = t * t, t3 = t2 * t;
    // Hermite basis for a value that ends at zero with zero slope, so only
    // the two start terms survive.
    const float h00 = 2.f * t3 - 3.f * t2 + 1.f;
    const float h10 = t3 - 2.f * t2 + t;
    float rx, ry, rz;
    routeAt(g_match.s + t * taperLen, rx, ry, rz);
    line.push_back(rx + h00 * d0X + h10 * m0x);
    line.push_back(ry + h00 * d0Y + h10 * m0y);
    line.push_back(rz + kLift + h00 * d0Z);
  }

  const size_t lp = line.size() / 3;

  // Width has to be taken across the bent line's own direction, not the
  // route's. Translating the cached vertex pairs and keeping the
  // perpendiculars they were built with is what made the ribbon go thin:
  // where the fade changes fastest the bent line runs at an angle to the
  // route, and measuring its width across the route's direction foreshortens
  // it. This is the same mitre BuildRouteGeometry uses, applied to the bent
  // line, so the ribbon keeps its width around the bend.
  auto mitre = [&](size_t j, float w, float &mx, float &my) {
    float d1x = 0.f, d1y = 0.f, d2x = 0.f, d2y = 0.f;
    if (j > 0) {
      d1x = line[j * 3] - line[(j - 1) * 3];
      d1y = line[j * 3 + 1] - line[(j - 1) * 3 + 1];
    }
    if (j + 1 < lp) {
      d2x = line[(j + 1) * 3] - line[j * 3];
      d2y = line[(j + 1) * 3 + 1] - line[j * 3 + 1];
    }
    if (j == 0)        { d1x = d2x; d1y = d2y; }
    if (j + 1 == lp)   { d2x = d1x; d2y = d1y; }
    const float l1 = sqrtf(d1x * d1x + d1y * d1y);
    const float l2 = sqrtf(d2x * d2x + d2y * d2y);
    if (l1 < 1e-4f || l2 < 1e-4f) {
      const float sx = d1x + d2x, sy = d1y + d2y;
      const float sl = sqrtf(sx * sx + sy * sy);
      if (sl < 1e-4f) { mx = 0.f; my = w; return; }
      mx = -sy / sl * w; my = sx / sl * w; return;
    }
    const float p1x = -d1y / l1, p1y = d1x / l1;
    const float p2x = -d2y / l2, p2y = d2x / l2;
    const float bx = p1x + p2x, by = p1y + p2y;
    const float bl = sqrtf(bx * bx + by * by);
    if (bl < 1e-3f) { mx = p2x * w; my = p2y * w; return; }
    const float mdx = bx / bl, mdy = by / bl;
    const float dot = mdx * p1x + mdy * p1y;
    // Mitre limit. The old clamp let the joint run out to three times the
    // ribbon's width before it gave up, and near a fold the bisector has no
    // dependable direction at all - it swings about and changes sign between
    // frames, which is what threw the wide flickering spike at the player when
    // the route doubled back. Past about a 120 degree turn, square the joint
    // off across the outgoing segment instead: a little overlap on the inside
    // of a tight bend cannot be seen, and a spike very much can.
    if (dot < .5f) { mx = p2x * w; my = p2y * w; return; }
    const float scale = w / dot;
    mx = mdx * scale; my = mdy * scale;
  };

  if (lp >= 2) {
    for (int pass = 0; pass < 2; pass++) {
      const std::vector<RouteVert> &strip =
          pass == 0 ? geom->outline : geom->fill;
      if (tapEnd * 2 + 1 >= strip.size()) continue;
      const float w = kConnWidths[pass];
      const uint32_t col = strip[skip].colour;

      // The one fade the ribbon has: it comes out from under the arrow rather
      // than starting abruptly at it. Measured in world units along the bent
      // line from the car, so it stays put as the route is driven - the old
      // version scaled the whole ribbon by a fraction of the total trip, which
      // is what made it flicker on every reroute.
      auto fadeAt = [&](float along) -> uint32_t {
        float t = along / kFadeIn;
        if (t >= 1.f) return col;
        if (t < 0.f) t = 0.f;
        const float smooth = t * t * (3.f - 2.f * t);
        const uint32_t a =
            uint32_t(float((col >> 24) & 0xFFu) * smooth + .5f);
        return (a << 24) | (col & 0x00FFFFFFu);
      };

      std::vector<RouteVert> band;
      band.reserve(lp * 2);
      float along = 0.f;
      for (size_t j = 0; j < lp; ++j) {
        // Finish on the untouched ribbon's own vertices, so the two meet
        // exactly and there is no seam to see. By here the fade is long since
        // complete (kFadeIn is well inside kTaper), so these carry the baked
        // colour unaltered and the join is invisible.
        if (j + 1 == lp) {
          band.push_back(strip[tapEnd * 2]);
          band.push_back(strip[tapEnd * 2 + 1]);
          break;
        }
        if (j > 0) {
          const float dx = line[j * 3]     - line[(j - 1) * 3];
          const float dy = line[j * 3 + 1] - line[(j - 1) * 3 + 1];
          along += sqrtf(dx * dx + dy * dy);
        }
        const float bxp = line[j * 3], byp = line[j * 3 + 1];
        const float bzp = line[j * 3 + 2];
        float mx, my;
        mitre(j, w, mx, my);
        const uint32_t c = fadeAt(along);
        band.push_back({bxp + mx, byp + my, bzp, c});
        band.push_back({bxp - mx, byp - my, bzp, c});
      }
      if (band.size() >= 4) {
        g_iso->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,
                               static_cast<UINT>(band.size()) - 2, band.data(),
                               sizeof(RouteVert));
        g_routeOnScreen = true;
      }
    }
  }

  // The rest of the route, exactly as it was built. Outline first, then the
  // brighter fill inside it.
  const size_t rest = tapEnd * 2;
  size_t visibleEnd = ArcIndex(geom->arc, g_match.s + 2.f * kFar);
  if (visibleEnd >= nc) visibleEnd = nc - 1;
  const size_t visibleVertices = (visibleEnd + 1) * 2;
  for (const std::vector<RouteVert> *strip : {&geom->outline, &geom->fill}) {
    const size_t end = visibleVertices < strip->size() ? visibleVertices : strip->size();
    // Nothing of this strip is left ahead of the player. That is a real state
    // rather than a rounding case: a match near the end of a short route puts
    // the whole of it behind the arrow. The panel has to know, because a route
    // it holds but cannot show is still a route being navigated to.
    if (rest + 4 > end) continue;
    g_iso->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,
                           static_cast<UINT>(end - rest) - 2,
                           strip->data() + rest, sizeof(RouteVert));
    g_routeOnScreen = true;
  }

  // The bias belongs to the ribbon alone; anything drawn after it must get the
  // depth it actually has.
  if (g_depthBias) {
    const float none = 0.f;
    g_iso->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,
                          *reinterpret_cast<const DWORD *>(&none));
    g_iso->SetRenderState(D3DRS_DEPTHBIAS,
                          *reinterpret_cast<const DWORD *>(&none));
  }
}


// The destination marker.
//
// Drawn in the isolated device's own screen space rather than at the
// destination's world position. The target is nearly always far beyond the
// three hundred unit far plane - a job across town is thousands of units out
// - so a marker placed in the world is off-camera essentially always, which
// is exactly why the first attempt at this never appeared.
//
// So it is projected when the destination is inside the view and pinned to
// the panel edge when it is not, pointing the way to a target that is not on
// screen yet. That is what the stock waypoint blip did and what a car GPS
// does; only the artwork changes.
//
// XYZRHW on g_iso, never on the game's device: this cannot disturb GTA's own
// render state the way drawing into its frame did.
// Place one destination's dot. `slot` is which of g_markers it writes to.
// The camera the last capture was taken with.
//
// The projection below is only valid against a particular frame's view, and
// the blips that want it are placed later, during the HUD pass, by a different
// module entirely. Keeping the matrix is what lets that projection be asked
// for after the capture it belongs to has finished.
D3DMATRIX g_lastView{};
float g_lastViewZ = 0.f;
bool g_lastViewValid = false;

// Project a world position onto the panel, and pin it to the rim when it falls
// outside - the whole of what makes the destination dot point at its
// destination instead of drifting.
//
// Returns panel fractions, 0..1 across and down. False when no capture has
// been taken yet, or the numbers do not survive the projection.
bool ProjectWorldToPanel(float wx, float wy, float &u, float &v) {
  if (!g_lastViewValid) return false;
  const D3DMATRIX &w = g_lastView;
  const float dz = g_lastViewZ;
  const float vx = wx * w._11 + wy * w._21 + dz * w._31 + w._41;
  const float vy = wx * w._12 + wy * w._22 + dz * w._32 + w._42;
  const float vz = wx * w._13 + wy * w._23 + dz * w._33 + w._43;
  if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz))
    return false;

  const float aspect = g_panelAspect;
  const float yScale = 1.f / tanf(kFovY * .5f);
  float px, py;
  bool offscreen;
  if (vz > kNear) {
    px = (vx * (yScale / aspect) / vz) * .5f + .5f;
    py = .5f - (vy * yScale / vz) * .5f;
    offscreen = px < 0.f || px > 1.f || py < 0.f || py > 1.f;
  } else {
    // Behind the near plane the projection inverts and means nothing. The
    // view-space direction still names an edge, which is all a pinned marker
    // needs - and this is the case that matters most here, because anything
    // behind the player is exactly what the flat radar transform used to
    // scatter across the middle of the panel.
    px = .5f + vx;
    py = .5f - vy;
    offscreen = true;
  }

  if (offscreen) {
    float ex = px - .5f, ey = py - .5f;
    if (fabsf(ex) < 1e-6f && fabsf(ey) < 1e-6f) { ex = 0.f; ey = -.5f; }
    const float ax = fabsf(ex) / .5f, ay = fabsf(ey) / .5f;
    const float reach = ax > ay ? ax : ay;
    if (reach > 1.f) {
      px = .5f + ex / reach;
      py = .5f + ey / reach;
    }
  }
  if (!std::isfinite(px) || !std::isfinite(py)) return false;
  u = px;
  v = py;
  return true;
}

void PlaceDestMarker(const D3DMATRIX &w, float oz, const DestState &dest,
                     int slot) {
  g_markers[slot].on = false;
  if (!dest.active) return;
  g_markers[slot].kind = dest.kind;

  // World -> view. The view matrix is identity, so the world matrix is the
  // entire transform, and D3D multiplies row vectors: v' = v * M.
  const float dx = dest.x, dy = dest.y, dz = oz;
  const float vx = dx * w._11 + dy * w._21 + dz * w._31 + w._41;
  const float vy = dx * w._12 + dy * w._22 + dz * w._32 + w._42;
  const float vz = dx * w._13 + dy * w._23 + dz * w._33 + w._43;

  const float capW = float(g_capW), capH = float(g_capH);
  const float aspect = g_panelAspect;
  const float yScale = 1.f / tanf(kFovY * .5f);

  float sx, sy;
  bool offscreen;
  if (vz > kNear) {
    sx = ((vx * (yScale / aspect) / vz) * .5f + .5f) * capW;
    sy = (.5f - (vy * yScale / vz) * .5f) * capH;
    offscreen = sx < 0.f || sx > capW || sy < 0.f || sy > capH;
  } else {
    // Behind the near plane the projection inverts and is meaningless. Steer
    // by the view-space direction instead; it only has to name an edge.
    sx = capW * .5f + vx;
    sy = capH * .5f - vy;
    offscreen = true;
  }

  if (offscreen) {
    // Slide the marker back along the line from the centre until it sits on
    // the panel boundary, so it marks the bearing to the destination.
    //
    // Its centre lands on the boundary itself, the way the N compass rides
    // the edge, rather than being tucked wholly inside it. The limits are
    // therefore the half-extents of the panel with nothing subtracted: pull
    // the marker's own radius off them, as this used to, and it sits a full
    // marker's width in from the rim instead of on it.
    const float cx = capW * .5f, cy = capH * .5f;
    float ex = sx - cx, ey = sy - cy;
    if (fabsf(ex) < 1e-4f && fabsf(ey) < 1e-4f) { ex = 0.f; ey = -1.f; }
    const float limX = cx, limY = cy;
    const float ax = fabsf(ex) / limX, ay = fabsf(ey) / limY;
    const float scale = 1.f / (ax > ay ? ax : ay);
    sx = cx + ex * scale;
    sy = cy + ey * scale;
  }

  // Fog the marker while it is out in the scene, so a destination deep in the
  // haze is not a bright dot floating over ground that has faded to nothing.
  // It comes out of the fog as it is approached, like everything else.
  //
  // The vertices are XYZRHW, so the pipeline's own vertex fog cannot help -
  // there is no depth left for it to work from by then. The same linear ramp
  // the scene is drawn with is applied to the marker's alpha here instead,
  // rebuilt from the same constants RenderIso uses.
  //
  // Only while it is genuinely out there, though. Once it is pinned to the
  // panel edge it has stopped being a thing in the world and has become the
  // sign that says which way to go, and that has to stay readable at any
  // distance - which is the whole reason it is pinned there.
  float fogFade = 1.f;
  if (!offscreen) {
    const float fogStart = kCamDist + 105.f, fogEnd = kFar * .92f;
    fogFade = (fogEnd - vz) / (fogEnd - fogStart);
    if (fogFade > 1.f) fogFade = 1.f;
    if (fogFade < 0.f) fogFade = 0.f;
    if (fogFade <= 0.004f) return; // wholly fogged out; nothing to draw
  }
  // Hand the position to the HUD pass rather than drawing it here.
  //
  // Drawn into the capture, the marker was cut in half whenever it rode the
  // panel edge: the capture is the panel, so everything past the boundary is
  // simply outside the image. The N compass does not have that problem because
  // game.cpp draws it on the real HUD, where it is free to straddle the rim.
  //
  // So the geometry moves to the HUD too, and only the position is worked out
  // here, as a fraction of the panel box. DrawDestMarkerHud puts the dot down
  // in Circle, alongside the loading dots and the border - the same device,
  // inside the same D3DSBT_ALL state block, so it is no more exposed to GTA's
  // render state than they are.
  g_markers[slot].u    = sx / capW;
  g_markers[slot].v    = sy / capH;
  g_markers[slot].fade = fogFade;
  g_markers[slot].on   = true;
}

// Both destinations, drawn as dots.
//
// The one being navigated to goes down second so it lands over the top of the
// other where the two overlap on the panel rim - the ribbon leads to that one,
// so it is the one that has to stay readable.
void DrawDestMarker(const D3DMATRIX &w, float oz) {
  // Every blip on the panel is projected against this, not just the two dots
  // below - see SprpRadar3DProjectWorld.
  g_lastView = w;
  g_lastViewZ = oz;
  g_lastViewValid = true;
  // A destination read off the game's own objective marker is already drawn -
  // by the game, as the triangle this was read from, in its own colour and
  // with its own black border. Putting a dot of ours on top of it is the same
  // place marked twice, in two different shapes.
  //
  // Nothing is lost by standing aside. The triangle is clamped to the panel
  // edge by the same rule ours would have been (see the blip clamp in
  // game.cpp), so it still shows the bearing once the objective is out of
  // range, which was the only thing our dot was doing that the game's was not.
  auto drawnByTheGame = [](const DestState &d) {
    return d.active && d.source == DestSource::Objective;
  };
  const DestState primary = Dest(), secondary = SecondaryDest();
  PlaceDestMarker(w, oz, drawnByTheGame(secondary) ? DestState{} : secondary, 1);
  PlaceDestMarker(w, oz, drawnByTheGame(primary) ? DestState{} : primary, 0);
}

// Put the destination dot on the HUD, in real screen pixels.
//
// Working in pixels rather than in the square capture also settles the shape
// for good: a circle here is a circle on screen, with no capture-to-panel
// stretch to divide back out.
//
// The radius is the N compass's own. game.cpp's ReshapeNorthSpriteRect gives N
// a half-size of 3.5 in HUD-Y units, and a HUD-Y unit is screenH/448 pixels,
// so the two markers riding the rim are the same size by construction rather
// than by a number copied between files.
void DrawDestMarkerHud(IDirect3DDevice9 *d, float cx, float cy, float hw,
                       float hh, float sy) {
  if (!g_markers[0].on && !g_markers[1].on) return;
  const radarcfg::Settings &cfg = radarcfg::Current();
  const float outer = cfg.markerSize * sy;
  // The ring was six of the capture's 1024 pixels across a panel spanning
  // the panel height in HUD units. Kept as that same proportion of the radius so
  // the outline reads identically at any resolution.
  // The stroke is a width now, not a share of the radius - see radarcfg.h,
  // MarkerOutlineWidth. As a share it thinned out whenever the dot was made
  // smaller and thickened when it was made bigger, so it never matched the
  // constant black border the game puts on its own markers. Held to at least a
  // third of the dot so a heavy setting cannot swallow the fill entirely.
  // In pixels, not HUD units. As HUD units this was multiplied by sy - about
  // 2.4 at 1080p - so a setting of 2 drew a 4.8 pixel ring on an 8.4 pixel
  // radius, over half the dot, which is nothing like the hairline the game
  // strokes its own markers with.
  const float strokeWidth = cfg.markerOutlineWidth;
  const float minimumFill = outer * .33f;
  float fillR = outer - strokeWidth;
  if (fillR < minimumFill) fillR = minimumFill;
  auto faded = [](uint32_t c, float f) -> uint32_t {
    const uint32_t a = uint32_t(float((c >> 24) & 0xFFu) * f + .5f);
    return (a << 24) | (c & 0x00FFFFFFu);
  };

  d->SetTexture(0, nullptr);
  d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  d->SetRenderState(D3DRS_ZENABLE, FALSE);
  d->SetRenderState(D3DRS_FOGENABLE, FALSE);
  d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  // The secondary dot first, so the one the ribbon leads to lands on top of
  // it where the two overlap.
  for (int slot = 1; slot >= 0; slot--) {
    const MarkerPlacement &m = g_markers[slot];
    if (!m.on) continue;
    const bool mission = m.kind == DestKind::Mission;
    const uint32_t fillColour =
        faded(mission ? cfg.missionMarker : cfg.gpsMarker, m.fade);
    const uint32_t ringColour =
        faded(mission ? cfg.missionMarkerOutline : cfg.gpsMarkerOutline, m.fade);
    const float px = cx - hw + m.u * 2.f * hw;
    const float py = cy - hh + m.v * 2.f * hh;

    struct DV { float x, y, z, w; uint32_t c; };
    constexpr int kSeg = 24;
    DV fill[kSeg + 2]{}, ring[kSeg + 2]{};
    fill[0] = {px, py, 0.f, 1.f, fillColour};
    ring[0] = {px, py, 0.f, 1.f, ringColour};
    for (int i = 0; i <= kSeg; i++) {
      const float a = 6.28318530f * i / kSeg;
      const float ca = cosf(a), sa = sinf(a);
      fill[i + 1] = {px + ca * fillR, py + sa * fillR, 0.f, 1.f, fillColour};
      ring[i + 1] = {px + ca * outer, py + sa * outer, 0.f, 1.f, ringColour};
    }
    // Outline first, dot over the top of it.
    d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kSeg, ring, sizeof(DV));
    d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kSeg, fill, sizeof(DV));
  }
}

// Watch the game's own map waypoint - the target blip the player places by
// right-clicking the pause map, and clears the same way.
//
// This is watched whoever currently owns navigation, which it did not used to
// be. The old version only looked at the blip in order to take navigation from
// it, so a destination that had arrived from the server was untouched by the
// player clearing the marker that stood for it - and on a server that is the
// normal case, not the odd one. SA-MP tells the gamemode about the click
// (OnPlayerClickMap), the gamemode routes to it and sends the destination
// back, so the destination the player sees is the server's even though they
// created it themselves by clicking the map. Clearing it then did nothing at
// all: SA-MP has no callback for removing a waypoint, so the server never
// heard, its reroute timer carried on re-sending the same destination twice a
// second, and the pink dot and ribbon stayed up for good.
//
// The blip going away is therefore the one signal that the player is finished
// with a destination, and it is only visible here.
// The player's own map marker is a GPS destination and nothing else. Every
// test in here is therefore against the GPS slot rather than against whatever
// currently holds the ribbon: placing a marker during a mission must not take
// the ribbon off the objective, and clearing one must not end the mission.
void PollWaypoint() {
  const int32_t handle = *reinterpret_cast<const int32_t *>(kTargetBlipIndex);
  EnterCriticalSection(&g_routeLock);
  const DestState gps = g_gpsDest;
  LeaveCriticalSection(&g_routeLock);

  // A marker still on the pause map is the map still asking for the place it
  // stands on, exactly as a reroute timer is the server still asking. Both
  // hold the arrival guard down; neither can lift it. Without this the guard
  // would go quiet a few seconds after arriving in single player, and the
  // restore below would put the ribbon back up for somewhere already reached.
  if (g_spentActive[SpentSlot(DestKind::Gps)] && g_waypointActive) {
    const int slot = SpentSlot(DestKind::Gps);
    const float sx = g_waypointX - g_spentX[slot], sy = g_waypointY - g_spentY[slot];
    if (sx * sx + sy * sy <= kMarkerSnap * kMarkerSnap) TouchSpent(DestKind::Gps);
  }
  if (handle != g_lastBlipHandle) {
    const bool hadBlip = g_lastBlipHandle != 0;
    g_lastBlipHandle = handle;

    if (!handle) {
      // The marker is gone. Whether that ends navigation depends on whether
      // navigation was going to the marker: a job's drop-off or a mission
      // objective is somewhere else and stands on its own, and clearing a
      // marker must not silently cancel it.
      const DestState dest = gps;
      const float dx = dest.x - g_waypointX, dy = dest.y - g_waypointY;
      const bool destWasTheMarker =
          dest.active && g_waypointActive &&
          dx * dx + dy * dy <= kMarkerSnap * kMarkerSnap;
      const bool ours = dest.active && dest.source == DestSource::Waypoint;
      g_waypointActive = false;
      g_waypointHandle = 0;
      if (!hadBlip || !(ours || destWasTheMarker)) {
        logfile::Line("radar3d: map marker cleared; navigation is elsewhere "
                      "and stands");
        return;
      }
      // Remember it as finished with. Without this the server's next reroute -
      // at most half a second away - puts the destination the player just
      // removed straight back, and it looks like the removal did nothing.
      MarkSpent(DestKind::Gps, dest.x, dest.y);
      EndGpsNavigation();
      logfile::Line("radar3d: map marker cleared at %.1f, %.1f - navigation "
                    "ended and re-sends of it refused",
                    dest.x, dest.y);
      return;
    }

    // A marker has appeared. Read where from the map cursor, which is on it
    // at this moment and will have moved on by the time anything else looks.
    const float *cursor = reinterpret_cast<const float *>(kMapCursorWorld);
    const float wx = cursor[0], wy = cursor[1];
    if (!std::isfinite(wx) || !std::isfinite(wy)) return;
    g_waypointX = wx;
    g_waypointY = wy;
    g_waypointActive = true;
    g_waypointHandle = handle;
    // Marking a place is asking for it, whatever was decided about it before.
    g_spentActive[SpentSlot(DestKind::Gps)] = false;

    // Take navigation, unless the server is already navigating somewhere else.
    // Its destination is one the game cannot express - a job's drop-off, a
    // property from the finder - and the player marking a second place on the
    // map does not abandon it.
    const DestState dest = gps;
    const float sx = dest.x - wx, sy = dest.y - wy;
    const bool serverElsewhere =
        dest.active && dest.source == DestSource::Server &&
        sx * sx + sy * sy > kArriveDist * kArriveDist;
    if (serverElsewhere) {
      logfile::Line("radar3d: map marker at %.1f, %.1f noted; the server's "
                    "destination keeps navigation", wx, wy);
      return;
    }
    EnterCriticalSection(&g_routeLock);
    g_gpsDest = {wx, wy, true, DestSource::Waypoint, DestKind::Gps};
    LeaveCriticalSection(&g_routeLock);
    // Arbitrate drops any stale route and forces a fresh search when this
    // marker actually takes the ribbon, so the panel shows the loading dots
    // rather than the old route for a second. During a mission it takes
    // nothing, and the marker appears as a second dot with no ribbon.
    Arbitrate();
    logfile::Line("radar3d: map marker set at %.1f, %.1f", wx, wy);
    return;
  }

  // The handle has not changed. A scripted destination may have owned
  // navigation while the player's marker sat underneath it; once that ends,
  // put the marker back, because no handle change is coming to announce it.
  //
  // Arrival is the exception, and it used to be a live loop. Arriving ends
  // navigation but leaves the marker where it is - the player is standing on
  // it, and nothing about it has changed - so this found no destination and a
  // remembered marker and put it straight back, which put the player back
  // inside the arrival circle, which ended navigation again. That ran every
  // frame: a log line and an epoch bump per frame, every route search
  // cancelled as it landed, and the panel's answer to "am I navigating"
  // flickering, which is what lets the game's flat 2D route flash over it.
  if (!handle || handle != g_waypointHandle || !g_waypointActive) return;
  if (IsSpent(DestKind::Gps, g_waypointX, g_waypointY)) return;
  if (gps.source != DestSource::None) return;
  EnterCriticalSection(&g_routeLock);
  g_gpsDest = {g_waypointX, g_waypointY, true, DestSource::Waypoint,
               DestKind::Gps};
  LeaveCriticalSection(&g_routeLock);
  Arbitrate();
  logfile::Line("radar3d: restoring the map marker at %.1f, %.1f", g_waypointX,
                g_waypointY);
}

// Work a route out ourselves when the server has not supplied one.
//
// The search runs on its own thread. It is quick, but not so quick that it
// belongs on a frame, and nothing here waits for the answer: when it arrives
// it simply replaces the geometry, the same way a server route does.
// The player's own position, in a car or on foot.
//
// Sample() is no use here: it refuses to produce anything outside a vehicle,
// on purpose, because the panel is a driving display. Arriving somewhere is
// not a driving event, so it needs a position that still exists on foot.
// The entity matrix holds the world position at +0x30; a ped that has not
// been given one yet keeps its position inline at +0x04.
bool PlayerXY(float &x, float &y) {
  auto *entity = reinterpret_cast<uint8_t *(__cdecl *)(int, bool)>(
      kFindPlayerVehicle)(-1, false);
  if (!entity)
    entity = reinterpret_cast<uint8_t *(__cdecl *)(int)>(kFindPlayerPed)(-1);
  if (!entity) return false;
  const auto matrix = *reinterpret_cast<uintptr_t *>(entity + 0x14);
  const float *p = matrix ? reinterpret_cast<const float *>(matrix + 0x30)
                          : reinterpret_cast<const float *>(entity + 0x04);
  x = p[0];
  y = p[1];
  return std::isfinite(x) && std::isfinite(y);
}

// Reaching the destination, and leaving one already reached.
//
// Called every frame from the present hook rather than from the route search,
// because reaching somewhere is not a driving event. The search only runs
// while the panel is being drawn, and the panel is only drawn in a vehicle -
// so with this left where it was, parking short of the destination and
// walking the last stretch never counted as arriving. Navigation stayed live
// with the player standing on top of it, and ended only when they next got
// into a car.
void ArriveIfReached(const DestState &dest, float px, float py);

// How long the objective marker may go unseen before it is treated as gone.
//
// It is read off the radar draw, so it stops being seen the moment it leaves
// radar range - which happens constantly on a long mission and is not the
// mission ending. The ribbon would blink out every time the objective fell
// behind, so a gap has to be long enough to cover that and short enough that a
// mission which actually ended does not leave a ribbon up.
//
// It does not have to carry the whole trip. The destination, once taken, is
// held by the slot and routed to from the road graph, and PollArrival is what
// ends it - this only has to notice the marker being replaced or withdrawn.
constexpr DWORD kObjectiveGoneMs = 4000;

// Follow the game's own objective marker - the yellow triangle a single-player
// mission puts on the radar.
//
// This is the whole of single-player mission support, and it needs no server:
// game::ObjectiveBlip recovers the marker's world position from the radar draw
// itself, and from here it is the same destination as any other. It takes the
// mission slot, so it gets the mission colour and outranks the player's own
// waypoint for the ribbon exactly as a gamemode objective does - one yellow
// ribbon to the objective, the pink dot still standing wherever the player
// marked, which is the rule this was all built around.
//
// A destination the server set wins. On SP-RP the gamemode knows what the
// objective is and says so directly; this reads a marker off a radar draw and
// is the weaker account of the same thing, so it stands aside rather than
// fighting a server that is already sending one.
void PollObjectiveMarker() {
  game::Point marker{};
  unsigned long age = 0;
  uint32_t rgb = 0;
  const bool present =
      game::ObjectiveBlip(marker, age, rgb) && age <= kObjectiveGoneMs;
  if (present && rgb) {
    const uint32_t opaque = 0xFF000000u | rgb;
    // A colour change is a reason to rebuild the ribbon, the same as a colour
    // changed in the ini is - the geometry carries it in its vertices.
    if (g_objectiveColour.exchange(opaque) != opaque) {
      EnterCriticalSection(&g_routeLock);
      g_routeGeom.reset();
      g_routeOnScreen = false;
      g_lastLocalRoute = 0;
      g_routeEpoch.fetch_add(1);
      LeaveCriticalSection(&g_routeLock);
    }
  }

  EnterCriticalSection(&g_routeLock);
  const bool serverOwns = g_missionDest.active &&
                          g_missionDest.source == DestSource::Server;
  const bool oursActive = g_missionDest.active &&
                          g_missionDest.source == DestSource::Objective;
  LeaveCriticalSection(&g_routeLock);
  if (serverOwns) return;

  if (!present) {
    if (!oursActive) return;
    EndNavigation(DestKind::Mission);
    logfile::Line("radar3d: objective marker gone; the mission ribbon is down");
    return;
  }

  if (!std::isfinite(marker.x) || !std::isfinite(marker.y)) return;
  if (IsSpent(DestKind::Mission, marker.x, marker.y)) {
    TouchSpent(DestKind::Mission);
    return;
  }

  EnterCriticalSection(&g_routeLock);
  const DestState current = g_missionDest;
  const bool same = current.active &&
                    fabsf(marker.x - current.x) < kSameDest &&
                    fabsf(marker.y - current.y) < kSameDest;
  if (!same)
    g_missionDest = {marker.x, marker.y, true, DestSource::Objective,
                     DestKind::Mission};
  LeaveCriticalSection(&g_routeLock);
  if (same) return;
  Arbitrate();
  logfile::Line("radar3d: objective marker at %.1f, %.1f taken as the mission "
                "destination", marker.x, marker.y);
}

void PollArrival(float px, float py) {
  // Everything having stopped asking for the destination already finished
  // with releases the guard, so the same place can be navigated to again -
  // taking the same job a second time, or re-marking somewhere visited.
  //
  // Only silence does it. Distance used to, and it was the wrong measure
  // entirely: see g_spentSeenAt, where a reroute timer that never learns the
  // player arrived kept re-sending, and driving sixty units away was all it
  // took to let the next one through - so the pink dot and the ribbon came
  // back for a place already reached, with no marker on the pause map to
  // match them.
  for (int slot = 0; slot < 2; slot++) {
    if (g_spentActive[slot] &&
        GetTickCount() - g_spentSeenAt[slot] >= kSpentQuietMs) {
      g_spentActive[slot] = false;
      logfile::Line("radar3d: nothing has asked for the last %s destination in "
                    "%u seconds; it may be set again",
                    slot ? "mission" : "gps", unsigned(kSpentQuietMs / 1000));
    }
  }

  // Both destinations are tested, not just the one holding the ribbon. A GPS
  // destination that is only a dot is still somewhere the player can drive to,
  // and arriving at it has to clear it - otherwise the dot outlives the trip
  // and sits on the panel until the mission ends.
  EnterCriticalSection(&g_routeLock);
  const DestState slots[2] = {g_gpsDest, g_missionDest};
  LeaveCriticalSection(&g_routeLock);
  for (const DestState &dest : slots) {
    if (!dest.active) continue;
    ArriveIfReached(dest, px, py);
  }
}

// One destination's arrival test.
void ArriveIfReached(const DestState &dest, float px, float py) {

  // Arrived.
  //
  // Take the ribbon down and, more importantly, stop putting another one up.
  // This is checked even while the server owns the route, because the way the
  // ribbon came back was the server clearing its own route on arrival: that
  // handed the ribbon to us, and we promptly routed the player to the
  // waypoint they were already standing on.
  //
  // The destination stays inactive until a different one is set. Reaching
  // somewhere and then driving off should not resume navigating to it.
  // Arrival allows for the roads not going all the way there. See
  // kOffRoadReach: the shortfall the route already measured is how close a car
  // can physically get, so judging arrival without it means a destination off
  // the network is never reached however long the player sits on top of it.
  //
  // The shortfall only counts for the destination the route was actually
  // built for. A route to a mission objective measures nothing about how close
  // the roads get to a GPS destination on the other side of town, and lending
  // it that slack would arrive at places the player is nowhere near.
  float shortfall = 0.f;
  EnterCriticalSection(&g_routeLock);
  const bool routedHere = g_routeGeom && g_dest.active &&
                          g_dest.kind == dest.kind;
  if (routedHere) shortfall = g_routeGeom->endGap;
  LeaveCriticalSection(&g_routeLock);
  if (shortfall > kOffRoadReach) shortfall = kOffRoadReach;
  const float reach = kArriveDist + shortfall;

  const float ddx = dest.x - px, ddy = dest.y - py;
  if (ddx * ddx + ddy * ddy <= reach * reach) {
    // Record where we arrived, so the server re-sending it is refused.
    MarkSpent(dest.kind, dest.x, dest.y);
    EndNavigation(dest.kind);
    logfile::Line("radar3d: %s destination reached, navigation ended",
                  dest.kind == DestKind::Mission ? "mission" : "gps");
  }
}

// Work a route out ourselves when the server has not supplied one.
//
// The search runs on its own thread. It is quick, but not so quick that it
// belongs on a frame, and nothing here waits for the answer: when it arrives
// it simply replaces the geometry, the same way a server route does.
void RequestLocalRoute(float px, float py) {
  if (!Dest().active) return;

  // Routing is ours whenever we have a graph, whoever set the destination.
  if (!router::Ready()) return;
  if (g_localRouteBusy.load()) return;

  // Only work a new route out when there is a reason to.
  //
  // A route to a fixed destination does not go stale as it is driven: the
  // matcher trims off the part already covered, and the rest is as good as it
  // was when it was found. Recomputing on a timer therefore replaced the
  // ribbon with an all but identical one every couple of seconds, which is
  // visible as a blink even standing still. Having left the route is the
  // reason to find another; nothing else is.
  bool haveRoute = false;
  uint32_t geomId = 0;
  bool unreachable = false;
  EnterCriticalSection(&g_routeLock);
  if (g_routeGeom) { haveRoute = true; geomId = g_routeGeom->id; }
  unreachable = g_routeUnreachable;
  LeaveCriticalSection(&g_routeLock);

  // The graph has already said it cannot reach this destination. That answer
  // is about the destination, not about where the player is standing, so it
  // will not become a different answer on the next attempt - and searching for
  // it twice a second for the rest of the trip only spends threads to be told
  // the same thing. The panel says so instead: see Circle, which stands the
  // loading dots down, and SprpRadar3DHasGpsRoute, which hands the route back
  // to the game's own GPS rather than suppressing it in favour of nothing.
  if (unreachable) {
    // Not for ever, though. The half-distance test is measured from where the
    // player was, so an answer taken at one end of the map is not necessarily
    // the answer at the other. Far enough away and it is worth asking again.
    const float udx = px - g_lastTryX, udy = py - g_lastTryY;
    if (udx * udx + udy * udy < kUnreachableRetryMove * kUnreachableRetryMove)
      return;
    EnterCriticalSection(&g_routeLock);
    g_routeUnreachable = false;
    LeaveCriticalSection(&g_routeLock);
  }

  // A route the matcher has not seen yet. Give it a frame to place us on it
  // before concluding we are nowhere near it.
  if (haveRoute && g_match.routeId != geomId) return;

  const bool offRoute = !haveRoute || !g_match.valid ||
                        g_match.cross > kOffRouteDist;
  const DWORD now = GetTickCount();
  if (!offRoute) { g_offRouteSince = 0; return; }

  // Having no route at all is not something to sit and think about. Having
  // drifted off one is, because the measurement is noisy at the boundary: a
  // cross-track distance hovering either side of kOffRouteDist - a parallel
  // carriageway, a wide junction, a route whose first node sits a lane over -
  // toggles this condition and reroutes every kLocalRerouteMs. Each reroute
  // swaps the geometry for a near identical one carrying a new id, the matcher
  // treats it as fresh, and the ribbon blinks. So a departure has to persist
  // before it is believed.
  if (haveRoute) {
    if (!g_offRouteSince) { g_offRouteSince = now; return; }
    if (now - g_offRouteSince < kOffRouteHold) return;
  }
  g_offRouteSince = 0;

  // Somewhere else entirely since the last attempt, so the reason it failed
  // may well be gone. Try again at once rather than serving out a backoff.
  const float mdx = px - g_lastTryX, mdy = py - g_lastTryY;
  if (g_routeFailures.load() && mdx * mdx + mdy * mdy > kRetryMove * kRetryMove)
    g_routeFailures = 0;

  DWORD wait = kLocalRerouteMs;
  const int failures = g_routeFailures.load();
  for (int i = 0; i < failures && i < 5; i++) wait *= 2;
  if (g_lastLocalRoute && now - g_lastLocalRoute < wait) return;
  g_lastLocalRoute = now;
  g_lastTryX = px;
  g_lastTryY = py;

  // Take the destination and the epoch together, under the lock. Read
  // separately, a destination arriving between the two hands the worker one
  // journey's coordinates and the other journey's epoch, and its answer is
  // then published as if it were current.
  EnterCriticalSection(&g_routeLock);
  const float dx = g_dest.x, dy = g_dest.y;
  const DestKind kind = g_dest.kind;
  const uint32_t epoch = g_routeEpoch.load();
  LeaveCriticalSection(&g_routeLock);

  g_localRouteBusy = true;
  try {
  std::thread([px, py, dx, dy, kind, epoch] {
    std::vector<float> pts;
    std::shared_ptr<const RouteGeometry> geom;
    // Set when the answer is about the destination rather than about where the
    // search started, which is the difference between an answer worth asking
    // for again and one that will be the same every time.
    bool unreachable = false;
    try {
    float endGap = -1.f;
    if (router::Route(px, py, dx, dy, pts, &endGap)) {
      geom = BuildRouteGeometry(pts, dx, dy, kind);
      if (geom && !geom->endsAtDest) {
        // The roads stop short of the destination. That is ordinary - a jetty,
        // a field, an island - and the ribbon covers the drivable part of the
        // trip the way the pause map's route does. It is only wrong when the
        // road it reaches is not on the way; see kMustClose.
        const float sdx = px - dx, sdy = py - dy;
        const float startGap = sqrtf(sdx * sdx + sdy * sdy);
        // Small in itself, or small against the trip. The first test is what
        // stops the second from throwing a good route away on the last corner:
        // a fraction of the distance remaining shrinks as the player arrives,
        // so a destination a car park's width off the road passed the test at
        // the start of the journey and failed it at the end, and the ribbon
        // disappeared exactly where it was most wanted.
        if (geom->endGap <= kOffRoadReach ||
            geom->endGap < startGap * kMustClose) {
          logfile::Line("radar3d: roads stop %.0f short of %.1f,%.1f - drawing "
                        "the drivable part of the trip",
                        geom->endGap, dx, dy);
        } else {
          unreachable = true;
          geom.reset();
        }
      }
    }
    if (unreachable) {
      logfile::Line("radar3d: no road goes towards %.1f,%.1f - the destination "
                    "stands and the dot points at it, but a ribbon to the "
                    "nearest road would not be taking anyone there", dx, dy);
    } else if (!geom) {
      // Only worth saying the first few times: the retry backs off, and a
      // player parked well off the network would otherwise fill the log with
      // the same line until they drove back onto a road.
      if (g_routeFailures < 3)
        logfile::Line("radar3d: no road path from %.1f,%.1f to %.1f,%.1f"
                      " - still looking", px, py, dx, dy);
    }
    } catch (...) {
      logfile::Line("radar3d: route construction failed; retaining the previous route");
    }
    // Publish under the lock, and check the epoch while holding it. Checking
    // outside and storing inside leaves a window for a new destination to
    // arrive in between, which published this route for a journey nobody
    // asked for.
    EnterCriticalSection(&g_routeLock);
    if (epoch == g_routeEpoch.load()) {
      if (geom) {
        g_routeGeom = geom;
        g_routeUnreachable = false;
        logfile::Line("radar3d: route ready to %.1f,%.1f (%u points, %.0fm)",
                      dx, dy, unsigned(geom->centres.size() / 2), geom->totalArc);
      } else if (unreachable) {
        g_routeUnreachable = true;
      }
      g_routeFailures.store(geom ? 0 : g_routeFailures.load() + 1);
    }
    LeaveCriticalSection(&g_routeLock);
    g_localRouteBusy = false;
  }).detach();
  } catch (...) {
    // Out of threads, or out of memory for one. Releasing the flag matters
    // more than the failed search does: left set, it stops every later search
    // for the rest of the session, and the GPS simply never works again.
    g_localRouteBusy = false;
    logfile::Line("radar3d: could not start the route search thread");
  }
}

bool RenderIso(float ox, float oy, float oz, float /*range*/, float angle) {
  if (!MakeIso())
    return false;

  // Work out where on the route we are, before the camera is placed, because
  // the ribbon is drawn from the result.
  //
  // The camera is NOT moved onto it. Centring the world on the matched point
  // is what a car GPS does with its vehicle marker, and it is wrong here: the
  // marker is fixed in the panel and the world moves under it, so correcting
  // the match shoves the entire world sideways and reads as the map snapping
  // about. The player's own position is the one thing on this display that
  // must never be second-guessed.
  //
  // One snapshot, taken here and used for both the match and the draw. They
  // must be the same route or the draw will not recognise the match.
  std::shared_ptr<const RouteGeometry> routeGeom;
  {
    EnterCriticalSection(&g_routeLock);
    routeGeom = g_routeGeom;
    LeaveCriticalSection(&g_routeLock);
    if (routeGeom) MapMatch(*routeGeom, ox, oy);
  }
  float s = sinf(angle), c = cosf(angle), tilt = kTilt, ct = cosf(tilt),
        st = sinf(tilt);
  D3DMATRIX w{};
  w._11 = c;
  w._21 = s;
  w._12 = -s * ct;
  w._22 = c * ct;
  w._32 = st;
  w._13 = -s * st;
  w._23 = c * st;
  w._33 = -ct;
  w._41 = -(c * ox + s * oy);
  // The radar exposes X/Y only. At this forward tilt, using absolute Z makes
  // elevated terrain slide away from the fixed player marker after a distant
  // teleport. Centre the camera on the full player position instead.
  // kPlayerDrop pushes the player below view-space centre (negative view Y
  // projects to the bottom of the screen in D3D), matching the reference:
  // the marker sits in the lower third with more road visible ahead of it
  // rather than dead centre.
  // The boom, shortened by whatever the camera is currently up against. The
  // drop is scaled with it: the player's place on screen comes out of
  // drop/depth, so holding the drop fixed while the depth shrank would slide
  // the car up the panel every time the camera met a wall.
  const float camDist    = kCamDist * g_camScale;
  const float playerDrop = kPlayerDrop * g_camScale;
  w._42 = ct * (s * ox - c * oy) - st * oz - playerDrop;
  w._43 = st * (s * ox - c * oy) + ct * oz + camDist;
  w._44 = 1;
  // Perspective projection. Aspect matches the HUD panel so the capture fills
  // the rectangle without stretch. Player sits at view-space depth kCamDist
  // and projects to screen centre; road ahead recedes into the distance.
  D3DMATRIX v = Mat(), p{};
  const float yScale = 1.f / tanf(kFovY * .5f);
  const float aspect = g_panelAspect;
  p._11 = yScale / aspect;
  p._22 = yScale;
  p._33 = kFar / (kFar - kNear);
  p._34 = 1.f;
  p._43 = -kNear * kFar / (kFar - kNear);
  g_iso->SetTransform(D3DTS_WORLD, &w);
  g_iso->SetTransform(D3DTS_VIEW, &v);
  g_iso->SetTransform(D3DTS_PROJECTION, &p);
  // The navigation grade: how much of a surface's own colour survives, and
  // what the rest of it is mixed with. Filled in by the fog block below and
  // used by both draw passes.
  //
  // It used to be computed there and then thrown away - every draw overwrote
  // TEXTUREFACTOR with 0xa0ffffff, pure white. So the time-of-day tint never
  // did anything at all: buildings blended toward white at midnight exactly as
  // at noon, which is the flashbang-white look, and why this panel washed out
  // pale where the reference stays a flat mid grey.
  //
  // The alpha is the share of the texture that survives. At the old 0xa0 that
  // was 63%, leaving 37% of the horizon colour laid over everything, and by
  // day the horizon is nearly white. 0xc4 keeps 77%, which holds the reference
  // contrast while still letting distance fall away.
  const DWORD kGradeAlpha = radarcfg::Current().navigationGrade;
  DWORD gradeFactor = (kGradeAlpha << 24) | 0x00ffffff;

  // Sample the game clock to derive a time-of-day fog palette. The isolated
  // renderer has its own clear color and fog that the game's timecyc does not
  // touch, so without this the radar is always daytime-bright even at night.
  // CClock::ms_nGameClockHours (0xB70153, uint8) and ...Minutes (0xB70152).
  {
    const float hour = *reinterpret_cast<const uint8_t *>(0xB70153) +
                       *reinterpret_cast<const uint8_t *>(0xB70152) / 60.f;
    // nightFactor: 0 = full day, 1 = full night
    float nightFactor;
    if (hour >= 6.f && hour <= 18.f)
      nightFactor = 0.f;
    else if (hour > 18.f && hour < 21.f)
      nightFactor = (hour - 18.f) / 3.f;
    else if (hour >= 3.f && hour < 6.f)
      nightFactor = 1.f - (hour - 3.f) / 3.f;
    else
      nightFactor = 1.f;
    auto lerp8 = [](uint32_t a, uint32_t b, float t) -> uint32_t {
      return uint32_t(float(a) + (float(b) - float(a)) * t + .5f);
    };
    // Day: #D9D9DE (neutral grey-blue).  Night: #08101E (deep navy).
    const uint32_t fogR = lerp8(0xD9, 0x08, nightFactor);
    const uint32_t fogG = lerp8(0xD9, 0x10, nightFactor);
    const uint32_t fogB = lerp8(0xDE, 0x1E, nightFactor);
    const DWORD fogColor = 0xFF000000u | (fogR << 16) | (fogG << 8) | fogB;
    g_captureClearColor = fogColor;
    // The BLENDFACTORALPHA op blends textures toward this color at the
    // horizon, so at night white buildings tint toward dark instead of white.
    gradeFactor = (kGradeAlpha << 24) | (fogR << 16) | (fogG << 8) | fogB;
    // Navigation-map grade: low-contrast terrain against the time-of-day sky.
    g_iso->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, fogColor, 1, 0);
    if (FAILED(g_iso->BeginScene()))
      return false;
    g_iso->SetVertexShader(nullptr);
    g_iso->SetPixelShader(nullptr);
    g_iso->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    g_iso->SetRenderState(D3DRS_LIGHTING, FALSE);
    const float fogStart = kCamDist + 105.f;
    const float fogEnd   = kFar * .92f;
    g_iso->SetRenderState(D3DRS_FOGENABLE, TRUE);
    g_iso->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    g_iso->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    g_iso->SetRenderState(D3DRS_FOGCOLOR, fogColor);
    g_iso->SetRenderState(D3DRS_FOGSTART,
                          *reinterpret_cast<const DWORD *>(&fogStart));
    g_iso->SetRenderState(D3DRS_FOGEND,
                          *reinterpret_cast<const DWORD *>(&fogEnd));
    g_iso->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_iso->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_iso->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_iso->SetRenderState(D3DRS_TEXTUREFACTOR, gradeFactor);
  }
  SetWorldStages();
  g_iso->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  g_iso->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  // Minification is where this view lives - almost everything on screen is
  // road running away toward the far plane, compressed hard along one axis by
  // the 72 degree camera tilt. That is precisely what anisotropic filtering is
  // for: it takes several taps along the direction of compression instead of
  // one, which is what stops the road surface crawling as the car moves.
  g_iso->SetSamplerState(0, D3DSAMP_MINFILTER,
                         g_anisotropy > 1 ? D3DTEXF_ANISOTROPIC
                                          : D3DTEXF_LINEAR);
  g_iso->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, g_anisotropy);
  g_iso->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  // No mip chain, deliberately. Each .r3a is a texture atlas holding many
  // separate cells, and world3d-radar-3dpack.py insets its UVs by only
  // SAFE_UV - one and a half texels at 512. The smallest cell in the set is
  // about twelve texels across, so by the second mip level a cell is three
  // texels wide with well under half a texel of guard, and the filter starts
  // averaging in whatever unrelated texture is packed next door. Mipmapping
  // this properly needs the bake to emit per-cell gutters, not a runtime
  // change. Anisotropic minification above does the same job here without
  // ever sampling outside the cell.
  g_iso->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  // Multisampling only antialiases geometry edges. The ribbon and the tile
  // seams are edges; the interiors are not, which is what the mip chain is for.
  g_iso->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,
                        g_samples != D3DMULTISAMPLE_NONE);
  D3DCAPS9 caps{};
  g_iso->GetDeviceCaps(&caps);
  UINT maxPrims = caps.MaxPrimitiveCount ? caps.MaxPrimitiveCount : 65535,
       draws = 0;
  const float extent = CaptureRange(0.f);
  const bool drewWater = DrawWaterPlane(ox,oy,extent*1.1f);
  int minx = (int)floorf((ox - extent) / kTile),
      maxx = (int)floorf((ox + extent) / kTile),
      miny = (int)floorf((oy - extent) / kTile),
      maxy = (int)floorf((oy + extent) / kTile);
  // Retire unused neighbours before allocating the visible tiles.
  Retire(minx, maxx, miny, maxy);
  g_lastMinX = minx;
  g_lastMaxX = maxx;
  g_lastMinY = miny;
  g_lastMaxY = maxy;
  int tilesLoaded = 0;
  // Where the scene is really being drawn from.
  //
  // The player sits at view (0, -kPlayerDrop, kCamDist), so the camera is that
  // far back and up from them in world terms. This is the only sight line that
  // matters: something hides the player exactly when it stands between this
  // point and the player.
  //
  // A second ray used to be cast straight down from three hundred units up, to
  // answer "is there a roof over the player". That is not the question this
  // display asks. The camera looks in from about thirty degrees above the
  // horizon, so a bridge deck or a gantry above that line hides nothing - and
  // the overhead ray faded it anyway, along with everything else in a three
  // hundred unit column. A roof the player is genuinely under belongs to a
  // building whose walls the camera ray already crosses, because the bake
  // keeps a building's roof in the same group as its walls.
  const float camBack = st * camDist - ct * playerDrop;
  const float camWx = ox + s * camBack;
  const float camWy = oy - c * camBack;
  const float camWz = oz + st * playerDrop + ct * camDist;

  // Camera collision: never render from inside the world.
  //
  // Cast from just above the car out to where the camera would sit at full
  // boom length, against everything - buildings, terrain, road decks. The
  // first thing hit is where the boom has to stop, so the camera is placed a
  // little short of it and the car stays in view. This is what stops the panel
  // filling with the inside of a hillside or the underside of a flyover, and
  // it does so without dissolving any ground, which is not something that can
  // be done safely at 128-unit cell granularity.
  //
  // Measured here, after this frame's tiles are in, and applied to the next
  // frame's matrix.
  {
    constexpr float kEyeLift = 1.2f;  // clear of the road surface itself
    const float fullBack = st * kCamDist - ct * kPlayerDrop;
    const float fullX = ox + s * fullBack, fullY = oy - c * fullBack;
    const float fullZ = oz + st * kPlayerDrop + ct * kCamDist;
    const float eyeZ = oz + kEyeLift;
    float boom = 1.f;
    const float bMinX = fullX < ox ? fullX : ox, bMaxX = fullX > ox ? fullX : ox;
    const float bMinY = fullY < oy ? fullY : oy, bMaxY = fullY > oy ? fullY : oy;
    const float bMinZ = fullZ < eyeZ ? fullZ : eyeZ,
                bMaxZ = fullZ > eyeZ ? fullZ : eyeZ;
    for (int ty = miny; ty <= maxy; ty++)
      for (int tx = minx; tx <= maxx; tx++)
        if (Tile *tile = Load(tx, ty))
          for (auto *group : tile->groups) {
            constexpr float pad = 1.f;
            if (group->maxx < bMinX - pad || group->minx > bMaxX + pad ||
                group->maxy < bMinY - pad || group->miny > bMaxY + pad ||
                group->maxz < bMinZ - pad || group->minz > bMaxZ + pad)
              continue;
            // The far end of this ray is the camera, so unlike the occlusion
            // test it is the far end that matters most: a wall right at the
            // camera is the whole point. Only the first fraction is trimmed,
            // to ignore the road immediately under the car.
            const float t = OcclusionHitT(*tile, *group, ox, oy, eyeZ,
                                          fullX, fullY, fullZ, false,
                                          .06f, 1.f);
            if (t < boom) boom = t;
          }
    // Stop a little short of whatever was hit, and never collapse onto the car
    // entirely - a very short boom is still a usable view, no view is not.
    float want = boom < 1.f ? boom * .85f : 1.f;
    if (want < .3f) want = .3f;
    // Snap in, ease out. Arriving late at a wall means a frame rendered from
    // inside it, which is the very thing being prevented; leaving late is only
    // a camera that takes a moment to settle back.
    g_camScale = want < g_camScale ? want
                                   : g_camScale + (want - g_camScale) * .12f;
  }

  // What fades, and why this is a search for one building rather than for
  // every crossing.
  //
  // The comment that used to stand here said a DrawGroup is not a building -
  // that the bake batches by material, so one group spans many buildings while
  // a single building's walls and roof land in different groups. That was
  // simply untrue, and five decisions were built on it. world3d-radar-3dpack.py
  // runs union-find over welded vertex positions and emits one group per
  // connected building, deliberately re-associating detached roofs and
  // terraces with their parent shell. flags&1 is one building, walls and roof
  // together, with tight bounds; flags==0 is ground and road in 128 unit cells.
  //
  // So the granularity is exactly right for "fade the one building in the
  // way", and that is what this does: the single nearest thing the sight line
  // actually passes through.
  //
  // The camera is guaranteed to be outside the world by the boom test above,
  // so all that is left to look for is something standing between it and the
  // car. There used to be a second test here - a stub cast backwards from the
  // camera to catch it being buried in a wall - and camera collision removes
  // the situation it existed for, so it is gone. It could only cause spurious
  // fades now, since anything behind a camera that is already in clear air is
  // by definition not in the way.
  DrawGroup *nearest = nullptr;
  float nearestT = 2.f;
  // Cheap reject first. Both segments together span about thirty units, so
  // nearly every group in capture range is nowhere near them and can be
  // dropped on six compares rather than a slab test and a triangle loop. The
  // stub lies beyond the camera, so a box around it and the player covers the
  // sight line too.
  auto lo3 = [](float a, float b) { return a < b ? a : b; };
  auto hi3 = [](float a, float b) { return a > b ? a : b; };
  const float segMinX = lo3(camWx, ox), segMaxX = hi3(camWx, ox);
  const float segMinY = lo3(camWy, oy), segMaxY = hi3(camWy, oy);
  const float segMinZ = lo3(camWz, oz), segMaxZ = hi3(camWz, oz);
  for (int ty = miny; ty <= maxy; ty++)
    for (int tx = minx; tx <= maxx; tx++)
      if (Tile *tile = Load(tx, ty))
        for (auto *group : tile->groups) {
          group->occludedNow = false;
          // Ground and road never fade. The loop that used to follow this one
          // propagated the fade to every flags==0 group whose XY bounds
          // overlapped an occluded one, believing those to be "roofs and
          // decorative trims" - so it dropped the street under and around
          // every fading building to twelve percent and showed the water plane
          // through the tarmac. The bake already keeps a building's roof with
          // its walls, so there is nothing to propagate to.
          if (!(group->flags & 1)) continue;
          constexpr float pad = .5f;
          if (group->maxx < segMinX - pad || group->minx > segMaxX + pad ||
              group->maxy < segMinY - pad || group->miny > segMaxY + pad ||
              group->maxz < segMinZ - pad || group->minz > segMaxZ + pad)
            continue;
          const float t = OcclusionHitT(*tile, *group, camWx, camWy, camWz,
                                        ox, oy, oz, false);
          if (t < nearestT) { nearestT = t; nearest = group; }
        }
  if (nearest) nearest->occludedNow = true;
  // Advance the dissolve on wall-clock time rather than once per capture.
  //
  // Captures come every frame while anything is moving but only four times a
  // second once the view settles, so a fixed per-capture step ran the
  // animation fifteen times slower when parked: a building took upwards of ten
  // seconds to clear while standing still, which is exactly when the player is
  // looking straight at it.
  static DWORD s_lastFadeTick = 0;
  const DWORD fadeTick = GetTickCount();
  float dt = s_lastFadeTick ? float(fadeTick - s_lastFadeTick) * .001f
                            : 1.f / 60.f;
  s_lastFadeTick = fadeTick;
  if (dt > .25f) dt = .25f; // a hitch or a tile load must not snap the fade
  // Rates chosen to reproduce the old per-frame constants at 60 Hz - 0.055 and
  // 0.09 a frame. Restoring is the quicker of the two, so a building that has
  // stopped blocking comes back promptly instead of lingering translucent.
  const float fadeOutStep = 1.f - expf(-3.4f * dt);
  const float fadeInStep  = 1.f - expf(-5.7f * dt);
  for (int y = miny; y <= maxy; y++)
    for (int x = minx; x <= maxx; x++)
      if (Tile *t = Load(x, y))
        for (auto *group : t->groups) {
          const float target = group->occludedNow ? .12f : 1.f;
          group->opacity += (target - group->opacity) *
                            (target < group->opacity ? fadeOutStep : fadeInStep);
          if (fabsf(target - group->opacity) < .002f) group->opacity = target;
        }

  auto drawGroup = [&](const Tile *t, const DrawGroup *group, bool count) {
    // A tile is much wider than this camera's view. Reject a whole group only
    // when all eight bounds corners lie outside the same frustum plane.
    // Testing just the centre would incorrectly remove long roads/buildings.
    // Legacy R3G1 has no group bounds, so keep its original draw behaviour.
    if (group->maxx > group->minx && group->maxy > group->miny) {
      unsigned outside = 63;
      for (unsigned corner = 0; corner < 8 && outside; ++corner) {
        const float gx = corner & 1 ? group->maxx : group->minx;
        const float gy = corner & 2 ? group->maxy : group->miny;
        const float gz = corner & 4 ? group->maxz : group->minz;
        const float vx = gx*w._11 + gy*w._21 + gz*w._31 + w._41;
        const float vy = gx*w._12 + gy*w._22 + gz*w._32 + w._42;
        const float vz = gx*w._13 + gy*w._23 + gz*w._33 + w._43;
        unsigned code = 0;
        if (vx*p._11 < -vz) code |= 1;
        if (vx*p._11 >  vz) code |= 2;
        if (vy*p._22 < -vz) code |= 4;
        if (vy*p._22 >  vz) code |= 8;
        if (vz < kNear) code |= 16;
        if (vz > kFar) code |= 32;
        outside &= code;
      }
      if (outside) return;
    }
    g_iso->SetIndices(group->ib);
    for (UINT start = 0; start < group->ni;) {
      UINT available = (group->ni - start) / 3,
           prims = maxPrims < available ? maxPrims : available;
      if (!prims)
        break;
      const HRESULT drawn=g_iso->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,t->nv,start,prims);
      if (SUCCEEDED(drawn) && count) draws++;
      if (FAILED(drawn)) {
        static DWORD lastError=0;
        const DWORD now=GetTickCount();
        if (now-lastError>=1000) {
          logfile::Line("radar3d: geometry draw failed 0x%08lx, bounds %.1f,%.1f..%.1f,%.1f vertices=%u",static_cast<unsigned long>(drawn),group->minx,group->miny,group->maxx,group->maxy,t->nv);
          lastError=now;
        }
      }
      start += prims * 3;
    }
  };

  // Pass one: everything still at full opacity, drawn and depth-written as
  // usual. TFACTOR's alpha carries the navigation colour grade and nothing
  // else now that the dissolve has a blend factor of its own.
  g_iso->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_iso->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_iso->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  g_iso->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  g_iso->SetRenderState(D3DRS_TEXTUREFACTOR, gradeFactor);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  bool anyFaded = false;
  for (int y = miny; y <= maxy; y++)
    for (int x = minx; x <= maxx; x++)
      if (Tile *t = Load(x, y)) {
        tilesLoaded++;
        bool bound = false;
        for (auto *group : t->groups) {
          if (group->opacity < .995f) { anyFaded = true; continue; }
          if (!bound) {
            g_iso->SetTexture(0, t->tex);
            g_iso->SetStreamSource(0, t->vb, 0, sizeof(Vertex));
            bound = true;
          }
          drawGroup(t, group, true);
        }
      }

  // The ribbon goes down before the blockers do, so a building dissolving over
  // it blends the route through itself rather than hiding it. The route is the
  // one thing on this display that has to stay readable whatever is standing
  // in front of it.
  DrawGpsRoute(routeGeom, ox, oy, oz, angle);

  if (anyFaded) {
    // Pass two: the blockers, each blended exactly once per pixel.
    //
    // Drawing these in line with everything else is the real reason a fading
    // building read as a pale mass rather than as glass. Culling is off and
    // depth writing was off, so every triangle blended independently - near
    // wall, far wall, roof, floor, interior detail. At twelve percent a piece,
    // eight layers leave 0.88^8, about a third, of the road behind it: a
    // building nominally at twelve percent opacity actually covered nearly two
    // thirds of what was behind it, and that stack of pale wall is what looked
    // white. Pinning TFACTOR's alpha fixed the tint but could not touch this.
    //
    // A depth prepass settles it. The first draw writes depth only, so the
    // nearest surface of the building wins the pixel; the second draws with
    // the depth test set to equal, so only that surface blends. One layer
    // whatever the building is made of, and twelve percent is then really
    // twelve percent.
    g_iso->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    SetWorldStages();
    for (int y = miny; y <= maxy; y++)
      for (int x = minx; x <= maxx; x++)
        if (Tile *t = Load(x, y)) {
          bool bound = false;
          for (auto *group : t->groups) {
            if (group->opacity >= .995f) continue;
            if (!bound) {
              g_iso->SetTexture(0, t->tex);
              g_iso->SetStreamSource(0, t->vb, 0, sizeof(Vertex));
              bound = true;
            }
            // Depth only: establish the near surface without touching colour.
            g_iso->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
            g_iso->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_iso->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            g_iso->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            drawGroup(t, group, false);
            // Colour, only where that surface actually landed.
            g_iso->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
            g_iso->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            g_iso->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
            g_iso->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            DWORD alpha = kGradeAlpha; // held steady while a group fades
            if (g_blendFactor) {
              const DWORD f = DWORD(group->opacity * 255.f);
              g_iso->SetRenderState(D3DRS_BLENDFACTOR,
                                    (f << 24) | (f << 16) | (f << 8) | f);
              g_iso->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR);
              g_iso->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR);
            } else {
              // No standalone blend factor on this device, so opacity has to
              // ride on TFACTOR's alpha again - which also drives the colour
              // grade, so the wall does pale as it goes. Nothing to be done
              // about that here; the prepass at least keeps it to one layer.
              alpha = DWORD(group->opacity * 255.f);
              g_iso->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
              g_iso->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            }
            g_iso->SetRenderState(D3DRS_TEXTUREFACTOR,
                                  (alpha << 24) | (gradeFactor & 0x00ffffff));
            drawGroup(t, group, true);
          }
        }
    g_iso->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_iso->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  }

  // Last, so the marker is never buried under the ribbon or a blocker.
  DrawDestMarker(w, oz);
  HRESULT end = g_iso->EndScene();
  g_lastTilesLoaded = tilesLoaded;
  g_lastDraws = static_cast<int>(draws);
  g_lastSceneWaterOnly = !draws && drewWater;
  // Nothing drawn at all is a broken render. Nothing drawn *but the sea* is
  // the open water, and it is what the panel should be showing there.
  //
  // draws only counts tile geometry, so a view with no land in it failed this
  // test, the capture was abandoned, and the frame fell back to GTA's round
  // radar - then a strip of coast came into range and the panel returned. That
  // alternation, frame by frame, is the flicker on a boat.
  if (!draws && !drewWater)
    return false;
  if (FAILED(end))
    return false;
  // Collapse the multisampled surface down to one value per pixel. StretchRect
  // between a multisampled source and a plain destination of the same size and
  // format is the resolve; it must use no filter, and it is the only way to get
  // at those samples, since the multisampled surface cannot be read back.
  if (g_rtMS &&
      FAILED(g_iso->StretchRect(g_rtMS, nullptr, g_rt, nullptr, D3DTEXF_NONE)))
    return false;
  return SUCCEEDED(g_iso->GetRenderTargetData(g_rt, g_read));
}
// Build the corner mark's texture from the mask compiled into the ASI.
// White throughout; only the alpha varies, which is all the mask carries.
bool EnsureLogo(IDirect3DDevice9 *d) {
  if (g_logoTex) return true;
  if (FAILED(d->CreateTexture(kRadarLogoSize, kRadarLogoSize, 1, 0,
                              D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_logoTex,
                              nullptr)))
    return false;
  D3DLOCKED_RECT r{};
  if (FAILED(g_logoTex->LockRect(0, &r, nullptr, 0))) {
    g_logoTex->Release();
    g_logoTex = nullptr;
    return false;
  }
  for (int y = 0; y < kRadarLogoSize; y++) {
    auto *row = reinterpret_cast<uint32_t *>(
        static_cast<uint8_t *>(r.pBits) + y * r.Pitch);
    for (int x = 0; x < kRadarLogoSize; x++)
      row[x] = (uint32_t(kRadarLogo[y * kRadarLogoSize + x]) << 24) | 0x00ffffff;
  }
  g_logoTex->UnlockRect(0);
  return true;
}

// Take what is behind the panel and blur it, by copying that piece of the
// frame into a small target. The stretch back out is done by whoever draws it.
bool FrostBehindPanel(IDirect3DDevice9 *d, int sw, int sh, float cx, float cy,
                      float hw, float hh) {
  if (!g_blurTex &&
      FAILED(d->CreateTexture(kBlurSize, kBlurSize, 1, D3DUSAGE_RENDERTARGET,
                              D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_blurTex,
                              nullptr)))
    return false;
  if (!g_blurSurf && FAILED(g_blurTex->GetSurfaceLevel(0, &g_blurSurf)))
    return false;

  IDirect3DSurface9 *back = nullptr;
  if (FAILED(d->GetRenderTarget(0, &back)) || !back) return false;

  // Clamped, because StretchRect will not take a rectangle that leaves the
  // surface and the panel sits against the bottom edge of the screen.
  RECT src{LONG(cx - hw), LONG(cy - hh), LONG(cx + hw), LONG(cy + hh)};
  if (src.left < 0) src.left = 0;
  if (src.top < 0) src.top = 0;
  if (src.right > sw) src.right = sw;
  if (src.bottom > sh) src.bottom = sh;

  bool ok = src.right > src.left && src.bottom > src.top &&
            SUCCEEDED(d->StretchRect(back, &src, g_blurSurf, nullptr,
                                     D3DTEXF_LINEAR));
  back->Release();
  return ok;
}

void DropPanelExtras() {
  if (g_blurSurf) g_blurSurf->Release();
  if (g_blurTex) g_blurTex->Release();
  g_blurSurf = nullptr;
  g_blurTex = nullptr;
}

bool Circle(IDirect3DDevice9 *d) {
  int sw = *(int *)kScreenW, sh = *(int *)kScreenH;
  if (sw <= 0 || sh <= 0 || !g_mainTex || !g_capValid)
    return false;
  float sx = sw / 640.f, sy = sh / 448.f,
        hw = radarbox::Width() * sx * .5f,
        hh = radarbox::Height() * sy * .5f,
        cx = radarbox::Left() * sx + hw,
        cy = sh - (radarbox::BottomMargin() + radarbox::Height() * .5f) * sy;
  // Direct UV: the perspective capture fills the HUD panel exactly.
  // No reprojection warp — each screen pixel maps linearly to its texture
  // coordinate. Every frame is a fresh capture when moving (CaptureMovingMs=0)
  // so there is no stale frame to warp around.
  struct V { float x, y, z, w, u, v; } fan[kFanVerts]{};
  auto uv = [&](float px, float py, float &u, float &v) {
    u = (px - (cx - hw)) / (2.f * hw);
    v = (py - (cy - hh)) / (2.f * hh);
  };
  float u, v;
  uv(cx, cy, u, v);
  fan[0] = {cx, cy, 0, 1, u, v};

  const float cr = (hw < hh ? hw : hh) * kCornerRadius;
  const float cornerX[4] = {cx + hw - cr, cx + hw - cr, cx - hw + cr,
                            cx - hw + cr};
  const float cornerY[4] = {cy - hh + cr, cy + hh - cr, cy + hh - cr,
                            cy - hh + cr};
  const float kHalfPi = 1.57079632679f;
  int written = 0;
  for (int corner = 0; corner < 4; corner++) {
    const float base = -kHalfPi + corner * kHalfPi;
    for (int step = 0; step <= kCornerSegments; step++) {
      const float a = base + kHalfPi * (float(step) / float(kCornerSegments));
      const float px = cornerX[corner] + cosf(a) * cr;
      const float py = cornerY[corner] + sinf(a) * cr;
      uv(px, py, u, v);
      fan[1 + written++] = {px, py, 0, 1, u, v};
    }
  }
  fan[1 + written] = fan[1];
  // A standalone test harness fed this exact function the exact bytes GTA
  // itself captured that session, from a device with no prior state, and it
  // drew perfectly - real buildings, real streets, not a flat colour. Run
  // in-game it still comes out flat, which only leaves state GTA had bound
  // that this did not explicitly clear. Two real gaps closed here:
  //
  //  - a bound vertex declaration silently wins over SetFVF in D3D9 if one
  //    is active; GTA/ProperRadar/other loaded D3D mods may well have one
  //    bound at this exact call site, which would have every vertex read
  //    through whatever layout it describes instead of ours;
  //  - only texture stage 1 was ever told to stop combining. Stages 2 and 3
  //    were never touched, so anything GTA left bound there - a lightmap,
  //    a detail texture - kept blending into this draw on top of ours.
  d->SetVertexDeclaration(nullptr);
  d->SetVertexShader(nullptr);
  d->SetPixelShader(nullptr);
  d->SetTexture(0, g_mainTex);
  d->SetTexture(1, nullptr);
  d->SetTexture(2, nullptr);
  d->SetTexture(3, nullptr);
  d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  DWORD inheritedCull=0,inheritedBlendOp=0;
  d->GetRenderState(D3DRS_CULLMODE,&inheritedCull);
  d->GetRenderState(D3DRS_BLENDOP,&inheritedBlendOp);
  d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
  d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);
  d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
  d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
  d->SetRenderState(D3DRS_FOGENABLE, FALSE);
  d->SetRenderState(D3DRS_ZENABLE, FALSE);
  d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  // The map panel is intentionally a little translucent, while GTA's native
  // blips are drawn afterwards and stay fully legible.
  d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  // Panel opacity. The map is meant to sit in the HUD rather than punch a
  // hole in it, so a little of the world still reads through it.
  d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xCCffffff);
  d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  // The remaining way a correct texture and correct UVs still sample as one
  // flat colour: the fixed-function pipeline never reading our UVs at all.
  // TEXCOORDINDEX can be left pointing at a different coordinate set, or at
  // a generated one (D3DTSS_TCI_CAMERASPACE*), and TEXTURETRANSFORMFLAGS can
  // be left enabled with a stale matrix that collapses every coordinate onto
  // a single texel. Neither is covered by SetFVF, by nulling shaders, or by
  // binding the texture - and neither existed on the clean device the test
  // harness used, which is exactly why it drew correctly there.
  d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  d->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
  d->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  d->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_DISABLE);
  d->SetTextureStageState(3, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  // The composite minifies by different amounts on each axis, so it wants
  // anisotropic filtering just as the scene does.
  //
  // The capture is square and the panel is wide: 1024 across into about 318
  // pixels is 3.2x, 1024 down into about 198 is 5.2x. A plain linear filter
  // with a mip chain has to choose one level for both, and it chooses from the
  // larger ratio - so horizontal detail was being blurred as though it were
  // minified 5.2x when it only needed 3.2x. That threw away a good part of
  // what rendering at 1024 and resolving 8x multisampling had just bought.
  if (!g_compositeAniso) {
    D3DCAPS9 mainCaps{};
    g_compositeAniso = 1;
    if (SUCCEEDED(d->GetDeviceCaps(&mainCaps)) &&
        (mainCaps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC)) {
      g_compositeAniso = mainCaps.MaxAnisotropy > 16 ? 16 : mainCaps.MaxAnisotropy;
      if (g_compositeAniso < 1) g_compositeAniso = 1;
    }
    logfile::Line("radar3d: composite sampler using %ux anisotropy",
                  unsigned(g_compositeAniso));
  }
  d->SetSamplerState(0, D3DSAMP_MINFILTER,
                     g_compositeAniso > 1 ? D3DTEXF_ANISOTROPIC
                                          : D3DTEXF_LINEAR);
  d->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, g_compositeAniso);
  d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  // Frost whatever is behind the panel, before the panel goes over it.
  //
  // The panel is ninety percent opaque, so a tenth of the game reads through
  // it - which at speed is a moving, contrasty distraction underneath a thing
  // meant to be read at a glance. Blurring that tenth keeps the depth without
  // the noise.
  //
  // It reuses the panel's own fan, so the frosting takes the rounded corners
  // exactly and needs no geometry of its own. The fan's UVs already run 0..1
  // across the panel box, which is precisely the region copied.
  if (FrostBehindPanel(d, sw, sh, cx, cy, hw, hh)) {
    d->SetTexture(0, g_blurTex);
    d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffffffff);
    d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kFanVerts - 2, fan, sizeof(V));
    d->SetTexture(0, g_mainTex);
    d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xCCffffff);
  }

  const HRESULT panelResult=d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kFanVerts - 2, fan, sizeof(V));
  const bool drewPanel = SUCCEEDED(panelResult);
  static DWORD lastCompositeReport=0;
  if(GetTickCount()-lastCompositeReport>5000) {
    lastCompositeReport=GetTickCount();
    D3DVIEWPORT9 viewport{};d->GetViewport(&viewport);
    logfile::Line("radar3d: composite hr=%08X inheritedCull=%u blendOp=%u viewport=%u,%u %ux%u texture=%p",
        static_cast<unsigned>(panelResult),static_cast<unsigned>(inheritedCull),static_cast<unsigned>(inheritedBlendOp),
        static_cast<unsigned>(viewport.X),static_cast<unsigned>(viewport.Y),static_cast<unsigned>(viewport.Width),static_cast<unsigned>(viewport.Height),static_cast<void*>(g_mainTex));
  }

  // What the status slot says has to follow what is actually on the panel.
  //
  // Holding geometry was the old test, and it is not the same question: the
  // ribbon is drawn from the matched point onward, so a route matched at its
  // own far end draws nothing, and the panel then showed no ribbon, no loading
  // dots and the idle mark - three ways of saying nothing is happening while a
  // destination was still set. g_routeOnScreen is set by the draw itself.
  bool routeShown = false, unreachable = false;
  EnterCriticalSection(&g_routeLock);
  routeShown = g_routeGeom != nullptr && g_routeOnScreen;
  unreachable = g_routeUnreachable;
  LeaveCriticalSection(&g_routeLock);
  // Nothing is being worked out when there is nothing that can be: no road
  // reaches the destination, or there is no road graph on this installation
  // and there never will be. A spinner that cannot resolve is worse than the
  // idle mark, and the destination dot still says which way to go.
  const bool canRoute = router::Ready() || router::Loading();
  const bool routeLoading =
      Dest().active && !routeShown && !unreachable && canRoute;

  // Loading owns the same quiet top-left slot as the idle logo and distance.
  // It does not dim the map: three small pulsing dots are enough to say that a
  // route is pending without turning the entire panel into a modal state.
  if (routeLoading && drewPanel) {
      struct DV { float x, y, z, w; uint32_t c; };
      d->SetTexture(0, nullptr);
      d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
      d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
      const DWORD phase = GetTickCount() / 250 % 3;
      const float inset = radarbox::Width() * sx * kLogoInsetFraction;
      const float dotR = 1.15f * sy;
      const float spacing = 4.0f * sx;
      const float slotX = cx - hw + inset + spacing;
      const float slotY = cy - hh + inset +
                          radarbox::Height() * sy * kLogoHeightFraction * 0.5f;
      for (int i = 0; i < 3; i++) {
        const uint32_t a = (phase == DWORD(i)) ? 185u : 55u;
        const uint32_t col = (a << 24) | 0x00ffffffu;
        const float px = slotX + (i - 1) * spacing;
        const DV dot[4] = {
            {px - dotR, slotY - dotR, 0.f, 1.f, col},
            {px + dotR, slotY - dotR, 0.f, 1.f, col},
            {px - dotR, slotY + dotR, 0.f, 1.f, col},
            {px + dotR, slotY + dotR, 0.f, 1.f, col},
        };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, dot, sizeof(DV));
      }
      // Restore for the border draw that follows.
      d->SetTexture(0, g_mainTex);
      d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
      d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xCCffffff);
      d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  }

  // The maker's mark, in the panel corner.
  //
  // Drawn after the map so it sits on top of it, and drawn from the texture's
  // alpha against a flat white so the shape is the only thing the texture
  // decides. kLogoAlpha keeps it at the threshold of visible - a moulding in
  // the bezel rather than a badge.
  if (!routeLoading && !routeShown && EnsureLogo(d)) {
    const float side = radarbox::Height() * sy * kLogoHeightFraction;
    const float inset = radarbox::Width() * sx * kLogoInsetFraction;
    const float lx = kLogoAtLeft ? (cx - hw + inset) : (cx + hw - inset - side);
    const float ly = kLogoAtTop ? (cy - hh + inset) : (cy + hh - inset - side);
    const V logo[4] = {
        {lx,        ly,        0.f, 1.f, 0.f, 0.f},
        {lx + side, ly,        0.f, 1.f, 1.f, 0.f},
        {lx,        ly + side, 0.f, 1.f, 0.f, 1.f},
        {lx + side, ly + side, 0.f, 1.f, 1.f, 1.f}};
    d->SetTexture(0, g_logoTex);
    d->SetRenderState(D3DRS_TEXTUREFACTOR,
                      (uint32_t(kLogoAlpha) << 24) | 0x00ffffff);
    // Colour from the factor, so the mark is flat white whatever the mask
    // holds; alpha from the mask scaled by the factor, so the shape is the
    // mask's and the strength is ours.
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, logo, sizeof(V));
    d->SetTexture(0, g_mainTex);
    d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xCCffffff);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  }

  // A hairline along the panel edge. Without it the map fades into whatever
  // happens to be behind it and the rounded corners lose their shape; with it
  // the panel reads as a deliberate piece of the HUD.
  //
  // The fan already walked this outline to build its perimeter, so the border
  // reuses those points rather than working the corners out a second time -
  // which also guarantees the two can never disagree about where the edge is.
  // fan[1] through fan[kFanVerts - 1] are that perimeter, already closed by the
  // repeat of the first point.
  struct BorderVert { float x, y, z, w; uint32_t colour; };
  BorderVert border[kFanVerts - 1]{};
  constexpr uint32_t kBorderColour = 0x70ffffffu;  // white, ~44%
  for (int i = 0; i < kFanVerts - 1; i++) {
    // Half a pixel over, so the line lands on pixel centres and stays one
    // pixel wide instead of smearing across two.
    border[i] = {fan[i + 1].x + .5f, fan[i + 1].y + .5f, 0.f, 1.f, kBorderColour};
  }
  d->SetTexture(0, nullptr);
  d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  // Smooth the border itself where the driver will do it. The rounded corners
  // are the one place on the panel where a hard stair-step would show, and
  // this costs nothing when it is unsupported - the state is simply ignored.
  // DrawBackground wraps this whole function in a D3DSBT_ALL state block, so
  // nothing set here escapes back into the game.
  d->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, TRUE);
  d->DrawPrimitiveUP(D3DPT_LINESTRIP, kFanVerts - 2, border, sizeof(BorderVert));
  d->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);

  // The destination dot last, over the border, so it rides the rim whole
  // instead of being cut in half by it.
  DrawDestMarkerHud(d, cx, cy, hw, hh, sy);

  return drewPanel;
}
bool Sample(float &x, float &y, float &z, float &range, float &angle) {
  const float *o = (const float *)kRadarOrigin;
  x = o[0];
  y = o[1];
  // FindPlayerPed has a player-id parameter; -1 is GTA's local-player
  // sentinel. Calling it without that argument left an undefined value in
  // the stack slot, so a valid render was rejected and the flat radar became
  // the fallback after the elevation-aware camera landed.
  auto *ped =
      reinterpret_cast<uint8_t *(__cdecl *)(int)>(kFindPlayerPed)(-1);
  if (!ped) {
    if (g_sampleRejectLogged++ < 3)
      logfile::Line("radar3d: local player not ready; retaining native radar until a safe sample exists");
    return false;
  }
  // This is an in-vehicle navigation display by default, not an always-on
  // pedestrian minimap - GTA's own round radar is what the game shows somebody
  // on foot, and rejecting the sample also prevents a previously captured
  // vehicle frame from being composited after the player gets out.
  //
  // ShowOnFoot in the ini keeps the panel up anyway. There is no vehicle to
  // take a heading from then, so the view follows the gameplay camera the way
  // the stock radar does - which is also the only thing that makes sense on
  // foot, where there is no direction of travel to be heading-up about.
  const bool onFoot = !radarcfg::Current().showOnFoot;
  float vehicleHeading{};
  const bool inVehicle = VehicleHeading(vehicleHeading);
  if (!inVehicle && onFoot)
    return false;
  const auto matrix = *reinterpret_cast<uintptr_t *>(ped + 0x14);
  z = matrix ? *reinterpret_cast<const float *>(matrix + 0x38)
             : *reinterpret_cast<const float *>(ped + 0x0C);
  range = radarbox::EffectiveDrivingRange(*(float *)kRadarRange);
  angle = *(float *)kRadarAngle;
  // On foot retain GTA's camera-relative behaviour. In a vehicle anchor the
  // view to the car itself, not north and not the freely orbiting camera.
  if (inVehicle) angle = vehicleHeading;
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         std::isfinite(range) &&
         range >= 40 && range <= 2000 && std::isfinite(angle);
}
void Update(IDirect3DDevice9 *main, float x, float y, float z, float range,
            float angle) {
  // Re-shape the capture whenever the panel does.
  //
  // The aspect depends on the screen resolution as well as on radarbox's own
  // constants, so it cannot be settled at compile time and it can change while
  // the game is running. When it does, every surface sized from it has to go:
  // the render targets and depth buffer live on the isolated device, and the
  // composite texture lives on the game's. Rare enough that dropping and
  // remaking them is the whole of the handling.
  {
    const float aspect = PanelPixelAspect();
    if (fabsf(aspect - g_panelAspect) > .01f * g_panelAspect || !g_iso) {
      UINT w = g_capW, h = g_capH;
      CaptureSizeFor(aspect, w, h);
      if (w != g_capW || h != g_capH || !g_iso) {
        if (g_iso) {
          logfile::Line("radar3d: panel aspect now %.3f - capture %ux%u",
                        aspect, w, h);
          DropIso();
        }
        if (g_mainTex) { g_mainTex->Release(); g_mainTex = nullptr; }
        g_capValid = false;
      }
      g_capW = w; g_capH = h;
      g_panelAspect = aspect;
    }
  }

  // Cheap: mostly returns having decided it is not time to look yet, and it
  // never searches on this thread. The waypoint is watched from the present
  // hook instead - see there for why it cannot be watched from here.
  RequestLocalRoute(x, y);

  // A full mip chain, not the single level this used to have.
  //
  // The capture is now shaped like the panel, so it minifies by the same
  // factor on both axes - about 3.9x at 1080p. A lone level sampled with a
  // bilinear tap reads a 2x2 footprint, which at four to one is four of the
  // sixteen texels that should contribute, so the scene's 8x multisampling was
  // resolved at full size and then mostly thrown away on the way down and fine
  // detail crawled as the world moved. With the chain in place that
  // minification lands on a properly filtered level and the oversized render
  // becomes real supersampling rather than wasted work.
  //
  // The reason mips are refused inside the scene does not apply here: that is
  // about atlas cells having barely a texel of guard between neighbours, and
  // this is one continuous image with nothing packed beside it.
  //
  // Still MANAGED, so a device reset costs nothing to recover from - which
  // is why the levels are filled below rather than left to AUTOGENMIPMAP,
  // since that wants the default pool.
  if (!g_mainTex &&
      FAILED(main->CreateTexture(g_capW, g_capH, 0, 0, D3DFMT_A8R8G8B8,
                                 D3DPOOL_MANAGED, &g_mainTex, nullptr)))
    return;
  DWORD now = GetTickCount();
  float dx = x - g_capX, dy = y - g_capY, da = fabsf(angle - g_capAngle);
  while (da > 3.14159265f)
    da = fabsf(da - 6.28318531f);
  bool moved = !g_capValid ||
               dx * dx + dy * dy >
                   (range * kMoveFraction) * (range * kMoveFraction) ||
               fabsf(z - g_capZ) > range * kMoveFraction ||
               da > kAngleTolerance ||
               fabsf(CaptureRange(range) - g_capRange) >
                   CaptureRange(range) * kRangeFraction;
  DWORD wait = moved ? MovingWaitMs() : SettledWaitMs();
  if (g_last && now - g_last < wait)
    return;
  g_last = now;
  const DWORD captureStart = GetTickCount();
  if (!RenderIso(x, y, z, CaptureRange(range), angle)) {
    g_capValid = false;
    return;
  }
  D3DLOCKED_RECT src{}, dst{};
  bool srcLocked = SUCCEEDED(g_read->LockRect(&src, nullptr, D3DLOCK_READONLY));
  UINT changed = 0, sampled = 0;
  uint8_t lo = 255, hi = 0;
  if (srcLocked) {
    for (UINT row = 0; row < g_capH; row += kValidateStride) {
      auto *p = (uint32_t *)((uint8_t *)src.pBits + row * src.Pitch);
      for (UINT col = 0; col < g_capW; col += kValidateStride) {
        uint32_t v = p[col];
        uint8_t b = v & 255, g = (v >> 8) & 255, r = (v >> 16) & 255,
                l = (uint8_t)((uint32_t(r) + g + b) / 3);
        if (l < lo)
          lo = l;
        if (l > hi)
          hi = l;
        sampled++;
        if ((v & 0x00ffffff) != (g_captureClearColor & 0x00ffffff))
          changed++;
      }
    }
  }
  // Same one-percent floor as before, against the number actually sampled
  // rather than the full pixel count, so striding does not quietly make the
  // test easier to pass.
  // The contrast floor is there to catch a capture that came back as one flat
  // colour because the render failed rather than because the world is flat.
  // The open sea is genuinely close to one flat colour - the water plane plus
  // whatever the fog does to it - so a boat far enough from land fails this on
  // a perfectly good picture and the panel drops back to the native radar for
  // that frame. Where the scene is known to be water and nothing else, the
  // coverage test against the clear colour is enough on its own.
  const bool waterOnly = g_lastSceneWaterOnly;
  bool useful = srcLocked && sampled && changed > sampled / 100 &&
                (waterOnly || hi > lo + 6);
  bool dstLocked =
      useful && SUCCEEDED(g_mainTex->LockRect(0, &dst, nullptr, 0));
  if (!dstLocked && g_updateLogged < 8) {
    // The capture rendered but was not adopted, so g_cap* keeps pointing at
    // wherever the last accepted one was taken - which is how a startup
    // capture at the world origin survives long after the player has spawned
    // seven kilometres away.
    g_updateLogged++;
    logfile::Line("radar3d: capture at %.1f,%.1f rejected - srcLocked=%d "
                  "coverage %u/%u luminance %u..%u useful=%d; still using "
                  "capture from %.1f,%.1f",
                  x, y, srcLocked ? 1 : 0, changed, sampled, lo, hi,
                  useful ? 1 : 0, g_capX, g_capY);
  }
  if (dstLocked) {
    for (UINT row = 0; row < g_capH; row++)
      memcpy((uint8_t *)dst.pBits + row * dst.Pitch,
             (uint8_t *)src.pBits + row * src.Pitch, g_capW * 4);
    g_mainTex->UnlockRect(0);

    // Build the rest of the chain, each level a 2x2 box average of the one
    // above. Every level after the first together comes to a third of the
    // first's pixels, so this is a fraction of the readback that produced it.
    const DWORD levels = g_mainTex->GetLevelCount();
    UINT w = g_capW, h = g_capH;
    for (DWORD level = 1; level < levels && (w > 1 || h > 1); level++) {
      const UINT pw = w, ph = h;
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
      D3DLOCKED_RECT up{}, dn{};
      if (FAILED(g_mainTex->LockRect(level - 1, &up, nullptr,
                                     D3DLOCK_READONLY)))
        break;
      if (FAILED(g_mainTex->LockRect(level, &dn, nullptr, 0))) {
        g_mainTex->UnlockRect(level - 1);
        break;
      }
      for (UINT row = 0; row < h; row++) {
        // Clamp so a level that is already one texel on an axis keeps
        // sampling that texel rather than reading off the end of the row.
        const UINT r0 = (row * 2 < ph) ? row * 2 : ph - 1;
        const UINT r1 = (row * 2 + 1 < ph) ? row * 2 + 1 : r0;
        const auto *a =
            (const uint32_t *)((const uint8_t *)up.pBits + r0 * up.Pitch);
        const auto *b =
            (const uint32_t *)((const uint8_t *)up.pBits + r1 * up.Pitch);
        auto *o = (uint32_t *)((uint8_t *)dn.pBits + row * dn.Pitch);
        for (UINT col = 0; col < w; col++) {
          const UINT c0 = (col * 2 < pw) ? col * 2 : pw - 1;
          const UINT c1 = (col * 2 + 1 < pw) ? col * 2 + 1 : c0;
          const uint32_t p[4] = {a[c0], a[c1], b[c0], b[c1]};
          uint32_t out = 0;
          // Each channel averaged on its own, alpha included.
          for (int shift = 0; shift < 32; shift += 8) {
            const uint32_t sum = ((p[0] >> shift) & 0xFFu) +
                                 ((p[1] >> shift) & 0xFFu) +
                                 ((p[2] >> shift) & 0xFFu) +
                                 ((p[3] >> shift) & 0xFFu);
            out |= ((sum + 2) / 4) << shift;
          }
          o[col] = out;
        }
      }
      g_mainTex->UnlockRect(level);
      g_mainTex->UnlockRect(level - 1);
    }
  }
  if (srcLocked)
    g_read->UnlockRect();
  if (dstLocked) {
    g_capX = x;
    g_capY = y;
    g_capZ = z;
    g_capRange = CaptureRange(range);
    g_capAngle = angle;
    g_capValid = true;
  } else {
    g_capValid = false;
  }
  static DWORD reportAt = 0, captureCount = 0, totalMs = 0, peakMs = 0;
  const DWORD elapsed = GetTickCount() - captureStart;
  ++captureCount;
  totalMs += elapsed;
  if (elapsed > peakMs) peakMs = elapsed;
  if (now - reportAt >= 5000) {
    logfile::Line("radar3d: capture %ux%u, %u samples in %ums, avg %.2fms peak %ums; tiles=%d draws=%d valid=%d moving=%d",
                  g_capW, g_capH, captureCount, now - reportAt,
                  float(totalMs) / float(captureCount), peakMs,
                  g_lastTilesLoaded, g_lastDraws, g_capValid ? 1 : 0, moved ? 1 : 0);
    logfile::Line("radar3d: view %.2f,%.2f,%.2f heading %.4f; match route=%u arc=%.1f cross=%.2f",
                  x, y, z, angle, g_match.routeId, g_match.s, g_match.cross);
    reportAt = now; captureCount = totalMs = peakMs = 0;
  }
}
// Present may stop on focus loss, or another HUD may bypass it entirely.
// Release default-pool resources at Reset itself, before GTA tries recovery.
HRESULT STDMETHODCALLTYPE OnReset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp) {
  if (d == *(IDirect3DDevice9 **)kMainDevice) {
    DropPanelExtras();
    DropIso();
    g_capValid = false;
    g_composited = 0;
  }
  return g_originalReset(d, pp);
}

HRESULT STDMETHODCALLTYPE OnPresent(IDirect3DDevice9 *d, const RECT *src,
                                    const RECT *dst, HWND wnd,
                                    const RGNDATA *dirty) {
  if (g_mapsOnly) {
    // Only the device the phone's map is drawn with.
    if (d == *(IDirect3DDevice9 **)kMainDevice) MakeIso();
    return g_originalPresent(d, src, dst, wnd, dirty);
  }
  // Deliberately no longer refreshes the capture - DrawBackground does that
  // now, from GTA's own radar draw. Driving it from here was what left the
  // capture frozen at the world origin, and feeding Update radar values
  // sampled at end-of-frame could also disagree with the values the
  // reprojection uses mid-frame, which shows up as a capture that is
  // immediately rejected for not covering the live view. The hook stays
  // installed for device creation and navigation; OnReset handles recovery.
  //
  // It does still create the isolated device, though, because here is
  // outside GTA's own BeginScene/EndScene pair - creating a second D3D9
  // device from inside the radar draw would be a needless risk when this
  // costs nothing and happens exactly once.
  if (d == *(IDirect3DDevice9 **)kMainDevice) {
    MakeIso();

    // Watch the map waypoint from here, every frame, rather than from the
    // radar's own update.
    //
    // The position is read from the map cursor at the moment the waypoint's
    // blip handle changes, and the radar does not draw while the pause map is
    // open - so watching from there meant looking some time after the fact,
    // by which point the cursor had usually moved on. Place a waypoint, drag
    // the cursor somewhere else, close the map, and the route went to the
    // second place rather than the marked one. Here the change is seen on the
    // frame it happens, with the cursor still on the mark.
    DrainRouteInputs();
    PollWaypoint();
    // Arrival is judged here rather than in the route search, so that walking
    // the last stretch to a destination still counts as reaching it.
    if (float px, py; PlayerXY(px, py))
      PollArrival(px, py);
    // The frosting target is the one thing we own in the default pool, and a
    // device that still holds one cannot be reset. Letting it live across an
    // alt-tab would not break the radar, it would break the game's recovery,
    // so it goes the moment the device is lost and is rebuilt on demand.
    if (d->TestCooperativeLevel() != D3D_OK)
      DropPanelExtras();
  }
  return g_originalPresent(d, src, dst, wnd, dirty);
}
// Take over IDirect3DDevice9::Present by writing the vtable slot.
//
// The slot is shared by every instance of the class, so a device reset - which
// keeps the object - keeps the hook, and there is nothing to reinstall. The
// original is chained to at the end, so another ASI that hooked the same slot
// before us still runs; one that hooks it after us runs outside us. Neither
// order breaks anything here, because this hook only reads state and drains a
// queue.
//
// It is never removed. An ASI is not unloaded while the game runs, and
// restoring the slot on unload would be the more dangerous of the two options
// anyway: whoever hooked it after us would have their pointer thrown away.
bool HookPresent() {
  auto *d = *(IDirect3DDevice9 **)kMainDevice;
  if (!d)
    return false;
  void **v = *(void ***)d;
  if (!v || !Executable(v[16]) || !Executable(v[17]))
    return false;
  g_originalPresent = (PresentFn)v[17];
  g_originalReset = (ResetFn)v[16];
  return Writable((uintptr_t)&v[16], 2 * sizeof(void *), [&] {
    v[16] = (void *)&OnReset;
    v[17] = (void *)&OnPresent;
  });
}
} // namespace
bool DrawBackground() {
  // Some HUD replacements bypass the main device's Present after loading.
  // Keep navigation alive on the verified radar draw path as well.
  DrainRouteInputs();
  PollWaypoint();
  if (float px, py; PlayerXY(px, py)) PollArrival(px, py);
  auto *main = *(IDirect3DDevice9 **)kMainDevice;
  float x, y, z, range, angle;
  if (!main || !Sample(x, y, z, range, angle))
    return false;

  // The refresh is driven from here rather than from the Present hook.
  //
  // Present was the original trigger, and in a real session it stopped
  // firing this after the very first capture: the log showed the capture
  // frozen at the world origin taken during load, while none of the
  // instrumented failure paths inside Update ever ran - so Update simply
  // was not being reached again once gameplay started.
  //
  // This call site is known-good instead of assumed-good: it is GTA's own
  // radar draw, the same one whose rejection messages proved it runs every
  // frame carrying live radar values. Refreshing here also removes a real
  // coherence bug, because the capture and the reprojection that consumes
  // it now read the radar origin, range and angle from the same instant
  // rather than from two different points in the frame.
  //
  // Update captures each moving frame and at 250 ms intervals once settled.
  // It only touches the isolated device plus a
  // managed texture, so it is safe to call from inside GTA's draw.
  Update(main, x, y, z, range, angle);
  if (!g_capValid)
    return false;
  IDirect3DStateBlock9 *s{};
  if (FAILED(main->CreateStateBlock(D3DSBT_ALL, &s)))
    return false;
  s->Capture();
  bool drawn = Circle(main);
  s->Apply();
  s->Release();
  if (drawn)
    g_composited = GetTickCount();
  return drawn;
}
// The phone's Maps app (valkyrie-phone): the same tiles seen from anywhere,
// straight down or leaning, into a texture of its own on the game's device.
//
// It borrows the isolated device and its render target between the radar's
// own captures - a corner of the target the size of the phone's map - and
// leaves both as the radar expects them: the viewport back to the whole
// capture, the transforms set afresh by every RenderIso. None of the radar's
// own state changes: no route matching, no camera boom, no fading, and tiles
// are retired before loading when neither current view needs them.
IDirect3DTexture9 *g_phoneTex{};
UINT g_phoneW = 0, g_phoneH = 0;
D3DMATRIX g_phoneW2V{}, g_phoneProj{};
bool g_phoneValid = false;
float g_phoneView[6]{};
bool g_phoneComplete = false;  // every tile in the last view was drawn
// How far out the phone's map reaches from its middle, and how long a frame
// may spend reading tiles from disk: each is a full-detail mesh with a
// texture of up to 2048 x 2048, so a zoomed-out view loads them a few at a
// time, the nearest first, over the frames that follow, never all at once.
constexpr float kPhoneReach = 1100.f;
constexpr int kPhoneLoadsPerFrame = 1;

IDirect3DTexture9 *RenderPhoneMap(const float *view, UINT w, UINT h) {
  auto *main = *(IDirect3DDevice9 **)kMainDevice;
  // The isolated device is made from the Present hook, never from here.
  if (!main || !g_iso || !g_rt || !g_read || g_iso->TestCooperativeLevel() != D3D_OK)
    return nullptr;
  if (w > g_capW) { h = h * g_capW / w; w = g_capW; }
  if (h > g_capH) { w = w * g_capH / h; h = g_capH; }
  if (w < 16 || h < 16) return nullptr;
  if (g_phoneTex && (w != g_phoneW || h != g_phoneH)) {
    g_phoneTex->Release();
    g_phoneTex = nullptr;
  }
  if (!g_phoneTex) {
    if (FAILED(main->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_phoneTex, nullptr)))
      return nullptr;
    g_phoneW = w;
    g_phoneH = h;
    g_phoneValid = false;
  }
  // Nothing moved and nothing is still loading: the last picture stands.
  if (g_phoneValid && g_phoneComplete && g_phoneKeepAt &&
      GetTickCount64() - g_phoneKeepAt <= 1000 &&
      !memcmp(view, g_phoneView, sizeof g_phoneView)) {
    g_phoneKeepAt = GetTickCount64();
    return g_phoneTex;
  }
  const float ox = view[0], oy = view[1], oz = view[2], angle = view[3];
  const float tilt = view[4] < 0.f ? 0.f : (view[4] > 1.1f ? 1.1f : view[4]);
  const float dist = view[5] < 20.f ? 20.f : (view[5] > 3000.f ? 3000.f : view[5]);
  const float s = sinf(angle), c = cosf(angle), ct = cosf(tilt), st = sinf(tilt);
  D3DMATRIX wv{};
  wv._11 = c;  wv._21 = s;
  wv._12 = -s * ct; wv._22 = c * ct; wv._32 = st;
  wv._13 = -s * st; wv._23 = c * st; wv._33 = -ct;
  wv._41 = -(c * ox + s * oy);
  wv._42 = ct * (s * ox - c * oy) - st * oz;
  wv._43 = st * (s * ox - c * oy) + ct * oz + dist;
  wv._44 = 1;
  const float nearZ = dist > 400.f ? dist * .02f : 1.f;
  const float farZ = dist * (1.f + 2.5f * st) + 400.f;
  const float aspect = float(w) / float(h);
  const float yScale = 1.f / tanf(kFovY * .5f);
  D3DMATRIX proj{};
  proj._11 = yScale / aspect;
  proj._22 = yScale;
  proj._33 = farZ / (farZ - nearZ);
  proj._34 = 1.f;
  proj._43 = -nearZ * farZ / (farZ - nearZ);
  D3DMATRIX ident = Mat();

  g_iso->SetRenderTarget(0, g_rtMS ? g_rtMS : g_rt);
  g_iso->SetDepthStencilSurface(g_depth);
  D3DVIEWPORT9 vp{0, 0, w, h, 0, 1};
  g_iso->SetViewport(&vp);
  g_iso->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffaecbd1, 1, 0);
  if (FAILED(g_iso->BeginScene())) {
    D3DVIEWPORT9 full{0, 0, g_capW, g_capH, 0, 1};
    g_iso->SetViewport(&full);
    return nullptr;
  }
  g_iso->SetTransform(D3DTS_WORLD, &wv);
  g_iso->SetTransform(D3DTS_VIEW, &ident);
  g_iso->SetTransform(D3DTS_PROJECTION, &proj);
  g_iso->SetVertexShader(nullptr);
  g_iso->SetPixelShader(nullptr);
  g_iso->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_iso->SetRenderState(D3DRS_FOGENABLE, FALSE);
  g_iso->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_iso->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_iso->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  g_iso->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_iso->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_iso->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
  // A day-lit map whatever the hour: no grade toward the horizon.
  g_iso->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffffffff);
  // The ground reaches well past the frame when the view leans.
  const float extent = dist * tanf(kFovY * .5f) * (aspect > 1.f ? aspect : 1.f) * (1.f + 3.f * st) + 64.f;
  DrawWaterPlane(ox, oy, extent);
  SetWorldStages();
  g_iso->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  g_iso->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  g_iso->SetSamplerState(0, D3DSAMP_MINFILTER, g_anisotropy > 1 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR);
  g_iso->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, g_anisotropy);
  g_iso->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  g_iso->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_iso->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
  D3DCAPS9 caps{};
  g_iso->GetDeviceCaps(&caps);
  const UINT maxPrims = caps.MaxPrimitiveCount ? caps.MaxPrimitiveCount : 65535;
  // Leaning, the far edge of the frame lies further off ahead than behind.
  const float ahead = extent * (1.f + st);
  const float fx = -s, fy = c;
  const float cx = ox + fx * (ahead - extent) * .5f, cy = oy + fy * (ahead - extent) * .5f;
  const float reach = std::min(extent + (ahead - extent) * .5f, kPhoneReach);
  const int minx = (int)floorf((cx - reach) / kTile), maxx = (int)floorf((cx + reach) / kTile);
  const int miny = (int)floorf((cy - reach) / kTile), maxy = (int)floorf((cy + reach) / kTile);
  g_phoneKeep[0] = minx;
  g_phoneKeep[1] = maxx;
  g_phoneKeep[2] = miny;
  g_phoneKeep[3] = maxy;
  g_phoneKeepAt = GetTickCount64();
  // Evict before allocation: a single detailed tile can occupy tens of MB.
  Retire(g_lastMinX, g_lastMaxX, g_lastMinY, g_lastMaxY);
  std::vector<std::pair<float, std::pair<int, int>>> order;
  for (int ty = miny; ty <= maxy; ty++)
    for (int tx = minx; tx <= maxx; tx++) {
      const float dx = (tx + .5f) * kTile - ox, dy = (ty + .5f) * kTile - oy;
      order.push_back({dx * dx + dy * dy, {tx, ty}});
    }
  std::sort(order.begin(), order.end());
  int loads = 0;
  bool complete = true;
  for (const auto &entry : order) {
      const int tx = entry.second.first, ty = entry.second.second;
      if (!g_tiles.count({tx, ty}) && loads++ >= kPhoneLoadsPerFrame) {
        complete = false;
        continue;
      }
      if (!g_tiles.count({tx, ty})) {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof memory;
        if (GlobalMemoryStatusEx(&memory) && memory.ullAvailVirtual < 256ull * 1024 * 1024) {
          complete = false;
          continue;
        }
      }
      Tile *t = Load(tx, ty);
      if (!t) { if (!g_tiles.count({tx, ty})) complete = false; continue; }
      g_iso->SetTexture(0, t->tex);
      g_iso->SetStreamSource(0, t->vb, 0, sizeof(Vertex));
      for (auto *group : t->groups) {
        if (group->maxx > group->minx && group->maxy > group->miny) {
          unsigned outside = 63;
          for (unsigned corner = 0; corner < 8 && outside; ++corner) {
            const float gx = corner & 1 ? group->maxx : group->minx;
            const float gy = corner & 2 ? group->maxy : group->miny;
            const float gz = corner & 4 ? group->maxz : group->minz;
            const float vx = gx * wv._11 + gy * wv._21 + gz * wv._31 + wv._41;
            const float vy = gx * wv._12 + gy * wv._22 + gz * wv._32 + wv._42;
            const float vz = gx * wv._13 + gy * wv._23 + gz * wv._33 + wv._43;
            unsigned code = 0;
            if (vx * proj._11 < -vz) code |= 1;
            if (vx * proj._11 > vz) code |= 2;
            if (vy * proj._22 < -vz) code |= 4;
            if (vy * proj._22 > vz) code |= 8;
            if (vz < nearZ) code |= 16;
            if (vz > farZ) code |= 32;
            outside &= code;
          }
          if (outside) continue;
        }
        g_iso->SetIndices(group->ib);
        for (UINT start = 0; start < group->ni;) {
          const UINT available = (group->ni - start) / 3, prims = maxPrims < available ? maxPrims : available;
          if (!prims) break;
          g_iso->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, t->nv, start, prims);
          start += prims * 3;
        }
      }
    }
  OneStage();
  const HRESULT end = g_iso->EndScene();
  bool ok = SUCCEEDED(end);
  if (ok && g_rtMS) {
    const RECT r{0, 0, LONG(w), LONG(h)};
    ok = SUCCEEDED(g_iso->StretchRect(g_rtMS, &r, g_rt, &r, D3DTEXF_NONE));
  }
  ok = ok && SUCCEEDED(g_iso->GetRenderTargetData(g_rt, g_read));
  D3DVIEWPORT9 full{0, 0, g_capW, g_capH, 0, 1};
  g_iso->SetViewport(&full);
  if (!ok) return nullptr;
  D3DLOCKED_RECT src{}, dst{};
  if (FAILED(g_read->LockRect(&src, nullptr, D3DLOCK_READONLY))) return nullptr;
  if (FAILED(g_phoneTex->LockRect(0, &dst, nullptr, 0))) {
    g_read->UnlockRect();
    return nullptr;
  }
  for (UINT row = 0; row < h; ++row) {
    memcpy((uint8_t *)dst.pBits + row * dst.Pitch, (const uint8_t *)src.pBits + row * src.Pitch, w * 4);
    // Opaque: the bake leaves alpha as the grade put it.
    auto *px = (uint32_t *)((uint8_t *)dst.pBits + row * dst.Pitch);
    for (UINT col = 0; col < w; ++col) px[col] |= 0xff000000u;
  }
  g_phoneTex->UnlockRect(0);
  g_read->UnlockRect();
  g_phoneW2V = wv;
  g_phoneProj = proj;
  g_phoneValid = true;
  memcpy(g_phoneView, view, sizeof g_phoneView);
  g_phoneComplete = complete;
  return g_phoneTex;
}

// Where a world point falls on the last phone map, 0..1 across and down.
bool ProjectPhoneMap(const float *world, float *out) {
  if (!g_phoneValid) return false;
  const D3DMATRIX &m = g_phoneW2V, &p = g_phoneProj;
  const float vx = world[0] * m._11 + world[1] * m._21 + world[2] * m._31 + m._41;
  const float vy = world[0] * m._12 + world[1] * m._22 + world[2] * m._32 + m._42;
  const float vz = world[0] * m._13 + world[1] * m._23 + world[2] * m._33 + m._43;
  if (vz <= .01f) return false;
  out[0] = .5f + .5f * vx * p._11 / vz;
  out[1] = .5f - .5f * vy * p._22 / vz;
  return true;
}

bool Init() {
  uint32_t p = *(uint32_t *)kVersionProbe;
  if (p != kHoodlum && p != kCompact && !game::Init())
    return false;
  char path[MAX_PATH]{};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (char *s = strrchr(path, '\\'))
    s[1] = 0;
  // No tiles, no radar.
  //
  // The panel is drawn from a baked tile set, and there is no version of this
  // that invents one. Standing down here rather than a few patches later is
  // the whole difference between an install without tiles keeping the game's
  // own round radar and being given a black rectangle where the radar was:
  // the shape, the transform, the perimeter and the blip filter are all
  // installed by main_radar3d.cpp on the strength of this returning true.
  //
  // That is also what makes this safe to drop into a stock San Andreas, or
  // any install that has not baked a tile set - it loads, finds nothing to
  // draw, says so in the log and leaves the game alone.
  g_root = std::string(path) + "Valkyrie-radar-tiles";
  if (GetFileAttributesA(g_root.c_str()) == INVALID_FILE_ATTRIBUTES)
    g_root = std::string(path) + "sprp-radar3d-tiles";
  const DWORD tiles = GetFileAttributesA(g_root.c_str());
  if (tiles == INVALID_FILE_ATTRIBUTES ||
      !(tiles & FILE_ATTRIBUTE_DIRECTORY)) {
    logfile::Line("radar3d: no tile set beside the game (looked for "
                  "Valkyrie-radar-tiles and sprp-radar3d-tiles) - standing "
                  "down, the game's own radar is left as it is");
    return false;
  }
  // Start pulling the road graph in behind us, so the GPS can work out its own
  // routes when there is no server to send them.
  router::Init(std::string(path));
  if (!HookPresent()) {
    logfile::Line("radar3d: presentation hook failed");
    return false;
  }
  logfile::Line("radar3d: isolated renderer refreshed by radar draw; "
                "navigation polled by radar/map and Present callbacks");
  g_navigationActive = true;
  return true;
}

bool InitForMaps() {
  char path[MAX_PATH]{};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (char *s = strrchr(path, '\\'))
    s[1] = 0;
  g_root = std::string(path) + "Valkyrie-radar-tiles";
  if (GetFileAttributesA(g_root.c_str()) == INVALID_FILE_ATTRIBUTES)
    g_root = std::string(path) + "sprp-radar3d-tiles";
  const DWORD tiles = GetFileAttributesA(g_root.c_str());
  if (tiles == INVALID_FILE_ATTRIBUTES || !(tiles & FILE_ATTRIBUTE_DIRECTORY)) {
    logfile::Line("maps: no tile set beside the game (Valkyrie-radar-tiles) - the Maps app has no map");
    return false;
  }
  g_mapsOnly = true;
  if (!HookPresent()) {
    logfile::Line("maps: presentation hook failed");
    return false;
  }
  logfile::Line("maps: tile set found; the phone's map draws from it");
  return true;
}
} // namespace radar3d

// The phone's Maps app. `view` is x, y, z of the point looked at, the
// heading (radians, 0 north), the lean from straight down (radians) and the
// camera's distance from the point. Returns a texture on the game's device,
// owned here and valid until the next call, or null when the radar cannot
// draw one this frame. Game thread only, from a draw.
extern "C" __declspec(dllexport) IDirect3DTexture9 *__cdecl ValkyrieRadarPhoneMap(const float *view, unsigned w,
                                                                                unsigned h) {
  return view ? radar3d::RenderPhoneMap(view, w, h) : nullptr;
}
// Where a world point (x, y, z) falls on that texture, 0..1 across and down.
extern "C" __declspec(dllexport) BOOL __cdecl ValkyrieRadarPhoneMapProject(const float *world, float *out) {
  return world && out && radar3d::ProjectPhoneMap(world, out) ? TRUE : FALSE;
}
extern "C" __declspec(dllexport) BOOL __cdecl SprpRadar3DDrawBackground() {
  return radar3d::DrawBackground() ? TRUE : FALSE;
}
extern "C" __declspec(dllexport) BOOL __cdecl ValkyrieRadarLuaLayout(float* bounds) {
  return game::LuaRadarLayout(bounds)?TRUE:FALSE;
}
extern "C" __declspec(dllexport) BOOL __cdecl ValkyrieRadarLuaDraw() {
  return game::LuaRadarBackground()?TRUE:FALSE;
}

// There is deliberately no exported waypoint setter any more.
//
// One existed, and nothing ever called it: game.cpp resolved the address and
// then never used it. What it did do was give the waypoint a second writer,
// racing PollWaypoint - which reads the same waypoint from CMenuManager, on
// the same schedule, without needing to be told. One owner of that state is
// correct; two was only ever a way for them to disagree.

// Called from sprp-overlay (via GetProcAddress) when a ROUTE 0 message arrives
// from blipd. `points` is an array of `count` floats: x, y, z triples tracing
// the road from the player to the waypoint, already placed on the correct side
// of the road by the server. `count` == 0 clears the route.
//
// Nothing is decided here, and no geometry is built here. The message is
// copied and queued, and the game thread applies it from the present hook, so
// arrival, waypoint polling and cancellation are all sequenced against each
// other on one thread rather than racing from two.
//
// The ribbon used to be built on this thread, which is what the comment here
// said for a long time after it stopped being true. It is built in
// ApplyGpsRoute now, on the game thread - and only in the one case that still
// needs it, an install with no road graph, where the server's own line is all
// there is to draw. A route arrives at most every couple of seconds, so the
// cost lands on one frame in a hundred and only where there is no alternative.
extern "C" __declspec(dllexport) void __cdecl SprpRadar3DSetGpsRoute(
    const float* points, int count) {
  if (count < 0 || count % 3 || (count && !points)) return;
  for (int i = 0; i < count; ++i)
    if (!std::isfinite(points[i])) return;
  std::vector<float> input;
  if (count) input.assign(points, points + count);
  std::lock_guard<std::mutex> guard(radar3d::g_routeInputLock);
  // Nobody may be draining this.
  //
  // The drain runs from the present hook, and the present hook is only
  // installed when the radar starts - which it does not do without a tile set
  // to draw. The export stays callable either way, so on an install with no
  // tiles the server's reroute timer files a route in here twice a second, for
  // as long as the session lasts, and not one of them is ever taken out again.
  // A cap makes that bounded: the newest destination is the only one that
  // matters, and the oldest is the one worth losing.
  constexpr size_t kMaxQueued = 8;
  if (radar3d::g_routeInputs.size() >= kMaxQueued)
    radar3d::g_routeInputs.erase(radar3d::g_routeInputs.begin());
  radar3d::g_routeInputs.push_back(std::move(input));
}

// Only the game thread changes navigation state. Network callers enqueue
// owned input so arrival, waypoint polling and cancellation cannot race.
static void ApplyGpsRoute(const float* points, int count) {
  // Three floats is one point, and one point is a destination - which, since
  // the server stopped sending routes and started sending destinations, is
  // the smallest complete message there is. Requiring two points meant a
  // gamemode sending exactly what the protocol now asks for had its
  // destination read as a cancellation.
  if (count >= 3 && points) {
    const float newDx = points[count - 3], newDy = points[count - 2];

    // Refuse a re-send of a destination this side has already finished with -
    // arrived at, or had its marker cleared by the player. The gamemode's
    // reroute timer fires twice a second and never learns about either, so
    // without this the destination the player just removed comes straight
    // back, and removing it looks like it did nothing.
    if (radar3d::IsSpent(radar3d::DestKind::Gps, newDx, newDy)) {
      // Still being asked for, so the guard stands. This is the whole of what
      // keeps a reroute timer that never learns the player arrived from
      // putting the ribbon back up a few seconds later.
      radar3d::TouchSpent(radar3d::DestKind::Gps);
      return;
    }
    // Anything else is a genuinely different destination, and asking for one
    // is what releases the guard.
    radar3d::g_spentActive[radar3d::SpentSlot(radar3d::DestKind::Gps)] = false;

    // A re-send of the destination already being navigated to must not
    // disturb what is on screen.
    //
    // The server reroutes on its own timer, every few seconds, and every one
    // of those landed here and reset g_routeGeom. That tore down a perfectly
    // good ribbon, raised the loading spinner, forced an immediate local
    // reroute - and, because SprpRadar3DHasGpsRoute went false for that
    // window, let another ASI's flat 2D route flash back onto the panel
    // (game.cpp only suppresses it while we say we have one). Nothing about
    // the destination had changed; that is the whole periodic flicker.
    //
    // So the teardown below happens only when the destination genuinely moves.
    // The route already drawn is still the right answer to the same question.
    EnterCriticalSection(&radar3d::g_routeLock);
    const radar3d::DestState current = radar3d::g_gpsDest;
    LeaveCriticalSection(&radar3d::g_routeLock);
    if (current.active && current.source == radar3d::DestSource::Server && fabsf(newDx - current.x) < radar3d::kSameDest &&
        fabsf(newDy - current.y) < radar3d::kSameDest) {
      return; // The local road search owns this destination, including retries.
    }

    // Take the destination, not the route.
    //
    // The server knows about destinations the game itself cannot express - a
    // job's drop-off, a property from the finder, a mission objective - and
    // that is the only thing needed from it. The road search is the same
    // search either way, and running it here means it can be redone as often
    // as we like, off a graph we already hold, with nothing to wait for.
    // The route it sent is used only as a fallback, for the case where our
    // own graph never loaded.
    EnterCriticalSection(&radar3d::g_routeLock);
    radar3d::g_gpsDest = {newDx, newDy, true, radar3d::DestSource::Server,
                          radar3d::DestKind::Gps};
    LeaveCriticalSection(&radar3d::g_routeLock);
    radar3d::Arbitrate();
    logfile::Line("radar3d: gps destination %.1f, %.1f from server", newDx,
                  newDy);

    // Never adopt an unverified server polyline as road geometry.
    return;
  }

  // ROUTE 0 is authoritative cancellation of the scripted destination.
  //
  // What it must not do is take the player's own pause-map waypoint with it.
  // It used to: it cleared the remembered waypoint outright, and because the
  // game's target blip handle never changed, PollWaypoint could never notice
  // that waypoint again - so finishing a job, or simply losing the server,
  // silently ended navigation to a waypoint the player had placed by hand and
  // left them to place it again. A disconnect and a CLEAR arrive here too,
  // which made that the ordinary case rather than the odd one.
  //
  // The waypoint is kept, then, but only when it is still genuinely there.
  // The gamemode can put its scripted checkpoint behind the same target blip,
  // and then the handle belongs to the checkpoint rather than to the player;
  // restoring the remembered coordinates in that case is what leaves a ghost
  // marker and ribbon behind. Comparing the live handle against the one the
  // waypoint was read from tells the two apart, and PollWaypoint puts it back
  // on the next frame when it survives.
  radar3d::g_spentActive[radar3d::SpentSlot(radar3d::DestKind::Gps)] = false;
  EnterCriticalSection(&radar3d::g_routeLock);
  const radar3d::DestSource gpsSource = radar3d::g_gpsDest.source;
  LeaveCriticalSection(&radar3d::g_routeLock);
  if (gpsSource != radar3d::DestSource::Server) return;
  const int32_t handle =
      *reinterpret_cast<const int32_t *>(radar3d::kTargetBlipIndex);
  const bool waypointSurvives = radar3d::g_waypointActive && handle &&
                                handle == radar3d::g_waypointHandle;
  if (!waypointSurvives) {
    radar3d::g_waypointActive = false;
    radar3d::g_waypointHandle = 0;
  }
  radar3d::g_lastBlipHandle = handle;
  radar3d::EndNavigation(radar3d::DestKind::Gps);
  logfile::Line("radar3d: gps destination cancelled; %s",
                waypointSurvives ? "the player's own waypoint stands"
                                 : "marker and route cleared");
}

// Called from sprp-overlay when a ROUTE 1 message arrives - the mission
// objective the gamemode is currently asking the player to reach.
//
// Same shape as SprpRadar3DSetGpsRoute and the same rules: x/y/z triples, the
// last of which is the objective, and a count of zero cancels. It is a
// separate export rather than a flag on the existing one so that an older
// overlay, which knows nothing about missions, keeps working unchanged - it
// simply never calls this and the panel behaves as it did.
extern "C" __declspec(dllexport) void __cdecl SprpRadar3DSetMissionRoute(
    const float* points, int count) {
  if (count < 0 || count % 3 || (count && !points)) return;
  for (int i = 0; i < count; ++i)
    if (!std::isfinite(points[i])) return;
  std::vector<float> input;
  if (count) input.assign(points, points + count);
  std::lock_guard<std::mutex> guard(radar3d::g_routeInputLock);
  constexpr size_t kMaxQueued = 8;
  if (radar3d::g_missionInputs.size() >= kMaxQueued)
    radar3d::g_missionInputs.erase(radar3d::g_missionInputs.begin());
  radar3d::g_missionInputs.push_back(std::move(input));
}

// The mission half of ApplyGpsRoute, on the game thread.
//
// Shorter than its GPS twin because most of what that one guards against does
// not arise here. There is no pause-map marker behind a mission objective to
// preserve, and no player-side way to clear one: an objective appears when the
// gamemode sets it and goes when the gamemode says so - on completion, on
// failure, on death, on disconnect - so a cancellation is always authoritative
// and never has to be second-guessed.
//
// The spent guard is still wanted. The gamemode re-sends the objective on the
// same reroute timer the GPS uses, so arriving at one has to refuse the next
// few re-sends or the yellow dot returns for an objective already reached,
// exactly as the pink one used to.
static void ApplyMissionRoute(const float* points, int count) {
  if (count >= 3 && points) {
    const float mx = points[count - 3], my = points[count - 2];

    if (radar3d::IsSpent(radar3d::DestKind::Mission, mx, my)) {
      radar3d::TouchSpent(radar3d::DestKind::Mission);
      return;
    }
    radar3d::g_spentActive[radar3d::SpentSlot(radar3d::DestKind::Mission)] = false;

    EnterCriticalSection(&radar3d::g_routeLock);
    const radar3d::DestState current = radar3d::g_missionDest;
    const bool same = current.active &&
                      fabsf(mx - current.x) < radar3d::kSameDest &&
                      fabsf(my - current.y) < radar3d::kSameDest;
    if (!same)
      radar3d::g_missionDest = {mx, my, true, radar3d::DestSource::Server,
                                radar3d::DestKind::Mission};
    LeaveCriticalSection(&radar3d::g_routeLock);
    if (same) return;  // a re-send of the objective already being drawn
    radar3d::Arbitrate();
    logfile::Line("radar3d: mission objective %.1f, %.1f from server", mx, my);
    return;
  }

  // The objective is over, however it ended. The ribbon goes back to the
  // player's own GPS destination if they still have one, which Arbitrate does,
  // and the spent guard is dropped so the same objective can be set again on a
  // retry of the same mission.
  EnterCriticalSection(&radar3d::g_routeLock);
  // Only the server's own objective is cancelled here. A single-player
  // objective read off the radar draw is not the server's to end - and on an
  // install with no server this path is never reached at all.
  const bool had = radar3d::g_missionDest.active &&
                   radar3d::g_missionDest.source == radar3d::DestSource::Server;
  LeaveCriticalSection(&radar3d::g_routeLock);
  radar3d::g_spentActive[radar3d::SpentSlot(radar3d::DestKind::Mission)] = false;
  if (!had) return;
  radar3d::EndNavigation(radar3d::DestKind::Mission);
  logfile::Line("radar3d: mission objective cleared");
}

// Whether this display is handling navigation. game.cpp asks before it
// suppresses another ASI's flat 2D route: with nothing to put in its place,
// suppressing it would leave the player with no route at all.
//
// An active destination counts even while the ribbon for it is still being
// worked out. Answering strictly on whether geometry exists meant that every
// gap - a new destination, a reroute - un-suppressed another ASI's flat route for a
// few frames, so the old-style line flashed across the panel before ours
// appeared. The panel already says what is happening during that gap: it dims
// and shows the loading dots. Letting the flat route punch through it as well
// is the worse of the two, and it is the one the player reads as a bug.
// How far is left to drive, in world units, or a negative value when there is
// no route to measure.
//
// The distance readout itself is drawn by Valkyrie Radar's game-integration
// layer, not by this isolated renderer. Text on this
// panel was tried from this ASI once, straight onto the game's device with
// CFont, and it corrupted GTA's render state badly enough to flash the whole
// screen in random colours. The integration layer owns a font path set up
// from scratch every call and flushed where the game expects, so the number
// crosses over and the drawing stays where it works.
extern "C" __declspec(dllexport) float __cdecl SprpRadar3DGetRouteRemaining() {
  EnterCriticalSection(&radar3d::g_routeLock);
  auto geom = radar3d::g_routeGeom;
  LeaveCriticalSection(&radar3d::g_routeLock);
  if (!geom) return -1.f;
  // The matcher knows how much of the route is already behind us. Without a
  // match on this particular route the whole length is the honest answer.
  const float done = (radar3d::g_match.valid &&
                      radar3d::g_match.routeId == geom->id)
                         ? radar3d::g_match.s
                         : 0.f;
  // The stretch the roads do not cover counts too. A route that stops short of
  // a jetty is still a trip to the jetty, and a readout that ends where the
  // tarmac does would say the journey is over while it is not.
  const float left = geom->totalArc - done + geom->endGap;
  return left > 0.f ? left : 0.f;
}

// Where a world position belongs on the panel, as fractions across and down,
// pinned to the rim when it is not in view.
//
// This is what puts the game's own map icons in the same place the panel's own
// markers go. They used to be placed by GTA's flat top-down radar transform,
// which is a different projection from the perspective scene drawn underneath
// them: it agrees roughly for things ahead and not at all for anything behind,
// so a shop the player had driven past was drawn in the middle of the panel
// over ground it was nowhere near, and no edge rule could have moved it,
// because by that transform's reckoning it was not near an edge.
extern "C" __declspec(dllexport) BOOL __cdecl SprpRadar3DProjectWorld(
    float worldX, float worldY, float* u, float* v) {
  if (!u || !v || !std::isfinite(worldX) || !std::isfinite(worldY))
    return FALSE;
  float pu = 0.f, pv = 0.f;
  if (!radar3d::ProjectWorldToPanel(worldX, worldY, pu, pv)) return FALSE;
  *u = pu;
  *v = pv;
  return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl SprpRadar3DTickNavigation() {
  radarcfg::Current();  // re-reads valkyrie-radar.ini when it has been edited
  radar3d::PollSettings();
  radar3d::DrainRouteInputs();
  radar3d::PollObjectiveMarker();
  radar3d::PollWaypoint();
  if (float px, py; radar3d::PlayerXY(px, py)) radar3d::PollArrival(px, py);
}

extern "C" __declspec(dllexport) BOOL __cdecl SprpRadar3DOwnsNavigation() {
  return radar3d::g_navigationActive ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL __cdecl SprpRadar3DHasGpsRoute() {
  EnterCriticalSection(&radar3d::g_routeLock);
  const bool has = radar3d::g_routeGeom != nullptr;
  const bool wanted = radar3d::g_dest.active;
  LeaveCriticalSection(&radar3d::g_routeLock);
  if (has) return TRUE;

  // A destination with no ribbon yet, or with no ribbon coming at all, still
  // counts. This was briefly answered the other way - hand the route back to
  // the game when no road can reach the destination, so the player is not left
  // with nothing - and what that produced on screen settles the question. The
  // game's route is drawn flat, north-up, in radar space, and game.cpp only
  // clips it to the box; laid over a perspective panel that turns with the car
  // it does not follow any road on it and reads as a magenta smear across the
  // scene. It is not a worse route, it is not a route at all.
  //
  // The panel is not left with nothing either way. The destination dot stands
  // whether or not a ribbon can be drawn, pinned to the panel edge and
  // pointing at the place, which is the honest answer to a destination the
  // road network cannot reach.
  return wanted ? TRUE : FALSE;
}
