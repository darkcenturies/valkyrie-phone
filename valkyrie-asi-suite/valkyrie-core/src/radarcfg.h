// Player-editable settings for the 3D radar, read from valkyrie-radar.ini.
//
// Everything here has a working default, so the file is optional: an install
// with no ini looks exactly as it did before the file existed. A missing key,
// an unparseable value or an out-of-range number falls back to that default
// key by key, not file by file - one bad line must not cost the player every
// other setting they set.
//
// The file is re-read when its modification time changes, so a player can edit
// colours with the game running and see them on the next frame. Values are
// published as plain scalars behind an atomic generation counter: the render
// thread reads them without a lock, and the worst a torn read can produce is
// one frame drawn with a mix of the old and new numbers.
#pragma once
#include <cstdint>

namespace radarcfg {

struct Settings {
  // Colours, ARGB. The alpha is used: a marker at 80 is half transparent.
  uint32_t gpsMarker;         // the pink destination dot
  uint32_t gpsMarkerOutline;  // the stroke around it - black, like the game's
  uint32_t gpsRibbon;         // the ribbon fill leading to it
  uint32_t missionMarker;     // the yellow objective dot
  uint32_t missionMarkerOutline;
  uint32_t missionRibbon;
  uint32_t ribbonOutline;     // the thin border on both ribbons

  // Ribbon half-widths in world units. The outline has to be the wider of the
  // two or it does not show around the fill.
  float ribbonWidth;
  float ribbonOutlineWidth;

  // Destination dot radius, in HUD-Y units. 3.5 is the N compass's own, which
  // is what makes the two markers on the panel rim read as a pair.
  float markerSize;

  // The stroke around the destination dot, in SCREEN PIXELS.
  //
  // Pixels rather than HUD units, because the thing it has to match - the thin
  // black border the game draws around its own markers - is a hairline on
  // screen, and a HUD unit is about two and a half pixels at 1080p. Specified
  // in HUD units a setting of 2 drew a ring thicker than half the dot.
  //
  // It is a width, not a share of the radius, so it stays the same weight
  // whatever MarkerSize is set to - and it is drawn in its own colour, so
  // recolouring the dot never takes its outline with it.
  float markerOutlineWidth;

  // How long the radar may go without taking a fresh 3D capture while the
  // player is moving, and while they are standing still. Raising the moving
  // figure trades panel smoothness for frame time on a slower machine.
  uint32_t captureMovingMs;
  uint32_t captureSettledMs;

  // How much of a surface's own colour survives the distance grade, 0-255.
  // Lower washes the panel out toward the horizon colour sooner.
  uint32_t navigationGrade;

  // Whether the game's own map icons - shops, properties, jobs, mission
  // markers, everything the pause map's legend lists - are drawn on the
  // driving panel.
  //
  // On by default. The panel used to filter every sprite except the north
  // compass, which made it a clean navigation display and also meant a player
  // could drive past a shop, a job or a mission marker with the panel showing
  // no sign of it - the icon was on the pause map and nowhere else.
  //
  // The player's own waypoint is still left out whichever way this is set:
  // the radar draws its own destination dot, and the stock diamond on top of
  // it is the same marker twice.
  bool showMapIcons;

  // Whether those same icons fade out with distance rather than staying at
  // full strength all the way to the edge of the panel - close to invisible
  // at maximum radar range, fully visible once you are near them, the way a
  // real GPS unit dims what is far away rather than cluttering the screen
  // with it.
  //
  // The active mission's objective marker is not part of this, and needs no
  // exception to stay out of it: it carries no sprite, so it is drawn by
  // ShowRadarTraceWithHeight rather than through the sprite path this rides
  // on, and keeps the distance fade CRadar's own CalculateBlipAlpha gives it.
  bool fadeMapIcons;

