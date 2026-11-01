#include "game/raceneg.h"

#include <algorithm>
#include <optional>

#include "core/fixed_width.h"
#include "game/cosmos.h"
#include "game/race.h"
#include "game/ship.h"

namespace oa::game {

namespace {

std::uint8_t bit(std::size_t race) { return static_cast<std::uint8_t>(1u << (race & 0x1f)); }

// Adds `delta` to the `i16` attitude of `race` toward `other` (no clamp).
void add_attitude(Race& race, std::size_t other, std::int32_t delta) {
  race.attitude[other] = wrap_i16(race.attitude[other] + delta);
}

bool alive(const Race& race) { return race.extinct != -1; }

// peCustom diplomacy: a target colony within these lane hops of the race's
// war or invasion ships is an occasion for war; this many days without an
// attack raise the enemy's attitude.
constexpr std::uint8_t kNearHops = 2;
constexpr std::int32_t kCeasefireDays = 20;

// The race and its living allies.
std::uint32_t side_mask(const Cosmos& cosmos, RaceIndex r) {
  std::uint32_t mask = 1u << r;
  for (std::int16_t t = 0; t < cosmos.race_count; ++t) {
    const auto ut = static_cast<std::size_t>(t);
    if (alive(cosmos.races[ut]) && cosmos.races[r].relation[ut] == kAlliance) {
      mask |= 1u << ut;
    }
  }
  return mask;
}

std::int32_t mask_strength(const Cosmos& cosmos, std::uint32_t mask) {
  std::int32_t sum = 0;
  for (std::int16_t t = 0; t < cosmos.race_count; ++t) {
    const Race& race = cosmos.races[static_cast<std::size_t>(t)];
    if ((mask & (1u << t)) != 0 && alive(race)) {
      sum += race_fleet_strength(cosmos, race);
    }
  }
  return sum;
}

// The star a ship is at or leaves (none while built or refitted).
std::optional<StarIndex> ship_star(const Cosmos& cosmos, const Ship& ship) {
  switch (ship.location_kind) {
    case ShipLocation::kOrbit:
      return cosmos.planets[static_cast<std::size_t>(ship.location)].star;
    case ShipLocation::kSystem:
      return ship.location;
    case ShipLocation::kLane:
      return cosmos.lanes[static_cast<std::size_t>(ship.location)].star_a;
    default:
      return std::nullopt;
  }
}

// Whether a colony of `target` lies within `kNearHops` lane hops of a war
// or invasion ship of `race`.
bool armed_ships_near(const Cosmos& cosmos, const Race& race, RaceIndex target) {
  for (const ShipIndex i : race_list_ships(cosmos, race)) {
    const Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    const std::optional<StarIndex> here = ship_star(cosmos, ship);
    if (!here || (ship.weapon_damage == 0 && !ship_has_gizmo(ship, kGizmoInvasionModule))) {
      continue;
    }
    for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
      if ((cosmos.stars[static_cast<std::size_t>(s)].owner_mask & bit(target)) != 0 &&
          cosmos.hops[static_cast<std::size_t>(*here)][static_cast<std::size_t>(s)] <= kNearHops) {
        return true;
      }
    }
  }
  return false;
}

// The race's action toward `other`: proposed to the player (a talk), else
// answered and applied at once.
void take_action(Session& session, Race& race, RaceIndex other, std::uint8_t action, std::int32_t arg) {
  if (other == session.player_race && !session.autoplay) {
    race.pending_action = action;
    race.pending_action_arg = static_cast<RawAddress>(arg);
    session.post_event(EventCode::kRaceWantsToTalk, race.index, action);
  } else {
    Race& target = session.game->cosmos.races[other];
    const std::uint8_t response = race_respond_to_diplomatic_action(session, target, race.index, action, arg);
    race_apply_diplomatic_action(session, race, other, action, response, arg);
  }
}

// `race.relation[i]` read with the negotiation window's signed third-party
// index: -1 (no third party listed) reads the byte before the array, the
// high byte of `attitude[6]`.
std::uint8_t relation_at(const Race& race, std::int16_t i) {
  if (i == -1) {
    return static_cast<std::uint8_t>(static_cast<std::uint16_t>(race.attitude[6]) >> 8);
  }
  return race.relation.at(static_cast<std::size_t>(i));
}

// `race.attitude[i]` read the same way: -1 reads `ship_patience` (+0x1b0).
std::int16_t attitude_at(const Race& race, std::int32_t i) {
  return i == -1 ? race.ship_patience : race.attitude.at(static_cast<std::size_t>(i));
}

// Both races meet: relations 1, each attitude its own START_ATTITUDE.
void meet(Race& a, Race& b) {
  a.relation[b.index] = kPeace;
  a.attitude[b.index] = a.start_attitude;
  b.relation[a.index] = kPeace;
  b.attitude[a.index] = b.start_attitude;
}

}  // namespace

// 0x3faa4
bool race_shares_star_with(const Cosmos& cosmos, const Race& race, RaceIndex other) {
  for (std::int32_t i = 0; i < cosmos.star_count; ++i) {
    const std::uint8_t explored = cosmos.stars[static_cast<std::size_t>(i)].explored_mask;
    if ((explored & bit(race.index)) != 0 && (explored & bit(other)) != 0) {
      return true;
    }
  }
  return false;
}

