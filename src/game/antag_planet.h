// The Antagonizer's planet management (`docs/10-antagonizer-ai.md` "Planet
// management and planet-module changes"), also run for the player's
// self-managed planets in that variant, and the planet valuation helpers its
// strategic AI uses ("Planet valuation helpers", "Research helpers").
// Addresses are `ANTAG.EXE`'s.
#pragma once

#include <cstdint>

#include "game/address.h"
#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// Tells whether `race` knows every research item (`Race_research_complete`
// 0x40590). The result is cached for the next calls, whatever the race,
// until `Session::research_complete_valid` is cleared (`Race_turn_update`
// does before each race's AI turn).
bool antag_race_research_complete(Session& session, const Race& race);

// Chooses the project of an idle planet (`Race_ai_manage_planet` 0x3d8f0):
// free basic buildings, orbital defences and Shipyard, specials,
// Terraforming / Transport Tubes, the focus building, or the rebuild of the
// least useful square.
void antag_race_ai_manage_planet(Session& session, Race& race, Planet& planet);
// The orbital part: returns 1 when an order was given, else 0
// (`Race_ai_choose_orbital_project` 0x3e1cc).
std::int32_t antag_race_ai_choose_orbital_project(Session& session, Race& race, Planet& planet);
// Demolishes the cheapest replaceable orbital defence and sets `item` there
// as the project; returns whether it did (`Race_ai_replace_orbital_defence`
// 0x3e020).
bool antag_race_ai_replace_orbital_defence(Session& session, Race& race, Planet& planet, std::uint8_t item);
// Starts a special building; returns whether it did (or claims it did, see
// the code) (`Race_ai_planet_specials` 0x3e520).
bool antag_race_ai_planet_specials(Session& session, Race& race, Planet& planet);
// Returns 0 industry, 1 research, 2 prosperity, 3 housing
// (`Race_ai_planet_focus` 0x3e800).
std::int32_t antag_race_ai_planet_focus(Session& session, Race& race, const Planet& planet);
// Replaces the least useful surface building with what the planet needs
// most; returns whether an order was given (`Race_ai_rebuild_planet_square`
// 0x3cce4).
bool antag_race_ai_rebuild_planet_square(Session& session, Race& race, Planet& planet);

// Demolishes the research buildings of the race's AI-run planets once its
// research is complete; returns whether any was demolished
// (`Race_ai_demolish_research_buildings` 0x405f4).
bool antag_race_ai_demolish_research_buildings(Session& session, Race& race);
// Demolishes the built Laboratories and Research Campuses of the surface
// (`Planet_demolish_research_buildings` 0x3623c).
bool planet_demolish_research_buildings(Session& session, Planet& planet);

// Counts the surface squares holding an item other than the Colony Base
// (`Planet_count_buildings` 0x358f0).
std::int32_t planet_count_buildings(const Cosmos& cosmos, const Planet& planet);
// The free squares where a Factory could go, by colour (blue -> research,
// red -> industry, green -> prosperity, others count for all three)
// (`Planet_count_free_squares_by_colour` 0x34a44).
struct FreeSquaresByColour {
  std::int32_t research = 0;
  std::int32_t industry = 0;
  std::int32_t prosperity = 0;
};
FreeSquaresByColour planet_count_free_squares_by_colour(const Session& session, const Planet& planet);
// Counts (up to 100) the empty surface squares without ruins, not black
// (unless the owner is Orfa) / black, next to a built square
// (`Planet_count_buildable_squares` 0x3407c,
// `Planet_count_buildable_black_squares` 0x3420c).
std::int32_t planet_count_buildable_squares(const Session& session, const Planet& planet);
std::int32_t planet_count_buildable_black_squares(const Session& session, const Planet& planet);
// Returns the best orbital shield (27, 26) / weapon (30, 29, 28) the owner
// knows, 0xff if none (`Planet_best_orbital_shield_item` 0x35da4,
// `Planet_best_orbital_weapon_item` 0x35e24).
std::uint8_t planet_best_orbital_shield_item(const Session& session, const Planet& planet);
std::uint8_t planet_best_orbital_weapon_item(const Session& session, const Planet& planet);
// Strengths of the built defences: orbital shields 26: 3, 27: 6; orbital
// weapons 28: 2, 29: 4, 30: 6; surface shields 18: 2, 19: 4
// (`Planet_orbital_shield_strength` 0x35fe0, `Planet_orbital_weapon_strength`
// 0x36050, `Planet_surface_shield_strength` 0x36158).
std::int32_t planet_orbital_shield_strength(const Cosmos& cosmos, const Planet& planet);
std::int32_t planet_orbital_weapon_strength(const Cosmos& cosmos, const Planet& planet);
std::int32_t planet_surface_shield_strength(const Cosmos& cosmos, const Planet& planet);
// Rates the built orbital defences: 26: 1, 27: 2, 28: 2, 29: 4, 30: 6
// (`Planet_orbital_defence_rating` 0x360d8).
std::int32_t planet_orbital_defence_rating(const Cosmos& cosmos, const Planet& planet);
// peCustom: the number of built orbital shields and weapons.
std::int32_t planet_orbital_defence_count(const Cosmos& cosmos, const Planet& planet);
// The value of `planet` as a target for `race`: an enemy planet's worth, an
// unowned planet's habitability above 70, else 0 (`Planet_ai_target_value`
// 0x364b4).
std::int32_t planet_ai_target_value(const Session& session, const Planet& planet, RaceIndex race);
// The value of an owned planet to its owner (`Planet_ai_own_value` 0x363b0).
std::int32_t planet_ai_own_value(const Session& session, const Planet& planet);

}  // namespace oa::game
