// Race set-up and counts (`race.cpp`) and the research-state reset
// (`research.cpp`) the galaxy generation and the save header use.
#pragma once

#include <cstdint>
#include <vector>

#include "game/address.h"
#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// Restores the AI thresholds (18, 22, 0), clears the targets, the two
// `BIO_MAX` values, the pending talk (14), attitudes, relations and the
// extinct flag: `Race_reset` 0x39ab8.
void race_reset(Race& race);

// Resets `race`, sets its slot and, only when the species changes, the
// species and its `.dip` values: `Race_init` 0x39b2c.
void race_init(Session& session, Race& race, std::uint8_t index, std::uint8_t species);

// Copies the `.dip` values of the race's species, sets `ability = species`,
// `ability_charge = 0` and the three last-trade days to -10:
// `Race_load_dip_values` 0x3f700.
void race_load_dip_values(Session& session, Race& race);

// Returns the number of ships of `race` among the used slots
// (`Race_list_ships` 0x3c338 without its lists).
std::int32_t race_ship_count(const Cosmos& cosmos, const Race& race);
// Returns the fleet strength of `race`: the sum of its ships' hull size + 1
// (`Race_list_ships` 0x3c338's sum).
std::int32_t race_fleet_strength(const Cosmos& cosmos, const Race& race);
// Returns the number of planets owned by `race` (`Race_count_planets`
// 0x3c3f4).
std::int32_t race_count_planets(const Cosmos& cosmos, const Race& race);
// Returns the number of research items `race` knows
// (`Race_count_discoveries` 0x3c678).
std::int32_t race_count_discoveries(const Research& research, const Race& race);
// Returns how many more ships `race` may build: 1 + the stars it owns alone
// (home excluded) - its ships + 1 while it holds its home star
// (`Race_ship_allowance` 0x3b748).
std::int32_t race_ship_allowance(const Cosmos& cosmos, const Race& race);

// Clears the research masks, projects (-1), progress and the two flags; on
// the session's first call, first copies the research tree
// (`Research_load` 0x41ce0 on the global).
void research_load(Session& session);

// Returns the used ship slots of `race`, collected in slot order then sorted
// by `built_day` with the Watcom `qsort` (`Race_list_ships` 0x3c338; its
// size sum output is not needed).
std::vector<ShipIndex> race_list_ships(const Cosmos& cosmos, const Race& race);

// The race's day (`Race_turn_update` 0x39b50, Standard): allowance
// scrapping, extinction, the ships' day, the AI turn, the ability charge.
// The variant's AI (`game/ai.h`) calls it.
void race_turn_update(Session& session, Race& race);

// Refills the battle moves of the race's ships at `star`
// (`Race_reset_ship_moves_at_star` 0x39e78).
void race_reset_ship_moves_at_star(Session& session, Race& race, const Star& star);

// Returns the mask of the races `race` is at war with
// (`Race_at_war_mask` 0x3ee6c).
std::uint8_t race_at_war_mask(const Session& session, const Race& race);

// `victim` is attacked by `aggressor` at `star`: attitude loss when not at
// war, the victim's bit in the star's day flags; returns whether the
// offence counted (`Race_offended_by` 0x3eec4).
bool race_offended_by(Session& session, Race& victim, RaceIndex aggressor, const Star& star);

// Returns whether the race's special ability is charged and, with `use`,
// applies it (`Race_use_special_ability` 0x3efc8).
bool race_use_special_ability(Session& session, Race& race, bool use);

// Returns the days the race's special ability needs to recharge, 0 for a
// passive one (`u16[21]` 0x86768 by `Race::ability`).
std::uint16_t race_ability_period(const Race& race);

}  // namespace oa::game