// 0x3fb00
void race_diplomacy_turn(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  race.pending_action = kDipNone;
  // The original's argument local is only written by the third-party
  // actions (7, 9, 10); otherwise it holds what the stack held
  // (`Session::stale_dip_arg`), and keeps its value from race to race.
  auto arg = static_cast<std::int32_t>(session.stale_dip_arg);
  for (std::uint32_t o = 0; o < kMaxRaces; ++o) {
    Race& other = cosmos.races[o];
    if (o == race.index || !alive(other)) {
      continue;
    }
    if ((o != session.player_race || session.autoplay) && race.relation[o] == kNotMet &&
        race_shares_star_with(cosmos, race, static_cast<RaceIndex>(o))) {
      race.attitude[o] = race.start_attitude;
      other.attitude[race.index] = other.start_attitude;
      other.relation[race.index] = kPeace;
      race.relation[o] = kPeace;
    }
    if (race.relation[o] == kNotMet) {
      continue;
    }
    const std::uint8_t action = race_choose_diplomatic_action(session, race, static_cast<RaceIndex>(o), arg);
    if (action != kDipNone) {
      take_action(session, race, static_cast<RaceIndex>(o), action, arg);
    }
  }
  session.stale_dip_arg = static_cast<std::uint32_t>(arg);
}

// 0x3fcb8
std::vector<std::uint8_t> race_list_actions(const Session& session, const Race& player, RaceIndex alien,
                                            std::int16_t third) {
  const Game& game = *session.game;
  const Race& a = game.cosmos.races.at(alien);
  const std::int32_t day = game.cosmos.day;
  std::vector<std::uint8_t> actions{kDipNone};
  const auto trades = [&] {
    if (day - game.last_trade_days[0] > 4) {
      actions.push_back(kDipTradeStarlanes);
    }
    if (day - game.last_trade_days[1] > 4) {
      actions.push_back(kDipTradeStars);
    }
    if (day - game.last_trade_days[2] > 4) {
      actions.push_back(kDipTradeResearch);
    }
  };
  const bool third_known = relation_at(player, third) != kNotMet;
  const std::uint8_t third_relation = relation_at(a, third);
  switch (player.relation.at(alien)) {
    case kPeace:
      actions.push_back(kDipDeclareWar);
      actions.push_back(kDipProposeAlliance);
      trades();
      if (third_known && third_relation == kWar) {
        actions.push_back(kDipOfferPeaceToOtherRace);
      }
      break;
    case kWar:
      actions.push_back(kDipOfferPeace);
      break;
    case kAlliance:
      actions.push_back(kDipBreakAlliance);
      trades();
      actions.push_back(kDipHelpControlSystem);
      if (third_known && third_relation == kWar) {
        actions.push_back(kDipOfferPeaceToOtherRace);
      }
      if (third_known && third_relation == kPeace) {
        actions.push_back(kDipDeclareWarOnOtherRace);
        actions.push_back(kDipProposeAllianceToOtherRace);
      }
      if (third_known && third_relation == kAlliance) {
        actions.push_back(kDipBreakAllianceWithOtherRace);
      }
      break;
    default:
      break;
  }
  return actions;
}

WarOdds race_war_odds(const Cosmos& cosmos, const Race& race, RaceIndex target) {
  const std::uint32_t own = side_mask(cosmos, race.index);
  std::uint32_t enemy = side_mask(cosmos, target);
  for (std::int16_t t = 0; t < cosmos.race_count; ++t) {
    const auto ut = static_cast<std::size_t>(t);
    if (alive(cosmos.races[ut]) && race.relation[ut] == kWar) {
      enemy |= side_mask(cosmos, static_cast<RaceIndex>(t));
    }
  }
  return {mask_strength(cosmos, own), mask_strength(cosmos, enemy & ~own)};
}

bool race_war_justified(const Session& session, const Race& race, RaceIndex target) {
  if (session.game->variant != Variant::kPeCustom) {
    return true;
  }
  const Cosmos& cosmos = session.game->cosmos;
  const WarOdds odds = race_war_odds(cosmos, race, target);
  if (odds.own < odds.enemy) {
    return false;
  }
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    const Star& star = cosmos.stars[static_cast<std::size_t>(s)];
    if ((star.owner_mask & bit(race.index)) != 0 && (star.ships_mask & bit(target)) != 0) {
      return true;
    }
  }
  for (std::int16_t t = 0; t < cosmos.race_count; ++t) {
    const Race& ally = cosmos.races[static_cast<std::size_t>(t)];
    if (alive(ally) && race.relation[ally.index] == kAlliance && ally.relation[target] == kWar) {
      return true;
    }
  }
  // A conquest within reach, clearly stronger.
  return 2 * odds.own >= 3 * odds.enemy && armed_ships_near(cosmos, race, target);
}

