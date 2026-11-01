// The player's actions of the planet, research and planet-list screens
// (`docs/11-screens.md`) that the original performs inline in its window
// code (`PlanWnd_handle_event` 0x35e98, `PlSquareWnd_handle_event` 0x3876c,
// `ResWnd_handle_event` 0x42a84), on top of the game core's functions
// (`planet_set_square_item`, `ship_destroy`, ...).
#pragma once

#include <cstdint>
#include <string_view>

#include "game/address.h"
#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// The square dialog's Abandon (command 0x35): destroys the square's ship,
// if any, then removes the item (`planet_set_square_item` remove).
void planet_abandon_square(Session& session, Planet& planet, std::uint16_t square);

// The system view's Abandon of the player's ship: destroys it. In peCustom
// a ship in orbit of a planet of its owner first returns its crew there:
// the population a ship takes when it is launched, at most up to the
// planet's maximum.
void ship_abandon(Session& session, Ship& ship);

// The planet screen's M key: toggles `planet.self_managed` (0 / -1).
void planet_toggle_self_managed(Planet& planet);

// What `planet_colonize` did, for its report (help topic `colonize`, or
// `maxcolonize` when `best_race` is the player): the player's colony count
// afterwards and the race with the most colonies among the player and the
// races it has met (the first one on a tie) with its count.
struct ColonizeResult {
  std::int32_t player_colonies = 0;
  RaceIndex best_race = 0;
  std::int32_t best_colonies = 0;
};

// Colony Base placement (`PlanWnd_handle_event`, left click with the
// selected item 5): removes the Colonizer (gizmo 71) of the ship on
// `ship_square`, names the unowned planet `name` (at most 20 characters, as
// the input box), makes it the ship owner's with population 2, acquired
// today, the owner's bit in the star's owner mask, and places a built Colony
// Base on `square`. Throws `std::logic_error` when the planet is owned (the
// original's assert) or no race has a colony afterwards.
ColonizeResult planet_colonize(Session& session, Planet& planet, std::uint16_t ship_square, std::uint16_t square,
                               std::string_view name);

// A click on an item of the research screen: makes `item` the research
// project of `race`; the progress is kept.
void research_set_project(Research& research, RaceIndex race, std::uint8_t item);

// The research screen's M key: toggles the player's self-managed research
// (`Research::self_managed`, 0 / -1).
void research_toggle_self_managed(Research& research);

// Leaving the research screen: clears `race`'s "newly discovered" bit on
// every item.
void research_clear_unseen(Research& research, RaceIndex race);

}  // namespace oa::game
