// The day (`cosmos.cpp`, `docs/09-simulation.md` "The daily turn"): the
// next-day command's contested-star walk, `Cosmos_end_day` and its battle
// resolution, ranking, controlled lanes and game end, plus the cosmos
// module's star and ship helpers the other modules call.
//
// The user interface ends a day as the next-turn command does:
//
//     while (const StarIndex star = cosmos_next_contested_star(session); star != kNone) {
//       // event 19: the player either fights at `star` (System Display) or
//       // moves on to the next day anyway (the loop continues).
//     }
//     cosmos_end_day(session);
//     // then show `session.events.take()`.
//
// (`cosmos_next_turn` does exactly this when the player moves on at every
// contested star.)
#pragma once

#include <cstdint>
#include <vector>

#include "game/address.h"
#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// Sets the owner of `planet` (a planet of `star`) to `race` (0xff: none):
// the star's owner bits (home bit cleared when the old owner loses its last
// planet at its home), the project cancelled (a ship in production
// destroyed, the project square emptied except for Automation), progress 0
// (`Star_set_planet_owner` 0x1d234).
void star_set_planet_owner(Session& session, Star& star, Planet& planet, std::uint8_t race);

// Puts `ship` in `star`: from a lane, at the lane end plus `(rand() % 200,
// -200, rand() % 200)` (z drawn first), location kind 4; from space (kind
// 4), only the star changes; any other kind keeps its location. Then the
// race's bits in the star's ships and explored masks and in every lane of
// the star (`Ship_enter_star_system` 0x1d3e8, third argument always 0).
void ship_enter_star_system(Session& session, Star& star, Ship& ship);

// Takes `ship` out of its star (undocking it first), clearing the race's
// ships bit when no other ship of the race remains there
// (`Ship_leave_star_system` 0x1d538).
void ship_leave_star_system(Session& session, Ship& ship);

// Sends every ship of the player at `star` through `lane` into the far star
// at once (`Star_send_player_ships_to_lane` 0x1d654, CosWnd mode +0x582).
void star_send_player_ships_to_lane(Session& session, Star& star, Lane& lane);

// Returns the races with a colony or ships at `star`, in index order
// (`Star_list_present_races` 0x1d734).
std::vector<RaceIndex> star_list_present_races(const Cosmos& cosmos, const Star& star);

// Returns the used ship slots located at `star`: in space there, or built,
// refitted or in orbit at one of its planets, in slot order
// (`Cosmos_ships_at_star` 0x1d794).
std::vector<ShipIndex> cosmos_ships_at_star(const Cosmos& cosmos, const Star& star);

// Tells whether a ship on one of the star's lanes, at this star's end, is
// at war with `race`, the player's or `race`'s own (`Star_lane_end_guarded`
// 0x1d834).
bool star_lane_end_guarded(const Session& session, const Star& star, RaceIndex race);

// Takes a free ship slot for `race` (`Ship_init`, owner, `ship_count + 1`),
// nothing when the 107 slots are used (`Cosmos_alloc_ship` 0x1fda8).
Ship* cosmos_alloc_ship(Session& session, RaceIndex race);
// Frees the slot of `ship` (`Cosmos_free_ship` 0x1fe44).
void cosmos_free_ship(Session& session, Ship& ship);

// Counts the lanes reachable from `star` through stars the player explored
// that hold no colony or ship of a non-allied other race, adding them to
// `Cosmos::controlled_lanes` (`Cosmos_flood_controlled_lanes` 0x1f704).
void cosmos_flood_controlled_lanes(Session& session, Star& star);

// Sets the ending and score when the game is over (`Cosmos_check_game_over`
// 0x1fec4), dropping the queued game events; returns whether it is. (The
// original then posts the change to the end-of-game screen 9, which the
// user interface does when `cosmos_end_day` sets the ending.)
bool cosmos_check_game_over(Session& session);

// Returns the next star not yet visited today where the player and a race
// at war with the player have ships, one of them with power, `kNone` when
// none is left (`Cosmos_next_contested_star` 0x20260).
StarIndex cosmos_next_contested_star(Session& session);

// Ends the day (`Cosmos_end_day` 0x203b4): battles, research, planets,
// ranking, the races' days, controlled lanes, day + 1 and the game-end test.
// The day's events go to `session.events`.
void cosmos_end_day(Session& session);

// The next-turn command (`WinMgr_dispatch_messages` 0x51290, commands 7
// and 8) when the player moves on at every contested star: every
// `cosmos_next_contested_star`, then `cosmos_end_day`.
void cosmos_next_turn(Session& session);

// Returns whether a ship of a race other than `race` and not allied to it
// is on `lane` (the ship scan of `Cosmos_flood_controlled_lanes` 0x1f704 and
// ANTAG `Cosmos_flood_frontier_lanes` 0x1fdf0). Original bug, fixed in
// peCustom: the scan stops after `ship_count` used slots, so the ship in the
// last used slot is missed.
bool lane_blocked(const Session& session, LaneIndex lane, const Race& race);

// peCustom: notes each ship's star and its first day there
// (`Session::ship_arrival_star`, `ship_arrival_day`).
void cosmos_note_ship_arrivals(Session& session);

// Ranks the races by ship count + colonies (`Cosmos_rank_races` 0x2078c).
void cosmos_rank_races(Session& session);

// Fights at every star where the player has neither colony nor ship
// (`Cosmos_resolve_ai_battles` 0x20890), then at every other star with the
// player's standing orders (`Cosmos_resolve_remaining_battles` 0x20a34).
void cosmos_resolve_ai_battles(Session& session);
void cosmos_resolve_remaining_battles(Session& session);

// Resolves the turns of every race present at `star` until none acts
// (`Star_resolve_combat` 0x2090c); the player takes part when
// `include_player` (or in autoplay).
void star_resolve_combat(Session& session, Star& star, bool include_player);

}  // namespace oa::game