std::int16_t race_war_concession(const Session& session, const Race& race, RaceIndex target) {
  const Cosmos& cosmos = session.game->cosmos;
  const WarOdds odds = race_war_odds(cosmos, race, target);
  // 2x stronger +1, 2.5x +2, 3x +3, 4x and more +5.
  std::int32_t d = odds.own == 0 ? 5 : std::clamp(2 * odds.enemy / odds.own - 3, 0, 5);
  if (cosmos.day - session.game->last_attack_days[race.index][target] >= kCeasefireDays) {
    ++d;
  }
  return static_cast<std::int16_t>(d);
}

void race_react_to_attack(Session& session, Race& victim, RaceIndex aggressor, AttackKind kind) {
  if (victim.index == session.player_race && !session.autoplay) {
    return;  // the player answers by its own decisions
  }
  std::int16_t& a = victim.attitude[aggressor];
  const bool allied = victim.relation[aggressor] == kAlliance;
  // Stable: an ally at least accepting an alliance, at peace offering peace.
  const std::int16_t stable = allied ? victim.accept_alliance : victim.propose_peace;
  std::uint8_t action = kDipNone;
  if (kind == AttackKind::kInvasion) {
    action = kDipDeclareWar;
  } else if (a >= stable) {
    a = wrap_i16(stable - 1);
  } else if (allied) {
    action = kDipBreakAlliance;
    a = std::min(a, wrap_i16(victim.propose_peace - 1));
  } else {
    // A weak race bears it below three quarters of the aggressor's strength.
    const WarOdds odds = race_war_odds(session.game->cosmos, victim, aggressor);
    if (4 * odds.own >= 3 * odds.enemy) {
      action = kDipDeclareWar;
    }
  }
  // Once: the further hits of the battle find it pending or done.
  if (action != kDipNone && victim.pending_action != action) {
    take_action(session, victim, aggressor, action, 0);
  }
}

// 0x3feb4; Antagonizer 0x44434 (peace and alliance rules by atmosphere).
std::uint8_t race_choose_diplomatic_action(Session& session, Race& race, RaceIndex other, std::int32_t& arg) {
  Cosmos& cosmos = session.game->cosmos;
  const bool antagonizer = antagonizer_rules(session.game->variant);
  const std::uint8_t war_mask = antagonizer ? race_at_war_mask(session, race) : 0;
  const bool player = other == session.player_race;
  const std::int32_t colonies = cosmos.ranks[race.index].colonies;
  std::int16_t& a = race.attitude[other];
  const bool pe_custom = session.game->variant == Variant::kPeCustom;
  // peCustom: below the war threshold with an occasion and the odds, in every
  // atmosphere and toward the AIs too (the original's colony minimum kept).
  const bool pe_war = pe_custom && a < race.declare_war && (player || colonies > 2) &&
                      race_war_justified(session, race, other);
  switch (race.relation[other]) {
    case kPeace:
      if (pe_custom) {
        if (pe_war) {
          return kDipDeclareWar;
        }
      } else if (!antagonizer) {
        if (a < race.declare_war && colonies > 2) {
          return kDipDeclareWar;
        }
      } else if (cosmos.atmosphere == Atmosphere::kPeaceful) {
        if (a < race.declare_war && (war_mask == 0 || player) && (player || colonies > 2)) {
          return kDipDeclareWar;
        }
      } else if (cosmos.atmosphere == Atmosphere::kNeutral) {
        if (a < race.declare_war && player) {
          return kDipDeclareWar;
        }
      } else if (cosmos.atmosphere == Atmosphere::kHostile && player) {
        // The attitude toward the player steps down one threshold a day.
        if (a < race.accept_peace) {
          a = wrap_i16(race.declare_war - 1);
          return kDipDeclareWar;
        }
        if (a < race.propose_peace) {
          a = wrap_i16(race.accept_peace - 1);
        } else if (a < race.break_alliance) {
          a = wrap_i16(race.propose_peace - 1);
        } else if (a < race.accept_alliance) {
          a = wrap_i16(race.break_alliance - 1);
        } else if (a < race.propose_alliance) {
          a = wrap_i16(race.accept_alliance - 1);
        } else {
          a = wrap_i16(race.propose_alliance - 1);
        }
      }
      if (a > race.propose_alliance) {
        return kDipProposeAlliance;
      }
      break;
    case kWar:
      if (a > race.propose_peace) {
        return kDipOfferPeace;
      }
      break;
    case kAlliance:
      if (pe_custom) {
        if (pe_war) {
          return kDipDeclareWar;
        }
        if (a < race.break_alliance) {
          return kDipBreakAlliance;
        }
      } else if (!antagonizer) {
        if (a < race.declare_war) {
          return kDipDeclareWar;
        }
        if (a < race.break_alliance) {
          return kDipBreakAlliance;
        }
      } else if (a < race.declare_war && war_mask == 0) {
        if (player || colonies > 2) {
          return kDipDeclareWar;
        }
        return kDipBreakAlliance;
      } else if (a < race.break_alliance) {
        return kDipBreakAlliance;
      }
      break;
    default:
      break;
  }
  if (cosmos.day % race.neg_player_turns != 0 || !player || race.relation[other] == kWar) {
    return kDipNone;
  }
  std::int32_t roll = session.rng.rand() % 100;
  if (race.relation[other] == kAlliance && roll < 80) {
    const Race& p = cosmos.races[session.player_race];
    // Three scans, in this priority.
    const std::pair<std::uint8_t, std::uint8_t> wanted[] = {{kWar, kAlliance}, {kAlliance, kWar}, {kWar, kPeace}};
    const std::uint8_t actions[] = {kDipBreakAllianceWithOtherRace, kDipOfferPeaceToOtherRace,
                                    kDipDeclareWarOnOtherRace};
    for (std::size_t k = 0; k < 3; ++k) {
      for (std::int16_t t = 0; t < cosmos.race_count; ++t) {
        const auto ut = static_cast<std::size_t>(t);
        if (ut != race.index && t != other && alive(cosmos.races[ut]) && race.relation[ut] == wanted[k].first &&
            p.relation[ut] == wanted[k].second) {
          arg = t;
          return actions[k];
        }
      }
    }
  }
  if (a >= race.declare_war) {
    if (race.relation[other] == kAlliance) {
      roll += 15;
    }
    if (a > race.break_alliance + 10 && roll > 92 && race_wants_research_trade(session, race, other)) {
      return kDipTradeResearch;
    }
    if (a > race.break_alliance && roll > 87 && race_wants_star_trade(session, race, other)) {
      return kDipTradeStars;
    }
    if (a > race.start_attitude + 20 && roll > 79 && race_wants_lane_trade(session, race, other)) {
      return kDipTradeStarlanes;
    }
  }
  return kDipNone;
}

