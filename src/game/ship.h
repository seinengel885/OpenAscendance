// Ships (`ship.cpp`, `shipgiz.cpp`): hulls and gizmos, the day on the lanes,
// orbits, and the battle actions (`docs/08-combat.md`, "Actions").
//
// A ship's action block (`Ship::action` .. `action_point`) is filled by the
// user interface or the AI, then `ship_do_action(.., false)` checks it and
// `ship_do_action(.., true)` applies it; pointers in it are runtime
// addresses (`game/address.h`).
#pragma once

#include <cstdint>

#include "game/address.h"
#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// `Ship::battle_flags` bits.
enum ShipBattleFlag : std::uint16_t {
  kShipImmobile = 1,       // Molecular Tie Down
  kShipToroidal = 2,       // Toroidal Blaster: +40 moves at each refill
  kShipAccutron = 4,       // range x2
  kShipShieldsDown = 8,    // Shield Blaster
  kShipInvulnerable = 16,  // Invulnerablizer
};

// `Ship::action_result` bits.
enum ShipActionResult : std::uint32_t {
  kActionPossible = 1,
  kActionPenetrated = 2,
  kActionDestroyed = 4,
  kActionReached = 8,
  kActionNoDamage = 0x10,
};

// `Ship::target_kind` values.
enum ShipTargetKind : std::uint8_t {
  kTargetShip = 0,
  kTargetPlanet = 1,
  kTargetLane = 2,
  kTargetPoint = 3,
  kTargetNone = 5,
};

// Gizmo ids met in the day and battle code (`gizmos.txt` order).
inline constexpr std::int8_t kGizmoRecaller = 36;
inline constexpr std::int8_t kGizmoStarLaneDrive = 43;
inline constexpr std::int8_t kGizmoStarLaneHyperdrive = 44;
inline constexpr std::int8_t kGizmoAccutron = 57;
inline constexpr std::int8_t kGizmoReplenisher = 65;
inline constexpr std::int8_t kGizmoGyroInductor = 67;
inline constexpr std::int8_t kGizmoColonizer = 71;
inline constexpr std::int8_t kGizmoInvasionModule = 73;

// Resets `ship` to an empty hull `hull`, owner -1, 1 hit point until the
// hull is set (`Ship_reset_with_hull` 0x444bc: `Ship_init` 0x4470c, the
// Antagonizer also clearing the location kind, then `ship_set_hull_size`).
void ship_init(Session& session, Ship& ship, std::uint8_t hull);

// Sets the hull size and full hit points (x2 for Snovemdomas) and names AI
// ships and unnamed player ships after the hull (`Ship_set_hull_size`
// 0x449a4).
void ship_set_hull_size(Session& session, Ship& ship, std::uint8_t hull);

// Installs `gizmo` in slot `slot` (accepts `slot == slot_count`: original
// off-by-one) and recomputes the stats; returns whether it was installed
// (`Ship_install_gizmo` 0x44b08).
bool ship_install_gizmo(Session& session, Ship& ship, std::int8_t gizmo, std::int32_t slot);
// Empties slot `slot` and recomputes the stats (`Ship_remove_gizmo`
// 0x44b54).
bool ship_remove_gizmo(Session& session, Ship& ship, std::int32_t slot);
// Returns the first slot holding a gizmo of `category` / the gizmo
// `gizmo`, -1 if none (`Ship_find_gizmo_by_category` 0x44b84,
// `Ship_find_gizmo` 0x44bd8).
std::int32_t ship_find_gizmo_by_category(const Session& session, const Ship& ship, std::uint8_t category);
std::int32_t ship_find_gizmo(const Ship& ship, std::int8_t gizmo);

// Moves the gizmos to the first slots and sets the new hull (and its name);
// false when they do not fit (`Ship_change_hull_size` 0x44c18, ship design).
bool ship_change_hull_size(Session& session, Ship& ship, std::uint8_t hull);

// Docks `ship` at `planet`, or places it in space next to it when the orbit
// is full or guarded (3 draws) (`Ship_enter_orbit` 0x44cf0).
void ship_enter_orbit(Session& session, Ship& ship, Planet& planet);
// Tells whether `ship` can travel `lane` (`Ship_can_enter_lane` 0x44e68).
bool ship_can_enter_lane(const Session& session, const Ship& ship, const Lane& lane);
// Puts `ship` on `lane`, leaving its star (`Ship_enter_lane` 0x44ea4).
void ship_enter_lane(Session& session, Ship& ship, Lane& lane);
// Sets the position, snapped to the grid (`Ship_set_position` 0x44f18).
void ship_set_position(Session& session, Ship& ship, const Vec3& position);
// Recomputes the stats from the gizmos (`Ship_recompute_stats` 0x44f3c).
void ship_recompute_stats(Session& session, Ship& ship);
// Finishes a ship's production or refit (`Ship_launch` 0x45084).
void ship_launch(Session& session, Ship& ship);
// Destroys `ship` (undocked first, out of its star, slot freed)
// (`Ship_destroy` 0x4519c).
void ship_destroy(Session& session, Ship& ship);
// Toggles every shield on (`on`) or off (`Ship_set_all_shields` 0x4529c).
void ship_set_all_shields(Session& session, Ship& ship, bool on);
// Toggles the gizmo of slot `slot` (`Ship_toggle_gizmo` 0x452e8).
void ship_toggle_gizmo(Session& session, Ship& ship, std::int32_t slot);

