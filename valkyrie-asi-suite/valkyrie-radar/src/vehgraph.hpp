// San Andreas' road network, server side.
//
// The companion to pedgraph.hpp, and it exists for the same reason: the game
// does not work out where a car may drive while it is driving, it follows a
// network it loaded. This is that network, 147,175 road nodes covering Project
// Eagle's whole world, built by deploy/build-vehicle-graph.py.
//
// A road node carries more than a position. Cars have to keep to a side and
// obey junctions, so each node also holds the direction the road runs, how many
// lanes go each way, and which of the two traffic light groups controls it.
//
// No open.mp SDK here, on purpose, so it can be exercised from the tests.

#ifndef SPRP_AI_VEHGRAPH_HPP
#define SPRP_AI_VEHGRAPH_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace sprp_ai
{
struct VehNode
{
	float x = 0.f, y = 0.f, z = 0.f;
	uint32_t linkStart = 0;
	uint16_t linkCount = 0;
	uint16_t flags = 0;
	float dirX = 0.f, dirY = 0.f, width = 0.f;
	uint8_t lanesWith = 0, lanesAgainst = 0;
	uint8_t lightGroup = 0, lightFacing = 0;
};

constexpr uint16_t VehNodeDeadEnd = 1;
constexpr uint16_t VehNodeWater = 2;
constexpr uint16_t VehNodeHighway = 4;

// eTrafficLightsState, TrafficLights.h
constexpr int LightGreen = 0;
constexpr int LightYellow = 1;
constexpr int LightRed = 2;

// eTrafficLightsDirection, TrafficLights.h
constexpr uint8_t LightGroupNorthSouth = 1;
constexpr uint8_t LightGroupWestEast = 2;

// CTrafficLights::LightForCars1 and LightForCars2, TrafficLights.cpp. The whole
// cycle is a function of the clock and nothing else, so the server reproduces
// it exactly rather than inventing a signal of its own.
inline int trafficLightForGroup(uint8_t group, uint32_t timeMs)
{
	const uint32_t t = (timeMs / 2u) & 16383u;
	if (group == LightGroupNorthSouth)
	{
		if (t < 5000u) return LightGreen;
		if (t < 6000u) return LightYellow;
		return LightRed;
	}
	if (group == LightGroupWestEast)
	{
		if (t < 6000u) return LightRed;
		if (t < 11000u) return LightGreen;
		if (t < 12000u) return LightYellow;
		return LightRed;
	}
	return LightGreen; // no light controls this node
}

class VehGraph
{
public:
	static constexpr float CellSize = 100.f;

	bool loaded() const { return !nodes_.empty(); }
	size_t size() const { return nodes_.size(); }
	const VehNode* node(int index) const
	{
		return index >= 0 && static_cast<size_t>(index) < nodes_.size() ? &nodes_[index] : nullptr;
	}

	// Node count, or negative: -1 unreadable, -2 bad magic, -3 truncated.
	int load(const std::string& path)
	{
		std::FILE* file = std::fopen(path.c_str(), "rb");
		if (!file) return -1;
		std::fseek(file, 0, SEEK_END);
		const long length = std::ftell(file);
		std::rewind(file);
		if (length <= 0) { std::fclose(file); return -2; }
		std::vector<uint8_t> bytes(static_cast<size_t>(length));
		const bool read = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
		std::fclose(file);
		return read ? load(bytes.data(), bytes.size()) : -3;
	}

	int load(const void* source, size_t size)
	{
		const auto* cursor = static_cast<const uint8_t*>(source);
		const auto* end = cursor ? cursor + size : nullptr;
		auto take = [&](void* out, size_t count) {
			if (!cursor || count > static_cast<size_t>(end - cursor)) return false;
			std::memcpy(out, cursor, count); cursor += count; return true;
		};
		char magic[4]{};
		uint32_t version = 0, nodeCount = 0, linkCount = 0;
		if (!take(magic, 4) || !take(&version, 4) || !take(&nodeCount, 4) ||
			!take(&linkCount, 4) || std::memcmp(magic, "SPVG", 4) != 0 || version != 1)
			return -2;
		constexpr size_t nodeBytes = 36;
		if (size < 16 || nodeCount > (size - 16) / nodeBytes ||
			linkCount > (size - 16 - static_cast<size_t>(nodeCount) * nodeBytes) / 4)
			return -3;
		std::vector<VehNode> nodes(nodeCount);
		std::vector<uint32_t> links(linkCount);
		for (uint32_t i = 0; i < nodeCount; ++i) {
			VehNode& n = nodes[i];
			if (!take(&n.x,4)||!take(&n.y,4)||!take(&n.z,4)||!take(&n.linkStart,4)||
				!take(&n.linkCount,2)||!take(&n.flags,2)||!take(&n.dirX,4)||
				!take(&n.dirY,4)||!take(&n.width,4)||!take(&n.lanesWith,1)||
				!take(&n.lanesAgainst,1)||!take(&n.lightGroup,1)||!take(&n.lightFacing,1)) return -3;
		}
		if (linkCount && !take(links.data(), static_cast<size_t>(linkCount) * 4)) return -3;

		nodes_ = std::move(nodes);
		links_ = std::move(links);
		buildIndex();
		buildComponents();
		return static_cast<int>(nodes_.size());
	}

	// The game's own lane placement, from CCarCtrl at 0x422760 in the 1.0 US
	// executable (docs/reverse-engineering/generated/gta-sa-1.0-us). The
	// constants are read straight out of that binary's .rdata:
	//
	//   0x858c50 = 5.4   lane width
	//   0x858c24 = 0.3   centre-line bias
	//   0x858b8c = 0.5   one-way halving
	//   0x858c58 = 0.01  direction scale, so dirX/dirY are stored times 100
	//
	// 0x858eec is 0.0115741, which is 1/16/5.4 exactly, the number written in
	// CCarPathLink::OneWayLaneOffset's own comment - that is what confirms the
	// read. The decompiled function indexes the link with stride 0xE and takes
	// direction from +8/+9 and lanes from +0xB, matching the data.
	static constexpr float LaneWidth = 5.4f;
	// 0x858eec, and 1/16/5.4 exactly.
	static constexpr float TwoWayWidthScale = 0.0115741f;
	// 0x858b8c.
	static constexpr float LaneHalf = 0.5f;
	// build-vehicle-graph.py's WIDTH_SCALE, undone so the game's own scale can
	// be applied to the raw byte it expects.
	static constexpr float WidthScale = 16.0f;

	// CCarPathLink's own lane offset, 0x44DB00 in the 1.0 US executable, read
	// from the decompilation. This is the function CCarCtrl calls at 0x434900
	// when it works out where on the road to aim, and it is the two-way answer
	// that used to be missing here:
	//
	//     lanesAgainst == 0 :  0.5 - lanesWith    * 0.5
	//     lanesWith    == 0 :  0.5 - lanesAgainst * 0.5
	//     otherwise         :  width * 0.0115741 + 0.5
	//
	// The link packs its lane counts into one byte, low three bits against the
	// stored direction and the next three with it, which is the order
	// deploy/build-vehicle-graph.py reads them in.
	//
	// The two-way branch keeps its 0.5 outside the width term, so a road whose
	// stored width is zero - which is every road Eagle exports - still puts the
	// car half a lane, 2.7 metres, onto its own side rather than on the centre
	// line. That was the thing blocking traffic.
	//
	// The 0.3 centre-line bias that used to be applied here belongs to 0x422760,
	// which is a different caller, and is not part of this calculation.
	float laneCount(const VehNode& n) const
	{
		if (!n.lanesAgainst) return LaneHalf - static_cast<float>(n.lanesWith) * LaneHalf;
		if (!n.lanesWith) return LaneHalf - static_cast<float>(n.lanesAgainst) * LaneHalf;
		// build-vehicle-graph.py already divides the link's width byte by 16,
		// and the game's 0.0115741 is 1/16/5.4, so the 16 is put back here
		// rather than applied twice. On this data it barely matters - 99.3% of
		// nodes carry no width at all - but the calculation should be the
		// game's, not nearly the game's.
		return (n.width * WidthScale) * TwoWayWidthScale + LaneHalf;
	}

	// How many lanes this node actually offers the way the car is going. The
	// link packs at most two per side, which the archive confirms.
	int laneChoices(const VehNode& n, bool withDirection) const
	{
		const int lanes = withDirection ? n.lanesWith : n.lanesAgainst;
		return lanes > 0 ? lanes : 1;
	}

	// CCarCtrl adds the car's own lane to the link's offset before scaling by
	// the lane width: `(GetLaneOffset() + m_nCurrentLane) * 5.4` at 0x434900.
	// Lane 0 is the one nearest the centre line, and a car asking for a lane the
	// road does not have is put in the outermost one it does.
	float laneOffset(const VehNode& n, bool withDirection, int lane) const
	{
		const int choices = laneChoices(n, withDirection);
		if (lane < 0) lane = 0;
		if (lane >= choices) lane = choices - 1;
		return (laneCount(n) + static_cast<float>(lane)) * LaneWidth;
	}

	// Where a car driving this node actually sits. The offset is perpendicular to
	// the road, on its right: the game applies it as x + dirY*k, y - dirX*k.
	void lanePosition(const VehNode& n, float& outX, float& outY,
		bool withDirection = true, int lane = 0) const
	{
		// 0x422760 scales the road direction by the travel direction before
		// using it, so driving the other way puts the car on the other side of
		// the centre line rather than the same one. Without the sign, oncoming
		// traffic sits in your lane.
		const float k = laneOffset(n, withDirection, lane) * (withDirection ? 1.f : -1.f);
		outX = n.x + n.dirY * k;
		outY = n.y - n.dirX * k;
	}

	// A drivable node in a ring around a point, chosen without bias.
	int findInRing(float x, float y, float minDist, float maxDist, std::mt19937& rng) const
	{
		if (nodes_.empty() || maxDist <= 0.f) return -1;
		const float minSq = minDist * minDist, maxSq = maxDist * maxDist;
		const int cells = static_cast<int>(maxDist / CellSize) + 1;
		const int cx = cell(x), cy = cell(y);

		int chosen = -1, seen = 0;
		for (int gx = cx - cells; gx <= cx + cells; ++gx)
		{
			for (int gy = cy - cells; gy <= cy + cells; ++gy)
			{
				auto bucket = grid_.find(key(gx, gy));
				if (bucket == grid_.end()) continue;
				for (uint32_t index : bucket->second)
				{
					const VehNode& n = nodes_[index];
					if (n.flags & (VehNodeDeadEnd | VehNodeWater)) continue;
					if (!n.linkCount) continue;
					const float dx = n.x - x, dy = n.y - y;
					const float d2 = dx * dx + dy * dy;
					if (d2 < minSq || d2 > maxSq) continue;
					if (std::uniform_int_distribution<int>(0, seen++)(rng) == 0) chosen = static_cast<int>(index);
				}
			}
		}
		return chosen;
	}

	int findNearest(float x, float y, float maxDist) const
	{
		if (nodes_.empty()) return -1;
		const int cells = static_cast<int>(maxDist / CellSize) + 1;
		const int cx = cell(x), cy = cell(y);
		int best = -1;
		float bestSq = maxDist * maxDist;
		for (int gx = cx - cells; gx <= cx + cells; ++gx)
		{
			for (int gy = cy - cells; gy <= cy + cells; ++gy)
			{
				auto bucket = grid_.find(key(gx, gy));
				if (bucket == grid_.end()) continue;
				for (uint32_t index : bucket->second)
				{
					const VehNode& n = nodes_[index];
					const float dx = n.x - x, dy = n.y - y;
					const float d2 = dx * dx + dy * dy;
					if (d2 < bestSq) { bestSq = d2; best = static_cast<int>(index); }
				}
			}
		}
		return best;
	}

	// The nearest node at any distance. findNearest sizes its cell sweep from
	// maxDist, so it cannot be asked for "anywhere" - the cell count would
	// overflow long before the search finished. This is the linear scan that
	// question actually wants, and it is only used when the grid search has
	// already come back empty.
	int findNearestAny(float x, float y) const
	{
		int best = -1;
		float bestSq = std::numeric_limits<float>::infinity();
		for (size_t i = 0; i < nodes_.size(); ++i)
		{
			const float dx = nodes_[i].x - x, dy = nodes_[i].y - y;
			const float d2 = dx * dx + dy * dy;
			if (d2 < bestSq) { bestSq = d2; best = static_cast<int>(i); }
		}
		return best;
	}

	// Where this car goes next. Traffic carries on through a junction rather
	// than turning back on itself, so a link that would reverse the direction of
	// travel is only taken when it is the only way out.
	int nextNode(int from, int avoid, std::mt19937& rng) const
	{
		const VehNode* n = node(from);
		if (!n) return -1;
		float inX = 0.f, inY = 0.f;
		if (const VehNode* prev = node(avoid))
		{
			inX = n->x - prev->x;
			inY = n->y - prev->y;
			const float len = std::sqrt(inX * inX + inY * inY);
			if (len > 0.0001f) { inX /= len; inY /= len; }
			else { inX = 0.f; inY = 0.f; }
		}

		int chosen = -1, seen = 0, fallback = -1;
		for (uint16_t i = 0; i < n->linkCount; ++i)
		{
			const uint32_t at = n->linkStart + i;
			if (at >= links_.size()) break;
			const int target = static_cast<int>(links_[at]);
			const VehNode* t = node(target);
			if (!t) continue;
			if (target == avoid) { fallback = target; continue; }
			if (t->flags & VehNodeWater) continue;
			if (inX != 0.f || inY != 0.f)
			{
				float outX = t->x - n->x, outY = t->y - n->y;
				const float len = std::sqrt(outX * outX + outY * outY);
				if (len > 0.0001f)
				{
					outX /= len;
					outY /= len;
					// Anything sharper than about 100 degrees is a U-turn.
					if (inX * outX + inY * outY < -0.17f)
					{
						if (fallback < 0) fallback = target;
						continue;
					}
				}
			}
			if (std::uniform_int_distribution<int>(0, seen++)(rng) == 0) chosen = target;
		}
		return chosen >= 0 ? chosen : fallback;
	}

	// Should a car stop for the light at this node? A node with no light group
	// never stops anybody, and green or yellow lets them through.
	bool shouldStopForLight(int index, uint32_t timeMs) const
	{
		const VehNode* n = node(index);
		if (!n || !n->lightGroup) return false;
		return trafficLightForGroup(n->lightGroup, timeMs) == LightRed;
	}

	// A* shortest path between two world points, each snapped to its nearest
	// drivable node within `snapDist`. Returns the path as an x,y pair per
	// node in travel order (the snapped start node first, the snapped goal
	// node last) or an empty vector if either point has no node nearby or no
	// path connects them - two ends of the map with no road between them, for
	// instance.
	//
	// Road distance, not turn count, is what open.set/priority_queue orders
	// on, so this is Dijkstra with a straight-line lower bound to the goal as
	// the A* heuristic - admissible because no road is ever shorter than a
	// straight line between its endpoints.
	// The A* itself, as node indices in travel order. findRoute and
	// findRouteDriving both build their answer from this, so the search lives
	// in one place and the two callers only differ in what they read off each
	// node.
	// bestEffort: when the destination cannot be reached, return the route to
	// the reachable node closest to it rather than nothing at all. Off by
	// default so the gamemode's own routing keeps its existing all-or-nothing
	// contract. arrivalRadius optionally accepts a reachable road node near
	// the destination; zero preserves exact-node routing for existing callers.
	std::vector<int> findRouteNodes(float startX, float startY, float endX, float endY,
		float snapDist = 60.f, bool bestEffort = false, float arrivalRadius = 0.f) const
	{
		int start = findNearest(startX, startY, snapDist);
		int goal  = findNearest(endX,   endY,   snapDist);
		// A destination off the end of the road network - somewhere out in the
		// extended map with no nodes near it - snaps to nothing at the normal
		// distance. Widen the search rather than give up.
		if (goal  < 0 && bestEffort) goal  = findNearestAny(endX,   endY);
		if (start < 0 && bestEffort) start = findNearestAny(startX, startY);
		if (start < 0 || goal < 0) return {};

		// If either endpoint landed on a disconnected island, re-snap it to the
		// nearest main-network node. Without this, A* finds no path and the
		// gamemode falls back to a two-point straight line.
		if (!comp_.empty())
		{
			if (comp_[start] != mainComp_)
			{
				const int s2 = findNearestInMain(startX, startY, snapDist * 4.f);
				if (s2 >= 0) start = s2;
			}
			if (comp_[goal] != mainComp_)
			{
				const int g2 = findNearestInMain(endX, endY, snapDist * 4.f);
				if (g2 >= 0) goal = g2;
			}
		}
		if (start == goal) return {start};

		const size_t n = nodes_.size();
		std::vector<float> best(n, std::numeric_limits<float>::infinity());
		std::vector<int> prev(n, -1);
		std::vector<bool> closed(n, false);

		struct Item { float f; int node; };
		struct Cmp { bool operator()(const Item& a, const Item& b) const { return a.f > b.f; } };
		std::priority_queue<Item, std::vector<Item>, Cmp> open;

		auto heuristic = [&](int at) {
			if (arrivalRadius > 0.f) {
				const float dx = nodes_[at].x - endX, dy = nodes_[at].y - endY;
				return std::max(0.f, std::sqrt(dx*dx + dy*dy) - arrivalRadius);
			}
			const float dx = nodes_[at].x - nodes_[goal].x;
			const float dy = nodes_[at].y - nodes_[goal].y;
			return std::sqrt(dx * dx + dy * dy);
		};

		best[start] = 0.f;
		open.push({heuristic(start), start});

		while (!open.empty())
		{
			const Item cur = open.top();
			open.pop();
			if (closed[cur.node]) continue;
			closed[cur.node] = true;
			if (arrivalRadius > 0.f) {
				const float dx = nodes_[cur.node].x - endX, dy = nodes_[cur.node].y - endY;
				if (dx*dx + dy*dy <= arrivalRadius*arrivalRadius) {
					goal = cur.node;
					break;
				}
			}
			if (cur.node == goal) break;

			const VehNode& cn = nodes_[cur.node];
			for (uint16_t i = 0; i < cn.linkCount; ++i)
			{
				const uint32_t at = cn.linkStart + i;
				if (at >= links_.size()) break;
				const int target = static_cast<int>(links_[at]);
				if (target < 0 || static_cast<size_t>(target) >= n || closed[target]) continue;

				const VehNode& tn = nodes_[target];
				const float dx = tn.x - cn.x, dy = tn.y - cn.y;
				const float cost = best[cur.node] + std::sqrt(dx * dx + dy * dy);
				if (cost < best[target])
				{
					best[target] = cost;
					prev[target] = cur.node;
					open.push({cost + heuristic(target), target});
				}
			}
		}

		// The goal was never reached: it sits on a part of the network with no
		// route in from here. Without bestEffort that is simply a failure; with
		// it, aim for whichever node the search did reach that ends up closest
		// to the goal, so the ribbon still covers the drivable part of the trip.
		int target = goal;
		if (!closed[goal])
		{
			if (!bestEffort) return {};
			float bestSq = std::numeric_limits<float>::infinity();
			int fallback = -1;
			for (size_t i = 0; i < n; ++i)
			{
				if (!closed[i]) continue;
				const float dx = nodes_[i].x - nodes_[goal].x;
				const float dy = nodes_[i].y - nodes_[goal].y;
				const float d2 = dx * dx + dy * dy;
				if (d2 < bestSq) { bestSq = d2; fallback = static_cast<int>(i); }
			}
			if (fallback < 0) return {};
			target = fallback;
		}

		std::vector<int> path;
		for (int at = target; at != -1; at = prev[at])
		{
			path.push_back(at);
			if (at == start) break;
		}
		if (path.empty() || path.back() != start) return {};
		std::reverse(path.begin(), path.end());
		return path;
	}

	// Road centre line, as "x y" pairs in travel order.
	std::vector<float> findRoute(float startX, float startY, float endX, float endY,
		float snapDist = 60.f, bool bestEffort = false) const
	{
		const std::vector<int> path = findRouteNodes(startX, startY, endX, endY, snapDist, bestEffort);
		std::vector<float> out;
		out.reserve(path.size() * 2);
		for (int idx : path)
		{
			out.push_back(nodes_[idx].x);
			out.push_back(nodes_[idx].y);
		}
		return out;
	}

	// Where a car would actually drive this route: "x y z" triples in travel
	// order, each on its own side of the road rather than on the centre line.
	//
	// The lane offset is lanePosition's, which is the game's own - it reads the
	// node's real width and lane counts instead of assuming a road shape, so a
	// dual carriageway and a one-lane back street come out differently.
	//
	// Which side "its own side" is depends on which way the route runs through
	// the node, not on the node's stored direction: driving a link backwards
	// has to put the line on the opposite side of the centre, or the route is
	// drawn into oncoming traffic. The sign comes from comparing the direction
	// of travel here against the node's stored one.
	//
	// Lane 0 is the lane nearest the centre line, which is where the game puts
	// a car that has not chosen to overtake.
	std::vector<float> findRouteDriving(float startX, float startY, float endX, float endY,
		float snapDist = 60.f, bool bestEffort = false, float arrivalRadius = 0.f) const
	{
		const std::vector<int> path = findRouteNodes(startX, startY, endX, endY, snapDist, bestEffort, arrivalRadius);
		const size_t count = path.size();
		std::vector<float> out;
		if (count == 0) return out;

		// The direction the route is actually travelling at each node, as a unit
		// vector. At a junction the node's own dirX/dirY belongs to whichever arm
		// the data was built around, which need not be the arm being driven, so
		// the offset is taken across the route's own tangent instead. Using the
		// stored direction there swings the line sideways at the junction and
		// back again - a visible swerve on a road being driven straight through.
		std::vector<float> tanX(count, 0.f), tanY(count, 0.f);
		for (size_t i = 0; i < count; ++i)
		{
			float ax = 0.f, ay = 0.f;
			auto addUnit = [&](size_t from, size_t to) {
				const float dx = nodes_[path[to]].x - nodes_[path[from]].x;
				const float dy = nodes_[path[to]].y - nodes_[path[from]].y;
				const float len = std::sqrt(dx * dx + dy * dy);
				if (len > 0.001f) { ax += dx / len; ay += dy / len; }
			};
			if (i > 0) addUnit(i - 1, i);
			if (i + 1 < count) addUnit(i, i + 1);
			const float len = std::sqrt(ax * ax + ay * ay);
			if (len > 0.001f) { tanX[i] = ax / len; tanY[i] = ay / len; }
			else { tanX[i] = nodes_[path[i]].dirX; tanY[i] = nodes_[path[i]].dirY; }
		}

		// How far onto its own side of the road the line sits at each node. This
		// is the game's own number, from the node's real width and lane counts.
		std::vector<float> offset(count, 0.f);
		for (size_t i = 0; i < count; ++i)
		{
			const VehNode& n = nodes_[path[i]];
			const bool withDirection = (tanX[i] * n.dirX + tanY[i] * n.dirY) >= 0.f;
			offset[i] = laneOffset(n, withDirection, 0);
		}

		// Junction nodes routinely carry a different width and lane count from
		// the road running into them, so the raw offset can step at a single
		// node and step back at the next. Multiple 1-2-1 passes spread any such
		// step over its neighbours; five passes are enough to suppress a single-
		// node spike without flattening genuine road-width changes.
		for (int pass = 0; pass < 5 && count >= 3; ++pass)
		{
			std::vector<float> smoothed = offset;
			for (size_t i = 1; i + 1 < count; ++i)
				smoothed[i] = (offset[i - 1] + 2.f * offset[i] + offset[i + 1]) * 0.25f;
			offset = std::move(smoothed);
		}

		out.reserve(count * 3);
		for (size_t i = 0; i < count; ++i)
		{
			const VehNode& n = nodes_[path[i]];
			// Right of the direction of travel, the way lanePosition applies it:
			// x + dirY*k, y - dirX*k. The tangent already points the way we are
			// going, so driving a link backwards lands on the opposite side
			// without needing a sign of its own.
			const float k = offset[i];
			out.push_back(n.x + tanY[i] * k);
			out.push_back(n.y - tanX[i] * k);
			out.push_back(n.z);
		}
		return out;
	}

private:
	static int cell(float v) { return static_cast<int>(std::floor(v / CellSize)); }
	static uint64_t key(int gx, int gy)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(gx)) << 32) | static_cast<uint32_t>(gy);
	}

	void buildIndex()
	{
		grid_.clear();
		grid_.reserve(nodes_.size() / 4 + 1);
		for (uint32_t i = 0; i < nodes_.size(); ++i)
			grid_[key(cell(nodes_[i].x), cell(nodes_[i].y))].push_back(i);
	}

	// Union-Find helpers for component labelling. Path compression only (no
	// rank) because we traverse the whole array once and never merge again.
	int ufFind(std::vector<int>& parent, int i) const
	{
		while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
		return i;
	}
	void ufUnion(std::vector<int>& parent, int a, int b) const
	{
		a = ufFind(parent, a); b = ufFind(parent, b);
		if (a != b) parent[b] = a;
	}

	// Label every node with its weakly connected component (treating each link
	// as undirected). comp_[i] is the root of node i's set; mainComp_ is the
	// root that the largest set belongs to. Islands are any node whose root
	// differs from mainComp_.
	void buildComponents()
	{
		const size_t n = nodes_.size();
		std::vector<int> parent(n);
		for (size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i);

		for (size_t i = 0; i < n; ++i)
		{
			const VehNode& nd = nodes_[i];
			for (uint16_t j = 0; j < nd.linkCount; ++j)
			{
				const uint32_t at = nd.linkStart + j;
				if (at >= links_.size()) break;
				const int t = static_cast<int>(links_[at]);
				if (t >= 0 && static_cast<size_t>(t) < n)
					ufUnion(parent, static_cast<int>(i), t);
			}
		}

		comp_.resize(n);
		std::unordered_map<int, int> counts;
		for (size_t i = 0; i < n; ++i)
		{
			comp_[i] = ufFind(parent, static_cast<int>(i));
			counts[comp_[i]]++;
		}

		mainComp_ = 0;
		int best = 0;
		for (auto& kv : counts)
			if (kv.second > best) { best = kv.second; mainComp_ = kv.first; }
	}

	// Nearest node that belongs to the main connected component, searching up
	// to maxDist away. Falls back to findNearest if no main-component node is
	// found within that radius (shouldn't happen for any point on the road map).
	int findNearestInMain(float x, float y, float maxDist) const
	{
		if (nodes_.empty()) return -1;
		const int cells = static_cast<int>(maxDist / CellSize) + 1;
		const int cx = cell(x), cy = cell(y);
		int best = -1;
		float bestSq = maxDist * maxDist;
		for (int gx = cx - cells; gx <= cx + cells; ++gx)
		{
			for (int gy = cy - cells; gy <= cy + cells; ++gy)
			{
				auto bucket = grid_.find(key(gx, gy));
				if (bucket == grid_.end()) continue;
				for (uint32_t index : bucket->second)
				{
					if (comp_[index] != mainComp_) continue;
					const VehNode& n = nodes_[index];
					const float dx = n.x - x, dy = n.y - y;
					const float d2 = dx * dx + dy * dy;
					if (d2 < bestSq) { bestSq = d2; best = static_cast<int>(index); }
				}
			}
		}
		return best;
	}

	std::vector<VehNode> nodes_;
	std::vector<uint32_t> links_;
	std::unordered_map<uint64_t, std::vector<uint32_t>> grid_;
	std::vector<int> comp_;
	int mainComp_ = 0;
};
} // namespace sprp_ai

#endif