// 0x40290: the accept/reject pair of each action (acknowledgement only for
// 0, 2 and 8).
std::vector<std::uint8_t> race_player_reply_options(const Session&, const Race&, std::uint8_t action) {
  switch (action) {
    case kDipDiscover:
      return {0};
    case kDipDiscoverAskHelp:
      return {1, 2};
    case kDipDeclareWar:
      return {3};
    case kDipProposeAlliance:
      return {4, 5};
    case kDipTradeStarlanes:
      return {6, 7};
    case kDipTradeStars:
      return {8, 9};
    case kDipTradeResearch:
      return {10, 11};
    case kDipOfferPeaceToOtherRace:
      return {12, 13};
    case kDipBreakAlliance:
      return {14};
    case kDipDeclareWarOnOtherRace:
      return {15, 16};
    case kDipBreakAllianceWithOtherRace:
      return {17, 18};
    case kDipProposeAllianceToOtherRace:
      return {19, 20};
    case kDipHelpControlSystem:
      return {21, 22};
    case kDipOfferPeace:
      return {27, 28};
    default:
      return {};
  }
}

// 0x40430. `arg` is a race index, or a star index for HelpControlSystem
// (the original stores the star's address in `pending_action_arg`).
std::uint8_t race_respond_to_diplomatic_action(Session& session, Race& responder, RaceIndex actor, std::uint8_t action,
                                               std::int32_t arg) {
  Cosmos& cosmos = session.game->cosmos;
  responder.pending_action_arg = action == kDipHelpControlSystem ? star_address(session, static_cast<StarIndex>(arg))
                                                                 : static_cast<RawAddress>(arg);
  const std::int32_t a = responder.attitude[actor];
  const Race& x = cosmos.races[actor];
  const auto t = static_cast<std::size_t>(arg);
  switch (action) {
    case kDipDeclareWar:
      return 3;
    case kDipProposeAlliance:
      return a <= responder.accept_alliance ? 5 : 4;
    case kDipTradeStarlanes:
      return a > responder.start_attitude + 20 && race_wants_lane_trade(session, responder, actor) ? 6 : 7;
    case kDipTradeStars:
      return a > responder.break_alliance && race_wants_star_trade(session, responder, actor) ? 8 : 9;
    case kDipTradeResearch:
      return a > responder.break_alliance + 10 && race_wants_research_trade(session, responder, actor) ? 10 : 11;
    case kDipOfferPeaceToOtherRace:
      // The actor's ACCEPT_ALLIANCE and the responder's PROPOSE_PEACE
      // (original's choice).
      return wrap_i16(a - x.accept_alliance) + attitude_at(responder, arg) <= responder.propose_peace ? 13 : 12;
    case kDipBreakAlliance:
      return 14;
    case kDipDeclareWarOnOtherRace:
      return wrap_i16(a - attitude_at(responder, arg)) <= responder.start_attitude ? 16 : 15;
    case kDipBreakAllianceWithOtherRace: {
      if (wrap_i16(a - attitude_at(responder, arg)) > 0) {
        const std::int32_t mine = wrap_i16(a - x.accept_alliance);
        // Without a third party (-1) the original reads a record before the
        // races array; the remake takes 0 for its ACCEPT_ALLIANCE.
        const std::int32_t theirs =
            wrap_i16(attitude_at(responder, arg) - (arg == -1 ? 0 : cosmos.races.at(t).accept_alliance));
        if (mine != theirs && mine - theirs > -1) {
          return 17;
        }
      }
      return 18;
    }
    case kDipProposeAllianceToOtherRace:
      // Looks inverted (original): accepts only a disliked third race.
      return responder.declare_war <= attitude_at(responder, arg) - wrap_i16(a - x.accept_alliance) ? 20 : 19;
    case kDipHelpControlSystem: {
      const Star& star = cosmos.stars.at(static_cast<std::size_t>(arg));
      if (race_count_ships_at_star(session, responder, kWar, star, nullptr) == 0) {
        return 24;
      }
      const bool here = (star.ships_mask & bit(responder.index)) != 0;
      if (race_find_help_ships(session, responder, star).empty()) {
        return here ? 25 : 23;
      }
      if (here) {
        return 26;
      }
      return a <= responder.propose_alliance ? 22 : 21;
    }
    case kDipOfferPeace:
      return a < responder.accept_peace ? 28 : 27;
    default:
      return 29;
  }
}

