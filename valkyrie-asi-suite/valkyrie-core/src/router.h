#pragma once

#include <string>
#include <vector>

// Road routing on the client, over the same graph the server routes with.
//
// The GPS route used to arrive only from the server, which meant no route at
// all in single player. The search that produced it is not server-specific in
// any way, so it runs here too, over the same graph file and the same code.
namespace router {

// Begin loading the road graph embedded at the end of valkyrie-radar.asi.
// Development builds retain the old loose-file fallback. Returns at once; the
// load itself runs on its own thread because it is several megabytes of nodes
// followed by a connected-component pass. gameDir must end in a backslash.
void Init(const std::string &gameDir);

// Whether the graph has finished loading and routes can be asked for.
bool Ready();

// Whether a graph is still expected. False once the load has finished and
// failed - no graph file, or an unreadable one - which is the only case where
// waiting for a local route is waiting for something that will never come.
// The GPS uses this to stop claiming it is handling navigation on an install
// with no road graph, so the game's own route is left alone instead of being
// suppressed in favour of a spinner that never resolves.
bool Loading();

// A* from (sx, sy) to (ex, ey). On success `out` holds x/y/z triples in
// travel order, already offset into the correct lane, which is exactly the
// form the server sends and BuildRouteGeometry expects. False if the graph is
// not loaded or no path exists.
//
// The roads do not have to go all the way there. A destination off the network
// - a jetty, an island, a spot in the middle of a field - routes to the
// reachable road that ends up nearest it, which is what the pause map's own
// GPS does and what a car GPS does: it takes you as far as roads go and leaves
// the last stretch to you. endGap, when given, is how far short of the
// destination the route stops, so the caller can decide whether a route that
// stops short is still taking the player there.
//
// The player's own end keeps the old contract. Best effort there would start
// the ribbon at whatever road was nearest however far away that was, and the
// lead-in bends the first sixty units of ribbon onto the car - so a start
// hundreds of units off would draw a spike across the panel rather than a
// road. Off the network at this end is still simply no route yet.
bool Route(float sx, float sy, float ex, float ey, std::vector<float> &out,
           float *endGap = nullptr);

} // namespace router
