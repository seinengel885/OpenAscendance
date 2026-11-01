// Diplomacy (`raceneg.cpp`) and the trades (`research.cpp`):
// `docs/09-simulation.md` "Diplomacy". The player's side of a negotiation
// (the negotiation window) uses `race_list_actions`,
// `race_player_reply_options`, `race_respond_to_diplomatic_action` and
// `race_apply_diplomatic_action`, which draw from the shared generator.
#pragma once

#include <cstdint>
#include <vector>

#include "game/address.h"
#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// Diplomatic actions (`.dip` ACTIONS order).
enum DipAction : std::uint8_t {
  kDipDiscover = 0,
  kDipDiscoverAskHelp = 1,
  kDipDeclareWar = 2,
  kDipProposeAlliance = 3,
  kDipTradeStarlanes = 4,
  kDipTradeStars = 5,
  kDipTradeResearch = 6,
  kDipOfferPeaceToOtherRace = 7,
  kDipBreakAlliance = 8,
  kDipDeclareWarOnOtherRace = 9,
  kDipBreakAllianceWithOtherRace = 10,
  kDipProposeAllianceToOtherRace = 11,
  kDipHelpControlSystem = 12,
  kDipOfferPeace = 13,
  kDipNone = 14,
};

// Tells whether a star's explored mask holds both races
// (`Race_shares_star_with` 0x3faa4).
bool race_shares_star_with(const Cosmos& cosmos, const Race& race, RaceIndex other);

// The AI race's daily diplomacy (`Race_diplomacy_turn` 0x3fb00).
void race_diplomacy_turn(Session& session, Race& race);

// peCustom: the fleet strength (`race_fleet_strength`) of the race's side
// (it and its allies) and of its opponents in a war on `target` (the
// target's side and the race's other enemies with their allies).
struct WarOdds {
  std::int32_t own = 0;
  std::int32_t enemy = 0;
};
WarOdds race_war_odds(const Cosmos& cosmos, const Race& race, RaceIndex target);

// peCustom: whether the race may declare war on `target`, by `race_war_odds`
// and an occasion: attacked by the target within 30 days (at 3/4 of its
// strength); or at least as strong and target ships at a colony's star
// within 10 days or an ally at war with the target; or 1.5 times as strong
// with war or invasion ships within 2 hops of a target colony. True in the
// other variants.
bool race_war_justified(const Session& session, const Race& race, RaceIndex target);

// peCustom: the daily attitude rise of the race toward `target`, its enemy:
// the more superior the target's side, the faster; +1 after days without
// attacks by the target.
std::int16_t race_war_concession(const Session& session, const Race& race, RaceIndex target);

// Returns the player's menu toward `alien` with `third` the selected third
// race, -1 for none (`Race_list_actions` 0x3fcb8): kDipNone first, then the
// actions. With -1 the original reads the byte before the relations (the
// high byte of `attitude[6]`), as the remake does.
std::vector<std::uint8_t> race_list_actions(const Session& session, const Race& player, RaceIndex alien,
                                            std::int16_t third);

// Returns the action `race` wants toward `other` and its argument (a race
// index or a star index) (`Race_choose_diplomatic_action` 0x3feb4).
std::uint8_t race_choose_diplomatic_action(Session& session, Race& race, RaceIndex other, std::int32_t& arg);

// Returns the player's possible replies to `action` (`Race_player_reply_options`
// 0x40290).
std::vector<std::uint8_t> race_player_reply_options(const Session& session, const Race& alien, std::uint8_t action);

// Returns the response of `responder` to `action` by `actor`
// (`Race_respond_to_diplomatic_action` 0x40430).
std::uint8_t race_respond_to_diplomatic_action(Session& session, Race& responder, RaceIndex actor, std::uint8_t action,
                                               std::int32_t arg);
// Applies `action` of `actor` toward `target` with `response`
// (`Race_apply_diplomatic_action` 0x40914).
void race_apply_diplomatic_action(Session& session, Race& actor, RaceIndex target, std::uint8_t action,
                                  std::uint8_t response, std::int32_t arg);

// Returns the race's ships less than 3 hops from `star` able to help
// (`Race_find_help_ships` 0x410ac).
std::vector<ShipIndex> race_find_help_ships(const Session& session, const Race& race, const Star& star);

// Counts the ships at `star` whose owner has `relation` toward `race` (3
// also counts the race's own); `strength` receives the sum of
// `hit_points * weapon_damage` (`Race_count_ships_at_star` 0x411bc).
std::int32_t race_count_ships_at_star(const Session& session, const Race& race, std::uint8_t relation, const Star& star,
                                      std::int32_t* strength);

// Sends `ships` to `star` (`Race_send_ships_to_star` 0x41270).
void race_send_ships_to_star(Session& session, const std::vector<ShipIndex>& ships, const Star& star);

// A ship of the player reaches `star` from a lane: first contact or
// arrival events (`Race_on_player_ship_arrival` 0x412b8).
void race_on_player_ship_arrival(Session& session, Race& player, Ship& ship, Star& star);
// A ship of another race reaches a star where the player has ships or a
// colony (`Race_on_alien_ship_arrival` 0x415b4).
void race_on_alien_ship_arrival(Session& session, Race& player, Ship& ship, Star& star);

// Whether `race` would trade with `other` (`Race_wants_lane_trade`
// 0x416c8, `Race_wants_star_trade` 0x41898, `Race_wants_research_trade`
// 0x41a6c).
bool race_wants_lane_trade(const Session& session, const Race& race, RaceIndex other);
bool race_wants_star_trade(const Session& session, const Race& race, RaceIndex other);
bool race_wants_research_trade(const Session& session, const Race& race, RaceIndex other);

// Exchanges (`mode` 0) or only receives (`mode` != 0) lanes, stars or
// research items (`Race_trade_lanes` 0x417c4, `Race_trade_stars` 0x41994,
// `Race_trade_research` 0x41b68).
void race_trade_lanes(Session& session, Race& race, RaceIndex other, std::int32_t mode);
void race_trade_stars(Session& session, Race& race, RaceIndex other, std::int32_t mode);
void race_trade_research(Session& session, Race& race, RaceIndex other, std::int32_t mode);

}  // namespace oa::game