// 0x40914. `arg` is the third race, or the star of HelpControlSystem.
void race_apply_diplomatic_action(Session& session, Race& actor, RaceIndex target, std::uint8_t action,
                                  std::uint8_t response, std::int32_t arg) {
  Game& game = *session.game;
  Cosmos& cosmos = game.cosmos;
  Race& b = cosmos.races[target];
  const std::size_t self = actor.index;
  const bool by_player = actor.index == session.player_race;
  const auto t = static_cast<std::size_t>(arg);
  switch (action) {
    case kDipDiscover:
      b.relation[self] = kPeace;
      actor.relation[target] = kPeace;
      actor.attitude[target] = actor.start_attitude;
      b.attitude[self] = b.start_attitude;
      break;
    case kDipDiscoverAskHelp:
      if (response == 1) {
        add_attitude(actor, target, 40);
      }
      break;
    case kDipDeclareWar:
      b.relation[self] = kWar;
      actor.relation[target] = kWar;
      add_attitude(b, self, -40);
      game.last_attack_days[target][self] = cosmos.day;
      game.last_attack_days[self][target] = cosmos.day;
      break;
    case kDipProposeAlliance:
      if (by_player) {
        add_attitude(b, session.player_race, 3);
      }
      if (response != 4) {
        actor.attitude[target] = actor.accept_alliance;
        break;
      }
      b.relation[self] = kAlliance;
      actor.relation[target] = kAlliance;
      add_attitude(actor, target, 10);
      add_attitude(b, self, 10);
      break;
    case kDipTradeStarlanes:
    case kDipTradeStars:
    case kDipTradeResearch: {
      const std::size_t k = static_cast<std::size_t>(action - kDipTradeStarlanes);
      constexpr std::int32_t kBonus[] = {5, 8, 8};
      constexpr std::uint8_t kAccepted[] = {6, 8, 10};
      constexpr std::int32_t kBack[] = {5, 5, 10};
      constexpr std::int32_t kRefused[] = {2, 3, 2};
      if (by_player) {
        add_attitude(b, session.player_race, kBonus[k]);
        game.last_trade_days[k] = cosmos.day;
      }
      if (response == kAccepted[k]) {
        if (action == kDipTradeStarlanes) {
          race_trade_lanes(session, actor, target, 0);
        } else if (action == kDipTradeStars) {
          race_trade_stars(session, actor, target, 0);
        } else {
          race_trade_research(session, actor, target, 0);
        }
        add_attitude(actor, target, 10);
        add_attitude(b, self, kBack[k]);
      } else {
        add_attitude(actor, target, -kRefused[k]);
        if (actor.relation[target] == kAlliance) {
          add_attitude(actor, target, -kRefused[k]);
        }
      }
      break;
    }
    case kDipOfferPeaceToOtherRace: {
      if (response != 12) {
        add_attitude(actor, target, -5);
        break;
      }
      add_attitude(actor, target, 5);
      if (arg == -1) {
        // No third party listed (`race_list_actions` read stale bytes): the
        // original's third race record lies before the races array, which
        // the remake does not reproduce.
        break;
      }
      Race& r = cosmos.races[t];
      if (r.attitude[target] > r.accept_peace - 10) {
        add_attitude(actor, target, 5);
        b.relation[t] = kPeace;
        r.relation[target] = kPeace;
        add_attitude(b, t, 10);
        add_attitude(b, self, 5);
        add_attitude(r, target, 10);
        add_attitude(r, self, 15);
      }
      break;
    }
    case kDipBreakAlliance:
      b.relation[self] = kPeace;
      actor.relation[target] = kPeace;
      add_attitude(b, self, -30);
      break;
    case kDipDeclareWarOnOtherRace: {
      if (response != 15) {
        add_attitude(actor, target, -10);
        break;
      }
      add_attitude(actor, target, 10);
      if (arg == -1) {
        break;  // no third party: see OfferPeaceToOtherRace
      }
      Race& r = cosmos.races[t];
      b.relation[t] = kWar;
      r.relation[target] = kWar;
      add_attitude(b, t, -15);
      add_attitude(b, self, 10);
      add_attitude(r, target, -20);
      add_attitude(r, self, -10);
      break;
    }
    case kDipBreakAllianceWithOtherRace: {
      if (response != 17) {
        add_attitude(actor, target, -10);
        break;
      }
      add_attitude(actor, target, 15);
      if (arg == -1) {
        break;  // no third party: see OfferPeaceToOtherRace
      }
      Race& r = cosmos.races[t];
      b.relation[t] = kPeace;
      r.relation[target] = kPeace;
      add_attitude(b, t, -5);
      add_attitude(b, self, -5);
      add_attitude(r, target, -10);
      add_attitude(r, self, -5);
      break;
    }
    case kDipProposeAllianceToOtherRace: {
      if (response != 19) {
        add_attitude(actor, target, -5);
        break;
      }
      add_attitude(actor, target, 5);
      if (arg == -1) {
        break;  // no third party: see OfferPeaceToOtherRace
      }
      Race& r = cosmos.races[t];
      if (r.attitude[target] > r.accept_alliance - 10) {
        add_attitude(actor, target, 5);
        b.relation[t] = kAlliance;
        r.relation[target] = kAlliance;
        add_attitude(b, t, 10);
        add_attitude(b, self, 5);
        add_attitude(r, target, 15);
        add_attitude(r, self, 10);
      }
      break;
    }
    case kDipHelpControlSystem:
      if ((response == 21 || response == 26) && target != session.player_race) {
        const Star& star = cosmos.stars.at(t);
        const std::vector<ShipIndex> ships = race_find_help_ships(session, b, star);
        if (!ships.empty()) {
          race_send_ships_to_star(session, ships, star);
        }
        add_attitude(actor, target, 10);
      } else if (response == 22) {
        add_attitude(actor, target, -5);
      }
      break;
    case kDipOfferPeace:
      if (by_player) {
        add_attitude(b, session.player_race, 3);
      }
      if (response != 27) {
        add_attitude(actor, target, -25);
        break;
      }
      b.relation[self] = kPeace;
      actor.relation[target] = kPeace;
      break;
    default:
      break;
  }
}

