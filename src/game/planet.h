// Planet surfaces and outputs (`planet.cpp`): the parts the galaxy
// generation uses. Squares live in `Cosmos::squares`; a planet owns
// `square_count` consecutive ones from `first_square` (surface squares
// first, then the 10 orbital ones).
#pragma once

#include <cstdint>
#include <span>

#include "game/address.h"
#include "game/session.h"
#include "game/state.h"
#include "game/tables.h"

namespace oa::game {

// No square (the original's 0xffff square index).
inline constexpr std::uint16_t kNoSquare = 0xffff;

// Returns the squares of `planet`.
std::span<Square> planet_squares(Cosmos& cosmos, const Planet& planet);
std::span<const Square> planet_squares(const Cosmos& cosmos, const Planet& planet);

// Lays out and colours a new surface at `first` (size clamped to 4, type
// set to 2 when out of range) and resets the planet's economy, owner and
// project: `Planet_init_surface` 0x33210 (one draw per surface square, then
// the ruins draws). Returns the planet's square count.
std::uint16_t planet_init_surface(Session& session, Planet& planet, std::uint16_t size, std::uint16_t type,
                                  SquareIndex first);

// Recomputes the free-square counts, the outputs, the population limits and
// the weapon/shield counts from the squares: `Planet_recalc_output` 0x34034.
// Antagonizer (ANTAG 0x34e70): outside autoplay, a planet not owned by the
// player gets industry x1.3 (Neutral, Hostile) and research x1.3 (Hostile),
// and the Tractor Beam counts as a shield.
void planet_recalc_output(const Session& session, Planet& planet);
// The same from `squares` (the planet's, or a planner's copy of them);
// `industry_sum`, if given, gets the industry before the output curve.
void planet_recalc_output(const Session& session, Planet& planet, std::span<const Square> squares,
                          std::int32_t* industry_sum = nullptr);
// The output curve of industry and prosperity before the rounding down:
// `(sum + 1) ** 0.85`, 0 for 0.
double planet_output_curve(std::int32_t sum);

// Returns the first square holding `item`, `kNoSquare` if none
// (`Planet_find_item` 0x34a1c).
std::uint16_t planet_find_item(const Cosmos& cosmos, const Planet& planet, std::uint8_t item);
// Returns the surface square at (`row`, `column`) of the planet's shape,
// `kNoSquare` outside it (`Planet_square_at` 0x34a54).
std::uint16_t planet_square_at(const PlanItemTable& items, const Planet& planet, std::int32_t row, std::int32_t column);
// Returns `row << 16 | column` of surface square `square`, -1 for an
// orbital square (`Planet_square_rowcol` 0x34aec).
std::int32_t planet_square_rowcol(const PlanItemTable& items, const Planet& planet, std::uint16_t square);

// Tells whether `item` may be placed on square `square` (the race's
// research, the free population, the square's kind, colour and ruins, and the
// adjacency of surface buildings): `Planet_can_place_item` 0x3379c.
bool planet_can_place_item(const Session& session, const Planet& planet, std::uint8_t item, std::uint16_t square);
// The same on `squares` (the planet's, or a planner's copy of them).
bool planet_can_place_item(const Session& session, const Planet& planet, std::span<const Square> squares,
                           std::uint8_t item, std::uint16_t square);

// Returns whether the planet's owner can start a ship here: a built Shipyard
// and a positive ship allowance (`Planet_can_build_ship` 0x34f2c).
bool planet_can_build_ship(const Session& session, const Planet& planet);

// Places `item` on empty square `square`, built at once, when it is a
// surface item on a surface square (an original bug rejects orbital items)
// and the free population suffices; sets the weapon shots; then recomputes
// the outputs. Returns whether it was placed (`Planet_set_square_item`
// 0x33cd0, mode 0).
bool planet_build_item_now(Session& session, Planet& planet, std::uint16_t square, std::uint8_t item);

// Returns a random empty square accepting `item` (`rand() % candidates`,
// no draw without candidates), `kNoSquare` if none
// (`Planet_pick_random_free_square` 0x33bec). Antagonizer: the first best
// scored candidate, no draw (`Planet_pick_scored_free_square` ANTAG 0x347cc).
std::uint16_t planet_pick_random_free_square(Session& session, const Planet& planet, std::uint8_t item);

// Picks a random free square for `item` and builds it there at once:
// `Planet_auto_place_item` 0x33ca8 with mode -1. Returns the square.
std::uint16_t planet_auto_build_item(Session& session, Planet& planet, std::uint8_t item);

// Mode of `planet_set_square_item` (`Planet_set_square_item` 0x33cd0).
enum class SquareMode : std::uint8_t {
  kPlaceBuilt = 0,  // place the item finished (instant build)
  kRemove = 1,      // demolish / empty the square
  kProject = 2,     // make the item the planet's project
  kForceProject = 3,
};

// Applies `mode` to square `square` (0xffff for a planet-wide project) with
// `item`, then recomputes the outputs; returns whether it was done
// (`Planet_set_square_item` 0x33cd0).
bool planet_set_square_item(Session& session, Planet& planet, std::uint16_t square, std::uint8_t item, SquareMode mode);

// Applies `planet_set_square_item` when `square` is not `kNoSquare` and
// empty: mode -1 places the item built, else it becomes the project
// (`Planet_place_item_if_free` 0x33ba8).
void planet_place_item_if_free(Session& session, Planet& planet, std::uint8_t item, std::uint16_t square,
                               std::int32_t mode);

// Picks a free square for `item` and applies `planet_place_item_if_free`
// with `mode` (-1 built, 0 project); returns the square
// (`Planet_auto_place_item` 0x33ca8).
std::uint16_t planet_auto_place_item(Session& session, Planet& planet, std::uint8_t item, std::int32_t mode);

// The planet's day: weapon shots, production and project completion,
// growth, outputs and the rotation around the star
// (`Planet_daily_update` 0x343f8).
void planet_daily_update(Session& session, Planet& planet);

// Returns the industry cost of `item` on `square` (re-read from the square
// when given; 0xffff for an empty square), a ship's design or refit cost
// for the Ship item (`Planet_project_cost` 0x34968).
std::uint32_t planet_project_cost(const Session& session, const Planet& planet, std::uint16_t square,
                                  std::uint8_t item);
// Returns the days left of the current project (`Planet_project_eta`
// 0x349e8).
std::int32_t planet_project_eta(const Session& session, const Planet& planet);

// Returns the ship slot kept in the flags of the Ship square `square`
// (`Planet_square_ship` 0x34b5c) and stores it (`Planet_set_square_ship`
// 0x34bf0; orbital squares only).
ShipIndex planet_square_ship(const Session& session, const Planet& planet, std::uint16_t square);
void planet_set_square_ship(Session& session, Planet& planet, std::uint16_t square, ShipIndex ship);

// Docks `ship` in the first free orbital square unless the planet is
// another race's and guarded (`Planet_dock_ship` 0x34ca0); returns whether
// it docked.
bool planet_dock_ship(Session& session, Planet& planet, Ship& ship);
// Undocks `ship` into space next to the planet (3 draws, at 250 from it);
// returns whether it was docked here (`Planet_undock_ship` 0x34d24).
bool planet_undock_ship(Session& session, Planet& planet, Ship& ship);

// Tells whether an owner's ship is docked or a built Orbital Shield guards
// the orbit (`Planet_has_orbital_guard` 0x34e90).
bool planet_has_orbital_guard(const Session& session, const Planet& planet);

// Returns the AI colony value of the surface: 2 per red, green or blue
// square, 1 per white one and, with `count_black`, per black one
// (`Planet_habitability` 0x34fb8).
std::int32_t planet_habitability(const Cosmos& cosmos, const Planet& planet, bool count_black);

// The hard-coded planetary weapons (`PlanItem_weapon_stats` 0x34ffc);
// other items leave the outputs unchanged.
struct PlanetWeaponStats {
  std::int32_t range = 0;  // gizmo units (x32)
  std::int32_t damage = 0;
  std::int32_t shots = 0;  // per day
};
void planitem_weapon_stats(std::uint8_t item, PlanetWeaponStats& stats);

// Checks (`apply` false) or fires (`apply` true) the planet weapon of
// square `fire_square` at the ship `fire_target`, the result in
// `fire_result` (`Planet_fire_weapon` 0x350a0); returns the result.
std::uint32_t planet_fire_weapon(Session& session, Planet& planet, bool apply);

// Bombards the planet with `damage` by `race` (`Planet_bombard` 0x35390).
void planet_bombard(Session& session, Planet& planet, std::int32_t damage, RaceIndex race);

// Invades the planet with the ship docked on square `ship_square`; `used`
// receives the Invasion Modules used; returns whether the planet was taken
// (`Planet_invade` 0x35604).
bool planet_invade(Session& session, Planet& planet, std::uint16_t ship_square, std::int32_t& used);

// Gives the planet up: no owner, every square but the Ship ones emptied,
// no population (`Planet_abandon` 0x35970).
void planet_abandon(Session& session, Planet& planet);

}  // namespace oa::game
