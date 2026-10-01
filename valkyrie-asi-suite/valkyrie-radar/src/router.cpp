#include "router.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

// The server's road graph and A*, included rather than copied.
//
// vehgraph.hpp depends on nothing outside the standard library, so the same
// header serves the server component and this plugin and the two cannot drift
// apart. It is written for clang and this project builds with /W4 /WX, so the
// warnings it trips are suppressed around the include rather than in the
// header itself, which has to stay as the server's.
#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4245 4267 4365 4456 4457 4458 4701)
#include "vehgraph.hpp"
#pragma warning(pop)

namespace router {
namespace {

sprp_ai::VehGraph g_graph;
std::mutex        g_lock;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_started{false};
// Set once the load thread has finished, whether or not it found a graph.
std::atomic<bool> g_settled{false};

// How far either end of a journey may be from a road and still snap to it in
// the ordinary way. The graph's own default, kept here so both ends of Route
// are visibly using the same number.
constexpr float kSnap = 60.f;

int LoadEmbeddedGraph(sprp_ai::VehGraph& graph) {
  HMODULE self{};
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&LoadEmbeddedGraph), &self)) return -1;
  char modulePath[MAX_PATH]{};
  if (!GetModuleFileNameA(self, modulePath, MAX_PATH)) return -1;
  std::ifstream file(modulePath, std::ios::binary | std::ios::ate);
  const std::streamoff total = file ? file.tellg() : std::streamoff(-1);
  constexpr char magic2[8] = {'V','K','R','G','R','P','H','2'};
  constexpr char magic1[8] = {'V','K','R','G','R','P','H','1'};
  if (total < 12) return -1;
  // Universal payload: [stock graph][legacy secondary graph][magic][sizes].
  if (total >= 16) {
    file.seekg(total - 16);
    char found[8]{};
    uint32_t stockSize = 0, secondarySize = 0;
    file.read(found, 8);
    file.read(reinterpret_cast<char*>(&stockSize), 4);
    file.read(reinterpret_cast<char*>(&secondarySize), 4);
    const uint64_t payload = static_cast<uint64_t>(stockSize) + secondarySize;
    if (file && std::memcmp(found, magic2, 8) == 0 && stockSize && secondarySize &&
        payload <= static_cast<uint64_t>(total - 16)) {
      const uint32_t size = stockSize;
      const std::streamoff offset = total - 16 - static_cast<std::streamoff>(payload);
      std::vector<uint8_t> bytes(size);
      file.seekg(offset);
      file.read(reinterpret_cast<char*>(bytes.data()), size);
      return file ? graph.load(bytes.data(), bytes.size()) : -3;
    }
    file.clear();
  }
  // Version 1 remains readable for developer builds and old installs.
  file.seekg(total - 12);
  char found[8]{};
  uint32_t size = 0;
  file.read(found, 8);
  file.read(reinterpret_cast<char*>(&size), 4);
  if (!file || std::memcmp(found, magic1, 8) != 0 || !size ||
      static_cast<std::streamoff>(size) > total - 12) return -1;
  std::vector<uint8_t> bytes(size);
  file.seekg(total - 12 - static_cast<std::streamoff>(size));
  file.read(reinterpret_cast<char*>(bytes.data()), size);
  return file ? graph.load(bytes.data(), bytes.size()) : -3;
}

} // namespace

void Init(const std::string &gameDir) {
  // Once only, however many times a caller asks.
  if (g_started.exchange(true)) return;

  const std::string branded = gameDir + "Valkyrie-roadgraph.bin";
  const std::string legacy = gameDir + "sprp-vehgraph.bin";
  try {
  std::thread([branded, legacy] {
    try {
    sprp_ai::VehGraph graph;
    int nodes = LoadEmbeddedGraph(graph);
    std::string path = nodes > 0 ? "embedded in valkyrie-radar.asi" : branded;
    if (nodes <= 0) {
      graph = sprp_ai::VehGraph{};
      nodes = graph.load(branded);
      if (nodes > 0) path = branded;
    }
    if (nodes <= 0) {
      graph = sprp_ai::VehGraph{};
      nodes = graph.load(legacy);
    }
    if (nodes <= 0) {
      // Negative values are the loader's own codes: -1 unreadable, -2 bad
      // magic, -3 truncated. Say which, because "no route in single player"
      // is otherwise indistinguishable from every other reason for no route.
      logfile::Line("router: no road graph at %s (load returned %d)",
                    path.c_str(), nodes);
      g_settled = true;
      return;
    }
    {
      std::lock_guard<std::mutex> guard(g_lock);
      g_graph = std::move(graph);
    }
    g_ready = true;
    g_settled = true;
    logfile::Line("router: road graph loaded, %d nodes", nodes);
    } catch (...) {
      g_settled = true;
      logfile::Line("router: graph load failed; retaining native GPS");
    }
  }).detach();
  } catch (...) {
    g_settled = true;
    logfile::Line("router: cannot start graph loader; retaining native GPS");
  }
}

bool Ready() { return g_ready.load(); }

bool Loading() { return g_started.load() && !g_settled.load(); }

bool Route(float sx, float sy, float ex, float ey, std::vector<float> &out,
           float *endGap) {
  if (endGap) *endGap = -1.f;
  if (!g_ready.load()) return false;
  std::lock_guard<std::mutex> guard(g_lock);

  // The player's end first, and at the ordinary snap distance. See the header:
  // best effort belongs to the destination, not to here.
  if (g_graph.findNearest(sx, sy, kSnap) < 0) {
    out.clear();
    return false;
  }

  // Arrival already triggers within 25m. Stop on the first reachable road
  // inside 20m instead of circling a junction to hit one particular node.
  //
  // Best effort on the goal is the whole of what lets a destination off the
  // road network be navigated to at all. The search aims for the reachable
  // node that ends up closest to it, and how far short that leaves the ribbon
  // comes back in endGap for the caller to judge.
  out = g_graph.findRouteDriving(sx, sy, ex, ey, kSnap, true, 20.f);
  // Two points is the minimum that can be drawn as a ribbon.
  if (out.size() < 6) {
    out.clear();
    return false;
  }
  if (endGap) {
    const float dx = out[out.size() - 3] - ex, dy = out[out.size() - 2] - ey;
    *endGap = sqrtf(dx * dx + dy * dy);
  }
  return true;
}

} // namespace router