// 0x410ac: ships not being built or refitted whose star (a lane's star A
// for a ship on a lane) is less than 3 hops away; a ship in space at a star
// holding ships of a race at war is skipped.
std::vector<ShipIndex> race_find_help_ships(const Session& session, const Race& race, const Star& star) {
  const Cosmos& cosmos = session.game->cosmos;
  std::vector<ShipIndex> found;
  std::optional<StarIndex> here;  // the original keeps the previous ship's star otherwise
  for (const ShipIndex s : race_list_ships(cosmos, race)) {
    const Ship& ship = cosmos.ships[static_cast<std::size_t>(s)];
    switch (ship.location_kind) {
      case ShipLocation::kBuilding:
      case ShipLocation::kRefit:
        continue;
      case ShipLocation::kOrbit:
        here = cosmos.planets[static_cast<std::size_t>(ship.location)].star;
        break;
      case ShipLocation::kSystem:
        if (race_count_ships_at_star(session, race, kWar, cosmos.stars[static_cast<std::size_t>(ship.location)],
                                     nullptr) != 0) {
          continue;
        }
        here = ship.location;
        break;
      case ShipLocation::kLane:
        here = cosmos.lanes[static_cast<std::size_t>(ship.location)].star_a;
        break;
      default:
        break;
    }
    if (here && cosmos.hops[static_cast<std::size_t>(*here)][static_cast<std::size_t>(star.index)] < 3) {
      found.push_back(s);
    }
  }
  return found;
}

// 0x411bc
std::int32_t race_count_ships_at_star(const Session& session, const Race& race, std::uint8_t relation, const Star& star,
                                      std::int32_t* strength) {
  const Cosmos& cosmos = session.game->cosmos;
  std::int16_t count = 0;
  std::int32_t sum = 0;
  for (const ShipIndex s : cosmos_ships_at_star(cosmos, star)) {
    const Ship& ship = cosmos.ships[static_cast<std::size_t>(s)];
    const auto owner = static_cast<std::size_t>(ship.owner);
    if ((owner == race.index && relation == kAlliance) || race.relation[owner] == relation) {
      ++count;
      sum = wrap_i32(static_cast<std::int64_t>(sum) + static_cast<std::int64_t>(ship.hit_points) * ship.weapon_damage);
    }
  }
  if (strength != nullptr) {
    *strength = sum;
  }
  return count;
}

// 0x41270
void race_send_ships_to_star(Session& session, const std::vector<ShipIndex>& ships, const Star& star) {
  Cosmos& cosmos = session.game->cosmos;
  for (const ShipIndex s : ships) {
    Ship& ship = cosmos.ships[static_cast<std::size_t>(s)];
    ship.destination_kind = ShipOrder::kGoStar;
    ship.destination = star.index;
  }
}