// Checks or applies the ship's action block; the result goes to
// `action_result` and is returned (`Ship_do_action` 0x45398).
std::uint32_t ship_do_action(Session& session, Ship& ship, bool apply);
// The move action (`Ship_battle_move` 0x453c4).
std::uint32_t ship_battle_move(Session& session, Ship& ship, bool apply);
// The gizmo action (`Ship_battle_use_gizmo` 0x457d0).
std::uint32_t ship_battle_use_gizmo(Session& session, Ship& ship, bool apply);

// Returns the industry of the installed gizmos (`Ship_gizmos_industry`
// 0x45844) and the ship's cost, hull included (`Ship_industry_cost`
// 0x4588c).
std::int32_t ship_gizmos_industry(const Session& session, const Ship& ship);
std::int32_t ship_industry_cost(const Session& session, const Ship& ship);
// Returns the known gizmo of `category` with the highest level for `race`
// (last on ties), -1 if none (`Ship_best_gizmo_for_race` 0x458b8).
std::int8_t ship_best_gizmo_for_race(const Session& session, std::uint8_t category, RaceIndex race);
// Tells whether the ship's owner knows the research of `gizmo`
// (`Ship_race_has_gizmo` 0x45950).
bool ship_race_has_gizmo(const Session& session, const Ship& ship, std::int8_t gizmo);
// Sets the action block to fire the first weapon that can hit `target` (a
// lane, planet or ship by `target_kind`: 0 lane, 1..2 planet, 3 ship);
// returns that slot, -1 if none (`Ship_ai_select_weapon` 0x459cc).
std::int32_t ship_ai_select_weapon(Session& session, Ship& ship, std::uint16_t target_kind, RawAddress target);
// Tells whether a weapon of the ship has uses left (`Ship_has_ready_weapon`
// 0x45a74).
bool ship_has_ready_weapon(const Session& session, const Ship& ship);
// Refills the moves and pays the toggled gizmos again
// (`Ship_reset_battle_moves` 0x45ac8).
void ship_reset_battle_moves(Session& session, Ship& ship);
// The ship's day: power, uses, the lane travel and arrival
// (`Ship_new_day` 0x45bbc).
void ship_new_day(Session& session, Ship& ship);
// Returns `clamp((day - built_day) / 100, 0, 10)` (`Ship_experience`
// 0x45ddc) and the day's power (`Ship_battle_power` 0x45e0c).
std::int32_t ship_experience(const Session& session, const Ship& ship);
std::int32_t ship_battle_power(const Session& session, const Ship& ship);
// Whether one of the hull's slots holds `gizmo` (`Ship_has_gizmo` ANTAG
// 0x4a534).
bool ship_has_gizmo(const Ship& ship, std::int8_t gizmo);
// Returns the hops from the ship's star to `star` and, unless it is the
// current star, heads the ship there (`Ship_set_destination_star` 0x45e80).
std::uint8_t ship_set_destination_star(Session& session, Ship& ship, const Star& star);
// Returns whether `race` sees `ship` in space: 0 no, 1 yes, 2 by a ship with
// X Ray Megaglasses (`Ship_detected_by_race` 0x45ef4).
std::int32_t ship_detected_by_race(const Session& session, const Ship& ship, RaceIndex race);

// Applies (`apply`) or checks `damage` by `attacker` to `ship`; returns the
// result bits (`Ship_take_damage` 0x46bb0).
std::uint8_t ship_take_damage(Session& session, Ship& ship, bool apply, std::int32_t damage, RaceIndex attacker);
// The gizmo effects by target kind (`Ship_use_gizmo` 0x46d54,
// `Ship_use_gizmo_on_planet` 0x47c0c, `Ship_use_gizmo_on_lane` 0x47ec8,
// `Ship_use_gizmo_untargeted` 0x48208); each returns the result bits.
std::uint32_t ship_use_gizmo(Session& session, Ship& ship, bool apply);
std::uint32_t ship_use_gizmo_on_planet(Session& session, Ship& ship, bool apply);
std::uint32_t ship_use_gizmo_on_lane(Session& session, Ship& ship, bool apply);
std::uint32_t ship_use_gizmo_untargeted(Session& session, Ship& ship, bool apply);

}  // namespace oa::game