  // How dim an icon gets at maximum range, 0-255.
  //
  // Not as low as it looks like it could go. An icon outside the panel is
  // pinned to the rim, and being outside the panel is exactly what puts it at
  // maximum range - so this is the alpha that most wrapped icons are drawn at,
  // for as long as they are wrapped. Set it near zero and they are still being
  // drawn on the edge, just invisibly, which looks identical to the wrap not
  // working at all.
  //
  // Low enough to read as far away, high enough to read at all.
  uint32_t iconFadeMinAlpha;

  // How far inside the panel edge a pinned marker is held, in HUD units.
  //
  // Zero, so the marker's centre lands on the boundary itself and the edge
  // passes through the middle of it - which is what the north compass and the
  // destination dot both do, and the only thing that makes a pinned marker
  // read as riding the rim rather than as floating near it.
  //
  // Anything above zero tucks it wholly inside instead. DrawDestMarker learned
  // this once already and says so where it places the dot: pull the marker's
  // own radius off the limits and it sits a full marker's width in from the
  // edge. Raise it only if something is being clipped badly enough to be worth
  // that.
  float iconEdgeInset;

  // How small a faded icon is drawn at maximum range, as a share of its
  // normal size. 1 keeps every icon the size the game draws it; lower makes a
  // distant icon small as well as faint, so it reads as far away rather than
  // as clutter, and it grows back to full size as it is approached.
  //
  // The size has to be rewritten at the moment of the draw. CRadar hardcodes
  // an icon's half-extent inside DrawRadarSprite, so no position transform can
  // change it - see ReshapeBlipSpriteRect in game.cpp, which is the same trick
  // the north compass is already resized with.
  float iconMinScale;

  // How far out an icon still rides the panel edge, in world units.
  //
  // An icon outside the panel is pinned to the rim pointing at where it is,
  // the way the north compass and the destination dot do. Most icons are
  // short-range blips, which the game drops entirely once they are out of
  // range - CRadar::DrawCoordBlip culls them before working out a position at
  // all - so riding the rim means surviving that cull, which is what the value
  // LimitRadarPoint returns decides. See LimitRadarPointToBoxHelper.
  //
  // It has to be bounded. Keeping every short-range blip alive would put every
  // shop in San Andreas on the rim at once, which is the "ruinous for four
  // hundred" this suite already learned about once. Inside this radius an icon
  // wraps; beyond it the game's own cull stands and it drops off the panel.
  // Zero turns wrapping off and leaves the game's behaviour alone.
  float iconWrapRange;

  // Whether the distance to the destination is shown in kilometres and metres
  // rather than miles and feet. GTA's world units are metres, so the metric
  // reading is the direct one and the imperial one is the conversion.
  bool metricDistance;

  // Whether the panel is drawn on foot as well as in a vehicle.
  //
  // Off by default, which is what it has always done: this is a driving
  // display, and GTA's own round radar is what the game shows a pedestrian.
  // On foot the view follows the gameplay camera rather than a vehicle's
  // heading, because there is no vehicle to take a heading from.
  bool showOnFoot;

  // The panel itself, in GTA's HUD units - x against 640, y against 448.
  // These are what every other part of the suite projects blips, the route
  // line and the north marker into, so they are read from here by all of it
  // rather than compiled into each ASI separately. See radarbox.h.
  float panelWidth, panelHeight, panelLeft, panelBottomMargin;

  // How many pixels the 3D capture is allowed, as the side of a square: 512
  // means a 512x512 budget, spread over the panel's aspect. Higher is a
  // sharper panel and more work per frame; it is the one setting here that
  // costs real performance.
  uint32_t captureResolution;
};

// The current settings. Cheap enough to call per frame; it re-reads the file
// at most once a second and only when its timestamp has moved.
const Settings &Current();

// Bumped every time the file is re-read with different values in it. Anything
// holding geometry built from these numbers - the ribbon bakes its colour and
// width into its vertices - compares this against what it built with and
// rebuilds when they differ.
uint32_t Generation();

// Reads the file once and writes what was found to the log. Called from Init
// so a player who mistypes a colour has something to look at.
void Load();

}  // namespace radarcfg