// 0x412b8: once per star and day (bit 0 of the day flags). Event 4 for
// the first unmet race present, else 5 (the last race at war with ships
// there), 6 (a race at war with a colony), 7 (two races at war with each
// other with ships), else the order is cleared and 20 (not explored yet) or
// 3. The event's second argument is the race found (0 for 3 and 20).
void race_on_player_ship_arrival(Session& session, Race& player, Ship& ship, Star& star) {
  Cosmos& cosmos = session.game->cosmos;
  std::uint8_t& flags = session.star_day_flags[static_cast<std::size_t>(star.index)];
  if ((flags & 1) != 0) {
    return;
  }
  const std::int16_t races = cosmos.race_count;
  bool met = false;
  bool ships_at_war = false;
  bool colony_at_war = false;
  bool others_at_war = false;
  std::size_t found = 0;
  for (std::int16_t r = 0; r < races; ++r) {
    const auto ur = static_cast<std::size_t>(r);
    if (ur != session.player_race && player.relation[ur] == kNotMet &&
        ((star.ships_mask | star.owner_mask) & bit(ur)) != 0) {
      meet(player, cosmos.races[ur]);
      met = true;
      found = ur;
      break;
    }
  }
  if (!met) {
    for (std::int16_t r = 0; r < races; ++r) {
      const auto ur = static_cast<std::size_t>(r);
      if (ur == session.player_race || player.relation[ur] != kWar) {
        continue;
      }
      if ((star.ships_mask & bit(ur)) != 0) {
        ships_at_war = true;
        found = ur;
      }
      if (!ships_at_war && (star.owner_mask & bit(ur)) != 0) {
        found = ur;
        colony_at_war = true;
      }
    }
  }
  if (!met && !ships_at_war && !colony_at_war) {
    for (std::int16_t u = 0; u < races && !others_at_war; ++u) {
      const auto uu = static_cast<std::size_t>(u);
      if (uu == session.player_race) {
        continue;
      }
      for (std::int16_t v = 0; v < races; ++v) {
        const auto uv = static_cast<std::size_t>(v);
        if ((star.ships_mask & bit(uu)) != 0 && (star.ships_mask & bit(uv)) != 0 &&
            cosmos.races[uu].relation[uv] == kWar) {
          others_at_war = true;
          break;
        }
      }
    }
  }
  EventCode code{};
  if (met) {
    code = EventCode::kFirstContact;
  } else if (ships_at_war) {
    code = EventCode::kHostileShips;
  } else if (colony_at_war) {
    code = EventCode::kHostileColony;
  } else if (others_at_war) {
    code = EventCode::kOthersAtWar;
  } else {
    ship.destination_kind = ShipOrder::kNothing;
    ship.destination = 0;
    ship.stale_destination = 0;
    code =
        (star.explored_mask & bit(session.player_race)) == 0 ? EventCode::kSystemDiscovered : EventCode::kShipArrived;
  }
  flags |= 1;
  session.post_event(code, ship_index(cosmos, ship), static_cast<std::int32_t>(found));
}

// 0x415b4: only when the arriving race had no ship at the star yet; once
// per star and day.
void race_on_alien_ship_arrival(Session& session, Race& player, Ship& ship, Star& star) {
  Cosmos& cosmos = session.game->cosmos;
  const auto owner = static_cast<std::size_t>(ship.owner);
  std::optional<EventCode> code;
  if ((star.ships_mask & bit(static_cast<std::uint32_t>(ship.owner))) == 0) {
    if (player.relation[owner] == kNotMet) {
      meet(player, cosmos.races[owner]);
      code = EventCode::kUnknownShipEntered;
    } else if (player.relation[owner] == kWar) {
      code = EventCode::kHostileShipEntered;
    }
  }
  std::uint8_t& flags = session.star_day_flags[static_cast<std::size_t>(star.index)];
  if (code && (flags & 1) == 0) {
    session.post_event(*code, ship_index(cosmos, ship), ship.owner);
    flags |= 1;
  }
}

// The ratio tests of the three `wants` functions: the count as a float over
// an integer, divided in double precision (x87, 53-bit precision control),
// against the double constants 0x82436.. (0.9, 2.0, 0.9, 1.6, 0.85, 1.7).
namespace {

bool trade_ratios_ok(std::int16_t mine, std::int16_t total, std::int16_t theirs, std::int16_t minimum, double max_share,
                     double max_ratio) {
  if (mine < minimum) {
    return false;
  }
  if (theirs == 0) {
    theirs = 1;
  }
  const double m = static_cast<float>(mine);
  return !(m / total > max_share) && !(m / theirs > max_ratio);
}

}  // namespace

// 0x416c8: always when allied.
bool race_wants_lane_trade(const Session& session, const Race& race, RaceIndex other) {
  if (race.relation[other] == kAlliance) {
    return true;
  }
  const Cosmos& cosmos = session.game->cosmos;
  std::int16_t mine = 0;
  std::int16_t theirs = 0;
  for (std::int16_t i = 0; i < cosmos.lane_count; ++i) {
    const std::uint8_t known = cosmos.lanes[static_cast<std::size_t>(i)].known_mask;
    mine = static_cast<std::int16_t>(mine + ((known & bit(race.index)) != 0));
    theirs = static_cast<std::int16_t>(theirs + ((known & bit(other)) != 0));
  }
  return trade_ratios_ok(mine, cosmos.lane_count, theirs, 5, 0.9, 2.0);
}

