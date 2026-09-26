#include "services.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "game.h"
#include "log.h"
#include "script.h"

namespace services {
namespace {

// Which city's force answers: the game's own patrol car for the zone.
constexpr uintptr_t kChoosePoliceCarModel = 0x421980;   // (uint32) -> model
constexpr uintptr_t kGetVehicle = 0x54FFF0;              // (handle) -> CVehicle* or null
constexpr uintptr_t kFindPlayerPed = 0x56E210;           // CPed* (int player)
// CVector FindPlayerCoors(int player), through a hidden pointer.
constexpr uintptr_t kFindPlayerCoors = 0x56E010;
constexpr size_t kMatrix = 0x14;
constexpr size_t kMatrixPos = 0x30;
constexpr size_t kPlacementPos = 0x04;
constexpr size_t kPedHealth = 0x540, kPedMaxHealth = 0x544;
// gFireManager: 60 CFires of 0x28 bytes - flags (bit 0 burning), then where.
constexpr uintptr_t kFires = 0xB71F80;
constexpr int kFireCount = 60;
constexpr size_t kFireSize = 0x28, kFirePos = 0x04;

// The mission-script commands a unit is made and run with, as a mission
// does it. Models through the script commands too, not the streaming
// arrays: a limit adjuster can move those.
constexpr uint16_t kLoadModel = 0x0247;              // model
constexpr uint16_t kLoadRequestedModels = 0x038B;
constexpr uint16_t kHasModelLoaded = 0x0248;         // model
constexpr uint16_t kReleaseModel = 0x0249;           // model
constexpr uint16_t kCreateCar = 0x00A5;              // model, x, y, z -> car
constexpr uint16_t kSetCarHeading = 0x0175;          // car, degrees
constexpr uint16_t kCreateDriver = 0x0129;           // car, ped type, model -> ped
constexpr uint16_t kCreatePassenger = 0x01C8;        // car, ped type, model, seat -> ped
constexpr uint16_t kSiren = 0x0397;                  // car, on
constexpr uint16_t kDrivingStyle = 0x00AE;           // car, style
constexpr uint16_t kCruiseSpeed = 0x00AD;            // car, speed
constexpr uint16_t kDriveTo = 0x00A7;                // car, x, y, z
constexpr uint16_t kWander = 0x00A8;                 // car
constexpr uint16_t kBlipForCar = 0x0186;             // car -> blip
constexpr uint16_t kBlipForChar = 0x0187;            // ped -> blip
constexpr uint16_t kBlipColour = 0x0165;             // blip, colour
constexpr uint16_t kRemoveBlip = 0x0164;             // blip
constexpr uint16_t kLeaveCar = 0x0633;               // ped
constexpr uint16_t kCarNoLongerNeeded = 0x01C3;      // car
constexpr uint16_t kCharNoLongerNeeded = 0x01C2;     // ped
constexpr uint16_t kClosestCarNode = 0x02C1;         // x, y, z -> x, y, z
constexpr uint16_t kSphereOnScreen = 0x00C2;         // x, y, z, radius
constexpr uint16_t kCharExists = 0x056D;             // ped
constexpr uint16_t kCharDead = 0x0118;               // ped
constexpr uint16_t kCharCoords = 0x00A0;             // ped -> x, y, z
constexpr uint16_t kCharOnFoot = 0x044B;             // ped
constexpr uint16_t kCharInAnyCar = 0x00DF;           // ped
constexpr uint16_t kCharInCar = 0x00DB;              // ped, car
constexpr uint16_t kPlayerGroup = 0x07AF;            // player -> group
constexpr uint16_t kGroupMember = 0x0631;            // group, ped
constexpr uint16_t kLeaveGroup = 0x06C9;             // ped
constexpr uint16_t kWantedLevel = 0x01C0;            // player -> level
constexpr uint16_t kMoney = 0x010B;                  // player -> money
constexpr uint16_t kAddMoney = 0x0109;               // player, money
constexpr uint16_t kGiveWeapon = 0x01B2;             // ped, weapon, ammo
constexpr uint16_t kCurrentWeapon = 0x01B9;          // ped, weapon
constexpr uint16_t kGoStraightTo = 0x05D3;           // ped, x, y, z, move state, ms
constexpr uint16_t kGoToChar = 0x05D9;               // ped, target, ms, radius
constexpr uint16_t kShootAtCoord = 0x0668;           // ped, x, y, z, ms
constexpr uint16_t kExtinguish = 0x0980;             // x, y, z, radius
constexpr uint16_t kFiresInRange = 0x06C3;           // x, y, z, radius -> count
constexpr uint16_t kFaceChar = 0x0639;               // ped, target
constexpr uint16_t kLookAtChar = 0x05BF;             // ped, target, ms
constexpr uint16_t kLookAbout = 0x05C9;              // ped, ms
constexpr uint16_t kPlayAnim = 0x0605;               // ped, anim, file, blend, loop, lock x, lock y, keep, ms
constexpr uint16_t kClearTasks = 0x0687;             // ped
constexpr uint16_t kRequestAnims = 0x04ED;           // file
constexpr uint16_t kAnimsLoaded = 0x04EE;            // file
constexpr uint16_t kEnterAsDriver = 0x05CB;          // ped, car, ms
constexpr uint16_t kEnterAsPassenger = 0x05CA;       // ped, car, ms, seat
constexpr uint16_t kAddFloatStat = 0x0A1F;           // stat, value (no message)
constexpr uint16_t kDriveWander = 0x05D2;            // ped, car, speed, driving style
constexpr uint16_t kKeepTask = 0x0961;               // ped, keep

constexpr int kPlayer = 0;
constexpr int kPedCop = 6;
constexpr int kPedEmergency = 18;
constexpr int kPedFireman = 19;
constexpr int kPedCivMale = 4, kPedCivFemale = 5;
constexpr int kDriveAvoidCars = 2;
constexpr int kRun = 6, kWalk = 4;
constexpr int kExtinguisher = 42;
constexpr int kStatCalories = 245;
constexpr int kBlipBlue = 2;
constexpr float kCruise = 30.0f, kDeliveryCruise = 24.0f;

// How close counts as there, how often the unit is pointed at where the
// player is now, and how long it is given before it is handed back.
constexpr float kArrivedDistance = 20.0f, kDeliveryArrived = 14.0f;
constexpr ULONGLONG kRedirectMs = 4000;
constexpr ULONGLONG kGiveUpMs = 180000, kDeliveryGiveUpMs = 240000;
// Where a unit starts: a road about this far off, out of sight.
constexpr float kSpawnDistance = 140.0f;
constexpr float kSpawnMin = 60.0f, kSpawnMax = 260.0f;
// The officers stay at CJ's side this long.
constexpr ULONGLONG kGuardMs = 180000;
// The firefighters work fires this far round the scene.
constexpr float kFireRadius = 45.0f;
// A paramedic's treatment, start to finish.
constexpr ULONGLONG kTreatMs = 4500;

// Each city's own units: the vehicle and the crew's model.
struct Crew {
    int car, ped, pedType;
};
enum City { kLosSantos, kSanFierro, kLasVenturas, kCountry };
const Crew kPolice[] = {{596, 280, kPedCop}, {597, 281, kPedCop}, {598, 282, kPedCop}, {599, 283, kPedCop}};
const Crew kAmbulance[] = {{416, 274, kPedEmergency}, {416, 276, kPedEmergency}, {416, 275, kPedEmergency},
                           {416, 274, kPedEmergency}};
const Crew kFireTruck[] = {{407, 277, kPedFireman}, {407, 279, kPedFireman}, {407, 278, kPedFireman},
                           {407, 277, kPedFireman}};

// The restaurants: data\shopping.dat's FDpiza, FDburg and FDchick - the
// names are the shops' own menu boards, the prices and calories the file's.
// A salad meal adds no calories. Delivered on the Pizzaboy by the
// restaurant's own staff.
const Meal kMenus[3][kMeals] = {
    {{"Buster", 2, 20.0f}, {"Double D-Luxe", 5, 40.0f}, {"Full Rack", 10, 70.0f}, {"Salad Meal", 10, 0.0f}},
    {{"Moo Kids Meal", 2, 20.0f}, {"Beef Tower", 5, 40.0f}, {"Meat Stack", 10, 70.0f}, {"Salad Meal", 5, 0.0f}},
    {{"Cluckin' Little Meal", 2, 20.0f}, {"Cluckin' Big Meal", 5, 40.0f}, {"Cluckin' Huge Meal", 10, 70.0f},
     {"Salad Meal", 10, 0.0f}},
};
const char* const kRestaurantNames[3] = {"Well Stacked Pizza", "Burger Shot", "Cluckin' Bell"};
const Crew kRiders[3] = {{448, 155, kPedCivMale}, {448, 205, kPedCivFemale}, {448, 167, kPedCivMale}};

enum class Kind { Police, Fire, Medical, Delivery };
enum class Stage { Driving, GettingOut, Working, Leaving };

// One of the crew at a fire: what they are putting out, and how far along.
struct Hand {
    int ped = 0;
    int phase = 0;  // 0 looking for a fire, 1 going to it, 2 spraying
    game::Vector fire{};
    ULONGLONG at = 0;
};

struct Dispatched {
    Kind kind = Kind::Police;
    int car = 0, driver = 0, partner = 0, blip = 0;
    int model = 0;
    Stage stage = Stage::Driving;
    ULONGLONG sent = 0, stageAt = 0, lastRedirect = 0, lastLog = 0, lastOrder = 0;
    // Police: in CJ's group, and since when CJ has been in a car.
    bool guarding = false;
    ULONGLONG inCarSince = 0;
    // Fire.
    Hand hands[2];
    ULONGLONG idleSince = 0;
    // Medical and delivery: how far along the hand-over or treatment is.
    int phase = 0;
    ULONGLONG phaseAt = 0;
    float healFrom = 0.0f;
    // Delivery.
    int restaurant = 0, meal = 0, price = 0;
    bool delivered = false;
};

std::deque<Unit> g_queue;
std::vector<Dispatched> g_units;
std::mt19937 g_rng(GetTickCount());
void (*g_messenger)(const char*, const char*) = nullptr;
std::string g_numbers[3];

uintptr_t PlayerPed() { return reinterpret_cast<uintptr_t(__cdecl*)(int)>(kFindPlayerPed)(-1); }
int PlayerHandle() {
    const uintptr_t ped = PlayerPed();
    return ped ? script::PedHandle(ped) : 0;
}

game::Vector PlayerCoors() {
    game::Vector at{};
    reinterpret_cast<game::Vector*(__cdecl*)(game::Vector*, int)>(kFindPlayerCoors)(&at, -1);
    return at;
}

float Distance2d(const game::Vector& a, const game::Vector& b) { return std::hypot(a.x - b.x, a.y - b.y); }
float Distance3d(const game::Vector& a, const game::Vector& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

bool CarCoors(int car, game::Vector& out) {
    if (!car) return false;
    const uintptr_t vehicle = reinterpret_cast<uintptr_t(__cdecl*)(int)>(kGetVehicle)(car);
    if (!vehicle) return false;
    const uintptr_t matrix = *reinterpret_cast<const uintptr_t*>(vehicle + kMatrix);
    const float* p = reinterpret_cast<const float*>(matrix ? matrix + kMatrixPos : vehicle + kPlacementPos);
    out = {p[0], p[1], p[2]};
    return true;
}

// Alive and still there - a ped the game has since removed is neither.
bool Alive(int ped) { return ped && script::Command(kCharExists, {ped}) && !script::Command(kCharDead, {ped}); }

bool CharCoors(int ped, game::Vector& out) {
    if (!Alive(ped)) return false;
    script::Locals l;
    script::Command(kCharCoords, {ped, script::Arg::Local(0), script::Arg::Local(1), script::Arg::Local(2)}, &l);
    out = {l.Float(0), l.Float(1), l.Float(2)};
    return true;
}

int Money() {
    script::Locals l;
    script::Command(kMoney, {kPlayer, script::Arg::Local(0)}, &l);
    return l.Int(0);
}

int Wanted() {
    script::Locals l;
    script::Command(kWantedLevel, {kPlayer, script::Arg::Local(0)}, &l);
    return l.Int(0);
}

City CityHere() {
    // The game's own answer to "whose police": its patrol car for the zone.
    switch (reinterpret_cast<int(__cdecl*)(uint32_t)>(kChoosePoliceCarModel)(0)) {
        case 597: return kSanFierro;
        case 598: return kLasVenturas;
        case 599: return kCountry;
        default: return kLosSantos;
    }
}

bool LoadModels(std::initializer_list<int> models) {
    for (int m : models) script::Command(kLoadModel, {m});
    script::Command(kLoadRequestedModels, {});
    for (int m : models) {
        if (!script::Command(kHasModelLoaded, {m})) {
            logfile::Line("services: model %d did not load", m);
            return false;
        }
    }
    return true;
}

void ReleaseModels(std::initializer_list<int> models) {
    for (int m : models) script::Command(kReleaseModel, {m});
}

void Text(int restaurant, const std::string& body) {
    if (g_messenger && !g_numbers[restaurant].empty()) g_messenger(g_numbers[restaurant].c_str(), body.c_str());
}

// The fires burning within `radius` of `at`, from the game's own list. If
// that list does not agree with the game's own count (a limit adjuster that
// moved it), none are given, and the crew work the scene as a whole.
std::vector<game::Vector> FiresNear(const game::Vector& at, float radius, int& gameCount) {
    std::vector<game::Vector> out;
    script::Locals l;
    script::Command(kFiresInRange, {at.x, at.y, at.z, radius, script::Arg::Local(0)}, &l);
    gameCount = l.Int(0);
    const auto* base = reinterpret_cast<const uint8_t*>(kFires);
    for (int i = 0; i < kFireCount; ++i) {
        const uint8_t* fire = base + i * kFireSize;
        if (!(fire[0] & 1)) continue;
        const float* p = reinterpret_cast<const float*>(fire + kFirePos);
        const game::Vector pos{p[0], p[1], p[2]};
        if (Distance3d(pos, at) <= radius) out.push_back(pos);
    }
    if (out.empty() && gameCount > 0) {
        static bool told = false;
        if (!told) {
            told = true;
            logfile::Line("services: the game counts %d fires but its fire list shows none - working the scene", gameCount);
        }
    }
    return out;
}

// A road out of sight, some way off: tried in eight directions from a random
// start, the first that fits is used, else the best seen.
bool SpawnPoint(const game::Vector& player, game::Vector& out) {
    const float start = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(g_rng);
    bool found = false;
    float bestScore = 1e9f;
    for (int i = 0; i < 8; ++i) {
        const float a = start + i * 0.7853982f;
        script::Locals l;
        script::Command(kClosestCarNode,
                        {player.x + std::cos(a) * kSpawnDistance, player.y + std::sin(a) * kSpawnDistance, player.z,
                         script::Arg::Local(0), script::Arg::Local(1), script::Arg::Local(2)},
                        &l);
        const game::Vector node{l.Float(0), l.Float(1), l.Float(2)};
        if (node.x == 0.0f && node.y == 0.0f) continue;
        const float d = Distance2d(node, player);
        const bool seen = script::Command(kSphereOnScreen, {node.x, node.y, node.z, 5.0f});
        if (d >= kSpawnMin && d <= kSpawnMax && !seen) {
            out = node;
            return true;
        }
        // Otherwise the nearest to the ideal distance, unseen first.
        const float score = std::fabs(d - kSpawnDistance) + (seen ? 1000.0f : 0.0f) + (d < 30.0f ? 5000.0f : 0.0f);
        if (score < bestScore) {
            bestScore = score;
            out = node;
            found = true;
        }
    }
    return found && bestScore < 5000.0f;
}

// Make the unit and start it on its way. False when it could not be made.
bool Dispatch(Kind kind, const Crew& crew, const game::Vector& player, Dispatched* made = nullptr) {
    game::Vector at{};
    if (!SpawnPoint(player, at)) {
        logfile::Line("services: no road found near %.0f, %.0f for vehicle %d", player.x, player.y, crew.car);
        return false;
    }
    if (!LoadModels({crew.car, crew.ped})) return false;
    script::Locals l;
    script::Command(kCreateCar, {crew.car, at.x, at.y, at.z + 0.5f, script::Arg::Local(0)}, &l);
    Dispatched u = made ? *made : Dispatched{};
    u.kind = kind;
    u.car = l.Int(0);
    u.model = crew.car;
    game::Vector check{};
    if (!u.car || !CarCoors(u.car, check)) {
        logfile::Line("services: vehicle %d could not be created at %.0f, %.0f, %.0f", crew.car, at.x, at.y, at.z);
        ReleaseModels({crew.car, crew.ped});
        return false;
    }
    // Facing the caller, so it sets off the right way.
    const float heading = std::atan2(-(player.x - at.x), player.y - at.y) * 57.29578f;
    script::Command(kSetCarHeading, {u.car, heading});
    script::Command(kCreateDriver, {u.car, crew.pedType, crew.ped, script::Arg::Local(1)}, &l);
    u.driver = l.Int(1);
    // A delivery is one rider; a unit is two.
    if (kind != Kind::Delivery) {
        script::Command(kCreatePassenger, {u.car, crew.pedType, crew.ped, 0, script::Arg::Local(2)}, &l);
        u.partner = l.Int(2);
    }
    ReleaseModels({crew.car, crew.ped});

    if (kind != Kind::Delivery) script::Command(kSiren, {u.car, 1});
    script::Command(kDrivingStyle, {u.car, kDriveAvoidCars});
    script::Command(kCruiseSpeed, {u.car, kind == Kind::Delivery ? kDeliveryCruise : kCruise});
    script::Command(kDriveTo, {u.car, player.x, player.y, player.z});
    script::Command(kBlipForCar, {u.car, script::Arg::Local(3)}, &l);
    u.blip = l.Int(3);
    if (u.blip && kind != Kind::Delivery) script::Command(kBlipColour, {u.blip, kBlipBlue});
    u.sent = u.stageAt = u.lastRedirect = u.lastLog = GetTickCount64();
    g_units.push_back(u);
    logfile::Line("services: vehicle %d with crew %d sent from %.0f, %.0f, %.0f - %.0f from the caller", crew.car,
                  crew.ped, at.x, at.y, at.z, Distance2d(at, player));
    return true;
}

void SetStage(Dispatched& u, Stage s) {
    u.stage = s;
    u.stageAt = GetTickCount64();
}

// Hand a unit back to the game: out of CJ's group, the blip gone, and car
// and crew no longer held, so they become ordinary police, firefighters,
// medics or a scooter rider going about their day.
void Release(Dispatched& u, const char* why) {
    if (u.blip) script::Command(kRemoveBlip, {u.blip});
    u.blip = 0;
    for (int ped : {u.driver, u.partner}) {
        if (!ped || !script::Command(kCharExists, {ped})) continue;
        if (u.guarding) script::Command(kLeaveGroup, {ped});
        script::Command(kCharNoLongerNeeded, {ped});
    }
    game::Vector at{};
    if (CarCoors(u.car, at)) {
        script::Command(kSiren, {u.car, 0});
        script::Command(kCarNoLongerNeeded, {u.car});
    }
    logfile::Line("services: vehicle %d %s after %llus", u.model, why, (GetTickCount64() - u.sent) / 1000);
}

// Done here: the crew back into their vehicle, then off on their way.
void Leave(Dispatched& u) {
    if (u.guarding) {
        for (int ped : {u.driver, u.partner}) {
            if (Alive(ped)) script::Command(kLeaveGroup, {ped});
        }
        u.guarding = false;
    }
    game::Vector at{};
    const bool car = CarCoors(u.car, at);
    if (Alive(u.driver)) {
        script::Command(kClearTasks, {u.driver});
        if (car) script::Command(kEnterAsDriver, {u.driver, u.car, 20000});
    }
    if (Alive(u.partner)) {
        script::Command(kClearTasks, {u.partner});
        if (car) script::Command(kEnterAsPassenger, {u.partner, u.car, 20000, 0});
    }
    if (u.blip) script::Command(kRemoveBlip, {u.blip});
    u.blip = 0;
    SetStage(u, Stage::Leaving);
}

// Police at the scene: with no wanted level, CJ's own protection.
void Police(Dispatched& u, const game::Vector& player, ULONGLONG now) {
    const int group = [] {
        script::Locals l;
        script::Command(kPlayerGroup, {kPlayer, script::Arg::Local(0)}, &l);
        return l.Int(0);
    }();
    if (!u.guarding) {
        for (int ped : {u.driver, u.partner}) {
            if (Alive(ped)) script::Command(kGroupMember, {group, ped});
        }
        u.guarding = true;
        logfile::Line("services: the officers are guarding CJ");
    }
    // Driven off: they see him go and get back to patrol.
    const int me = PlayerHandle();
    if (me && script::Command(kCharInAnyCar, {me})) {
        if (!u.inCarSince) u.inCarSince = now;
    } else {
        u.inCarSince = 0;
    }
    bool anyone = false, close = false;
    for (int ped : {u.driver, u.partner}) {
        game::Vector at{};
        if (!CharCoors(ped, at)) continue;
        anyone = true;
        close = close || Distance2d(at, player) < 120.0f;
    }
    const char* why = !anyone                              ? "both officers down"
                      : now - u.stageAt > kGuardMs         ? "time up"
                      : u.inCarSince && now - u.inCarSince > 4000 ? "CJ drove off"
                      : !close                             ? "CJ left them behind"
                                                           : nullptr;
    if (why) {
        logfile::Line("services: the officers stop guarding CJ - %s", why);
        Leave(u);
    }
}

// Fire at the scene: each firefighter takes an extinguisher to the nearest
// fire nobody else has, sprays it, and puts it out; with none left, back to
// the truck.
void Fire(Dispatched& u, ULONGLONG now) {
    game::Vector scene{};
    if (!CarCoors(u.car, scene) && !CharCoors(u.driver, scene)) {
        Leave(u);
        return;
    }
    int gameCount = 0;
    const std::vector<game::Vector> fires = FiresNear(scene, kFireRadius, gameCount);
    const bool listUnknown = fires.empty() && gameCount > 0;
    bool working = false;
    for (int i = 0; i < 2; ++i) {
        Hand& h = u.hands[i];
        game::Vector at{};
        if (!CharCoors(h.ped, at)) continue;
        if (h.phase == 0) {
            // The nearest fire the other one is not already on.
            const Hand& other = u.hands[1 - i];
            float best = 1e9f;
            game::Vector pick{};
            bool found = false;
            for (const game::Vector& f : fires) {
                if (other.phase != 0 && Distance3d(f, other.fire) < 3.0f) continue;
                const float d = Distance3d(f, at);
                if (d < best) {
                    best = d;
                    pick = f;
                    found = true;
                }
            }
            if (!found && listUnknown) {
                // Where the fires are is not known: the scene round the truck.
                pick = scene;
                found = true;
            }
            if (found) {
                h.fire = pick;
                // A few metres short, on the side the firefighter comes from.
                const float dx = at.x - pick.x, dy = at.y - pick.y, len = std::max(0.1f, std::hypot(dx, dy));
                script::Command(kGoStraightTo, {h.ped, pick.x + dx / len * 3.5f, pick.y + dy / len * 3.5f, pick.z, kRun, 20000});
                h.phase = 1;
                h.at = now;
            }
        } else if (h.phase == 1) {
            if (Distance2d(at, h.fire) < 5.5f || now - h.at > 12000) {
                script::Command(kShootAtCoord, {h.ped, h.fire.x, h.fire.y, h.fire.z + 0.3f, 3500});
                h.phase = 2;
                h.at = now;
            }
        } else if (h.phase == 2 && now - h.at > 3500) {
            // Sprayed: out it goes, whatever the spray's aim made of it.
            script::Command(kExtinguish, {h.fire.x, h.fire.y, h.fire.z, listUnknown ? kFireRadius : 3.0f});
            h.phase = 0;
        }
        working = working || h.phase != 0;
    }
    if (working || !fires.empty() || listUnknown) {
        u.idleSince = 0;
    } else if (!u.idleSince) {
        // Nothing burning: a look round first.
        u.idleSince = now;
        for (const Hand& h : u.hands) {
            if (Alive(h.ped)) script::Command(kLookAbout, {h.ped, 5000});
        }
        logfile::Line("services: no fires left near the scene");
    }
    if ((u.idleSince && now - u.idleSince > 6000) || now - u.stageAt > 150000) Leave(u);
}

// A paramedic at the scene: to CJ, and treats him back to full health.
void Medical(Dispatched& u, const game::Vector& player, ULONGLONG now) {
    const uintptr_t ped = PlayerPed();
    const int me = PlayerHandle();
    if (!ped || !me) {
        Leave(u);
        return;
    }
    float& health = *reinterpret_cast<float*>(ped + kPedHealth);
    const float max = *reinterpret_cast<const float*>(ped + kPedMaxHealth);
    const int medic = Alive(u.driver) ? u.driver : u.partner;
    game::Vector at{};
    if (!CharCoors(medic, at)) {
        Leave(u);
        return;
    }
    const int helper = medic == u.driver ? u.partner : u.driver;
    const float d = Distance2d(at, player);
    if (u.phase == 0) {
        if (health >= max - 0.5f) {
            // Nothing to treat: a look round, then off.
            if (!u.phaseAt) {
                u.phaseAt = now;
                script::Command(kLookAbout, {medic, 5000});
                logfile::Line("services: CJ needs no treatment");
            } else if (now - u.phaseAt > 6000) {
                Leave(u);
            }
            return;
        }
        u.phaseAt = 0;
        if (d <= 2.2f && script::Command(kCharOnFoot, {me})) {
            script::Command(kFaceChar, {medic, me});
            script::Command(kPlayAnim, {medic, "IDLE_chat", "ped", 4.0f, true, false, false, false, -1});
            if (Alive(helper)) script::Command(kLookAtChar, {helper, me, static_cast<int>(kTreatMs)});
            u.phase = 1;
            u.phaseAt = now;
            u.healFrom = health;
            logfile::Line("services: the paramedic is treating CJ (%.0f of %.0f)", health, max);
        } else if (now - u.lastOrder > 3000) {
            u.lastOrder = now;
            script::Command(kGoToChar, {medic, me, 20000, 1.5f});
        }
    } else {
        if (d > 4.0f) {
            // He walked off mid-treatment: after him.
            script::Command(kClearTasks, {medic});
            u.phase = 0;
            return;
        }
        const float t = std::min(1.0f, static_cast<float>(now - u.phaseAt) / kTreatMs);
        health = std::max(health, u.healFrom + (max - u.healFrom) * t);
        if (t >= 1.0f) {
            health = max;
            script::Command(kClearTasks, {medic});
            logfile::Line("services: CJ treated");
            Leave(u);
        }
    }
    if (now - u.stageAt > 120000) Leave(u);
}

// The rider at CJ: walks up, hands the meal over, and CJ eats it.
void Delivery(Dispatched& u, const game::Vector& player, ULONGLONG now) {
    const uintptr_t ped = PlayerPed();
    const int me = PlayerHandle();
    game::Vector at{};
    if (!ped || !me || !CharCoors(u.driver, at)) return;  // Track sees the rider gone
    script::Command(kRequestAnims, {"DEALER"});
    const float d = Distance2d(at, player);
    if (u.phase == 0) {
        const bool ready = script::Command(kCharOnFoot, {me}) && script::Command(kAnimsLoaded, {"DEALER"});
        if (d <= 1.8f && ready) {
            script::Command(kFaceChar, {u.driver, me});
            script::Command(kFaceChar, {me, u.driver});
            script::Command(kPlayAnim, {u.driver, "DEALER_DEAL", "DEALER", 4.0f, false, false, false, false, -1});
            script::Command(kPlayAnim, {me, "DRUGS_BUY", "DEALER", 4.0f, false, false, false, false, -1});
            u.phase = 1;
            u.phaseAt = now;
        } else if (now - u.lastOrder > 3000) {
            u.lastOrder = now;
            script::Command(kGoToChar, {u.driver, me, 20000, 1.2f});
        }
    } else if (now - u.phaseAt > 2500) {
        // Eaten, as in the shop: health back to full, and the meal's
        // calories on him.
        const Meal& meal = kMenus[u.restaurant][u.meal];
        float& health = *reinterpret_cast<float*>(ped + kPedHealth);
        health = std::max(health, *reinterpret_cast<const float*>(ped + kPedMaxHealth));
        if (meal.calories > 0.0f) script::Command(kAddFloatStat, {kStatCalories, meal.calories});
        u.delivered = true;
        logfile::Line("services: %s delivered a %s", kRestaurantNames[u.restaurant], meal.name);
        Leave(u);
    }
}

void Refund(Dispatched& u, const char* why) {
    if (u.kind != Kind::Delivery || u.delivered || !u.price) return;
    script::Command(kAddMoney, {kPlayer, u.price});
    Text(u.restaurant, std::string("Sorry, we couldn't get your ") + kMenus[u.restaurant][u.meal].name + " to you" +
                           why + ". Your $" + std::to_string(u.price) + " has been refunded.");
    u.price = 0;
}

void Track() {
    if (g_units.empty()) return;
    const ULONGLONG now = GetTickCount64();
    const game::Vector player = PlayerCoors();
    const uintptr_t playerPed = PlayerPed();
    const bool playerAlive = playerPed && *reinterpret_cast<const float*>(playerPed + kPedHealth) > 0.0f;
    for (size_t i = 0; i < g_units.size();) {
        Dispatched& u = g_units[i];
        bool done = false;
        game::Vector car{};
        const bool carThere = CarCoors(u.car, car);
        const bool crew = Alive(u.driver) || Alive(u.partner);
        if (!playerAlive) {
            Refund(u, "");
            Release(u, "released - CJ is down");
            done = true;
        } else if (!crew) {
            Refund(u, "");
            Release(u, "lost its crew");
            done = true;
        } else if (u.stage == Stage::Driving) {
            if (!carThere || !Alive(u.driver)) {
                Refund(u, "");
                Release(u, "was stopped on the way");
                done = true;
            } else if (Distance2d(car, player) < (u.kind == Kind::Delivery ? kDeliveryArrived : kArrivedDistance) &&
                       (u.kind != Kind::Delivery || !script::Command(kCharInAnyCar, {PlayerHandle()}))) {
                for (int ped : {u.driver, u.partner}) {
                    if (Alive(ped)) script::Command(kLeaveCar, {ped});
                }
                if (u.blip) script::Command(kRemoveBlip, {u.blip});
                u.blip = 0;
                if (u.kind == Kind::Delivery) {
                    // The rider on the radar, to meet.
                    script::Locals l;
                    script::Command(kBlipForChar, {u.driver, script::Arg::Local(0)}, &l);
                    u.blip = l.Int(0);
                }
                SetStage(u, Stage::GettingOut);
                logfile::Line("services: vehicle %d arrived after %llus", u.model, (now - u.sent) / 1000);
            } else if (now - u.sent > (u.kind == Kind::Delivery ? kDeliveryGiveUpMs : kGiveUpMs)) {
                Refund(u, " in time");
                Release(u, "did not arrive and was released");
                done = true;
            } else {
                if (now - u.lastRedirect > kRedirectMs) {
                    // The caller may have moved.
                    u.lastRedirect = now;
                    script::Command(kDriveTo, {u.car, player.x, player.y, player.z});
                }
                if (now - u.lastLog > 5000) {
                    u.lastLog = now;
                    logfile::Line("services: vehicle %d is %.0f from the caller", u.model, Distance2d(car, player));
                }
            }
        } else if (u.stage == Stage::GettingOut) {
            bool out = true;
            for (int ped : {u.driver, u.partner}) {
                if (Alive(ped) && !script::Command(kCharOnFoot, {ped})) out = false;
            }
            if (out || now - u.stageAt > 5000) {
                if (u.kind == Kind::Police && Wanted() > 0) {
                    // He is wanted: they are the police, and come for him.
                    logfile::Line("services: CJ is wanted - the officers are left to the police's own work");
                    Release(u, "handed to the police");
                    done = true;
                } else {
                    if (u.kind == Kind::Fire) {
                        u.hands[0].ped = u.driver;
                        u.hands[1].ped = u.partner;
                        for (int ped : {u.driver, u.partner}) {
                            if (!Alive(ped)) continue;
                            script::Command(kGiveWeapon, {ped, kExtinguisher, 10000});
                            script::Command(kCurrentWeapon, {ped, kExtinguisher});
                        }
                    }
                    SetStage(u, Stage::Working);
                }
            }
        } else if (u.stage == Stage::Working) {
            switch (u.kind) {
                case Kind::Police: Police(u, player, now); break;
                case Kind::Fire: Fire(u, now); break;
                case Kind::Medical: Medical(u, player, now); break;
                case Kind::Delivery:
                    if (now - u.sent > kDeliveryGiveUpMs) {
                        Refund(u, " - we couldn't find you");
                        Leave(u);
                    } else {
                        Delivery(u, player, now);
                    }
                    break;
            }
        } else {  // Leaving
            bool in = carThere;
            if (carThere) {
                for (int ped : {u.driver, u.partner}) {
                    if (Alive(ped) && !script::Command(kCharInCar, {ped, u.car})) in = false;
                }
            }
            if (in || now - u.stageAt > 25000) {
                if (in) {
                    // Off at a normal pace. Wandering the car alone left the
                    // driver's own drive to the caller in charge - and a car
                    // at the end of its drive only creeps. The driver is
                    // given the wander as a task of their own instead, kept
                    // after the phone lets them go.
                    const float speed = u.kind == Kind::Delivery ? kDeliveryCruise : 25.0f;
                    script::Command(kCruiseSpeed, {u.car, speed});
                    script::Command(kWander, {u.car});
                    if (Alive(u.driver)) {
                        script::Command(kDriveWander, {u.driver, u.car, speed, kDriveAvoidCars});
                        script::Command(kKeepTask, {u.driver, true});
                    }
                }
                Release(u, in ? "left the scene" : "was released at the scene");
                done = true;
            }
        }
        if (done) g_units.erase(g_units.begin() + i);
        else ++i;
    }
}

}  // namespace

void Request(Unit unit) {
    if (g_queue.size() < 4) g_queue.push_back(unit);
    logfile::Line("services: %s requested", unit == Unit::Police ? "police" : unit == Unit::Fire ? "fire" : "medical");
}

const Meal& MealOf(Restaurant restaurant, int meal) {
    return kMenus[static_cast<int>(restaurant)][std::clamp(meal, 0, kMeals - 1)];
}

const char* NameOf(Restaurant restaurant) { return kRestaurantNames[static_cast<int>(restaurant)]; }

Order Deliver(Restaurant restaurant, int meal) {
    const int r = static_cast<int>(restaurant);
    if (!Reachable()) return Order::Unreachable;
    for (const Dispatched& u : g_units) {
        if (u.kind == Kind::Delivery && u.restaurant == r) return Order::Busy;
    }
    const Meal& m = MealOf(restaurant, meal);
    if (Money() < m.price) return Order::NoMoney;
    Dispatched order;
    order.restaurant = r;
    order.meal = std::clamp(meal, 0, kMeals - 1);
    order.price = m.price;
    if (!Dispatch(Kind::Delivery, kRiders[r], PlayerCoors(), &order)) return Order::Unreachable;
    script::Command(kAddMoney, {kPlayer, -m.price});
    logfile::Line("services: %s ordered from %s for $%d", m.name, kRestaurantNames[r], m.price);
    return Order::Sent;
}

bool Reachable() {
    game::Point p{};
    return game::PlayerInterior() == 0 && game::PlayerPosition(p);
}

void SetMessenger(void (*text)(const char*, const char*)) { g_messenger = text; }

void SetNumbers(const char* pizza, const char* burger, const char* chicken) {
    g_numbers[0] = pizza ? pizza : "";
    g_numbers[1] = burger ? burger : "";
    g_numbers[2] = chicken ? chicken : "";
}

void Process() {
    Track();
    if (g_queue.empty()) return;
    const Unit unit = g_queue.front();
    g_queue.pop_front();
    if (!Reachable()) {
        logfile::Line("services: the caller is indoors - nothing sent");
        return;
    }
    const game::Vector player = PlayerCoors();
    const City city = CityHere();
    switch (unit) {
        case Unit::Police: Dispatch(Kind::Police, kPolice[city], player); break;
        case Unit::Fire: Dispatch(Kind::Fire, kFireTruck[city], player); break;
        case Unit::Medical: Dispatch(Kind::Medical, kAmbulance[city], player); break;
    }
}

}  // namespace services
