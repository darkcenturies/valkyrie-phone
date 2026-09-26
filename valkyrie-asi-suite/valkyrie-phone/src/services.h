// The numbers that answer: what happens in the world when the player calls
// them.
//
// 911 sends the game's own emergency vehicles, made the way a mission script
// makes a unit: a road found out of sight, the vehicle created there with a
// crew from that city's force, the siren on, and a drive to the caller that
// follows them if they move. Then they do their job:
//
// - Police: with no wanted level, the officers join CJ's group - the game's
//   own recruits' behaviour - and so follow him and fight whoever attacks
//   him, for a few minutes or until he drives off. With a wanted level they
//   are simply police, and come for him.
// - Fire: the firefighters take their extinguishers to every fire burning
//   near the scene and put them out.
// - Medical: a paramedic comes to CJ and treats him back to full health.
//
// The restaurants deliver: a rider on a Pizzaboy brings the meal, hands it
// over, and CJ eats it as he would in the shop - the prices and calories are
// the game's own, from data\shopping.dat.
#pragma once

namespace services {

enum class Unit { Police, Fire, Medical };

// Ask for a unit to come to the player. Queued; the work happens in Process().
void Request(Unit unit);

enum class Restaurant { Pizza, Burger, Chicken };
constexpr int kMeals = 4;
struct Meal {
    const char* name;
    int price;
    float calories;  // added to the Calories stat, as the shop adds them
};
const Meal& MealOf(Restaurant restaurant, int meal);
const char* NameOf(Restaurant restaurant);

enum class Order { Sent, NoMoney, Unreachable, Busy };
// Pay for a meal and send it out. The money is given back if it never
// arrives.
Order Deliver(Restaurant restaurant, int meal);

// Whether the player is somewhere a unit can drive to. Interiors are rooms
// built far away from the streets, so nothing can reach the player there.
bool Reachable();

// How the phone is told what happened to an order: a text from `number`.
void SetMessenger(void (*text)(const char* number, const char* body));
// The restaurants' numbers, for the texts to come from.
void SetNumbers(const char* pizza, const char* burger, const char* chicken);

// Run on the game thread, from the frame hook, outside the draw.
void Process();

}  // namespace services