// 0x41898
bool race_wants_star_trade(const Session& session, const Race& race, RaceIndex other) {
  if (race.relation[other] == kAlliance) {
    return true;
  }
  const Cosmos& cosmos = session.game->cosmos;
  std::int16_t mine = 0;
  std::int16_t theirs = 0;
  for (std::int16_t i = 0; i < cosmos.star_count; ++i) {
    const std::uint8_t explored = cosmos.stars[static_cast<std::size_t>(i)].explored_mask;
    mine = static_cast<std::int16_t>(mine + ((explored & bit(race.index)) != 0));
    theirs = static_cast<std::int16_t>(theirs + ((explored & bit(other)) != 0));
  }
  return trade_ratios_ok(mine, cosmos.star_count, theirs, 3, 0.9, 1.6);
}

// 0x41a6c
bool race_wants_research_trade(const Session& session, const Race& race, RaceIndex other) {
  if (race.relation[other] == kAlliance) {
    return true;
  }
  const Research& research = session.game->research;
  std::int16_t mine = 0;
  std::int16_t theirs = 0;
  for (std::uint16_t i = 0; i < research.count; ++i) {
    const std::uint8_t known = research.items[i].known_mask;
    mine = static_cast<std::int16_t>(mine + ((known & bit(race.index)) != 0));
    theirs = static_cast<std::int16_t>(theirs + ((known & bit(other)) != 0));
  }
  return trade_ratios_ok(mine, static_cast<std::int16_t>(research.count), theirs, 5, 0.85, 1.7);
}

// 0x417c4: up to `n` lanes each way (`rand() % 6 + 1` allied, else `% 3
// + 1`), in lane order.
void race_trade_lanes(Session& session, Race& race, RaceIndex other, std::int32_t mode) {
  Cosmos& cosmos = session.game->cosmos;
  const std::int32_t n = session.rng.rand() % (race.relation[other] == kAlliance ? 6 : 3) + 1;
  const std::uint8_t mine = bit(race.index);
  const std::uint8_t theirs = bit(cosmos.races[other].index);
  std::int32_t given = 0;
  std::int32_t received = 0;
  for (std::int16_t i = 0; i < cosmos.lane_count; ++i) {
    std::uint8_t& known = cosmos.lanes[static_cast<std::size_t>(i)].known_mask;
    if (mode == 0 && (known & mine) != 0) {
      if (given < n && (known & theirs) == 0) {
        ++given;
        known |= theirs;
      }
    } else if ((known & theirs) != 0 && received < n && (known & mine) == 0) {
      ++received;
      known |= mine;
    }
  }
}

// 0x41994: `rand() % 4 + 1` allied, else `% 2 + 1` stars each way; the
// receiver learns the star and its lanes' existence (explored and
// lanes-known bits).
void race_trade_stars(Session& session, Race& race, RaceIndex other, std::int32_t mode) {
  Cosmos& cosmos = session.game->cosmos;
  const std::int32_t n = session.rng.rand() % (race.relation[other] == kAlliance ? 4 : 2) + 1;
  const std::uint8_t mine = bit(race.index);
  const std::uint8_t theirs = bit(cosmos.races[other].index);
  std::int32_t given = 0;
  std::int32_t received = 0;
  for (std::int16_t i = 0; i < cosmos.star_count; ++i) {
    Star& star = cosmos.stars[static_cast<std::size_t>(i)];
    if (mode == 0 && (star.explored_mask & mine) != 0) {
      if (given < n && (star.explored_mask & theirs) == 0) {
        star.lanes_known_mask |= theirs;
        ++given;
        star.explored_mask |= theirs;
      }
    } else if ((star.explored_mask & theirs) != 0 && received < n && (star.explored_mask & mine) == 0) {
      star.lanes_known_mask |= mine;
      ++received;
      star.explored_mask |= mine;
    }
  }
}

// 0x41b68: `rand() % 2 + 1` items each way when allied (the only draw),
// else 1. The receiver's `known` bit only; a received item that was its
// project clears the project without resetting the progress (original).
void race_trade_research(Session& session, Race& race, RaceIndex other, std::int32_t mode) {
  Game& game = *session.game;
  Research& research = game.research;
  const std::int32_t n = race.relation[other] == kAlliance ? session.rng.rand() % 2 + 1 : 1;
  const std::uint8_t mine = bit(race.index);
  const RaceIndex other_index = game.cosmos.races[other].index;
  const std::uint8_t theirs = bit(other_index);
  std::int32_t given = 0;
  std::int32_t received = 0;
  for (std::uint16_t i = 0; i < research.count; ++i) {
    std::uint8_t& known = research.items[i].known_mask;
    if (mode == 0 && (known & mine) != 0) {
      if (given < n && (known & theirs) == 0) {
        known |= theirs;
        if (static_cast<std::uint16_t>(research.project[other_index]) == i) {
          research.project[other_index] = -1;
        }
        ++given;
      }
    } else if ((known & theirs) != 0 && received < n && (known & mine) == 0) {
      known |= mine;
      if (static_cast<std::uint16_t>(research.project[race.index]) == i) {
        research.project[race.index] = -1;
      }
      ++received;
    }
  }
}

}  // namespace oa::game
