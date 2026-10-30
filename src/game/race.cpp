#include "game/race.h"

#include <array>
#include <span>

#include "core/fixed_width.h"
#include "core/watcom_qsort.h"
#include "game/cosmos.h"
#include "game/planet.h"
#include "game/race_ai.h"
#include "game/ship.h"

namespace oa::game {

void race_reset(Race& race) {
  race.ai_build_threshold_a = 18;
  race.ai_build_threshold_b = 22;
  race.ai_unknown_1ae = 0;
  race.target_count = 0;
  race.bio_max_positive = 0;
  race.bio_max_negative = 0;
  race.pending_action = kNoDipAction;
  race.pending_action_arg = 0;
  race.relation.fill(kNotMet);
  race.attitude.fill(0);
  race.extinct = 0;
}

void race_init(Session& session, Race& race, std::uint8_t index, std::uint8_t species) {
  race_reset(race);
  race.index = index;
  if (race.species != species) {
    race.species = species;
    race_load_dip_values(session, race);
  }
}

void race_load_dip_values(Session& session, Race& race) {
  session.game->last_trade_days.fill(-10);
  const DipValues& d = session.tables->dip.at(race.species);
  race.declare_war = d.declare_war;
  race.accept_peace = d.accept_peace;
  race.propose_peace = d.propose_peace;
  race.break_alliance = d.break_alliance;
  race.accept_alliance = d.accept_alliance;
  race.propose_alliance = d.propose_alliance;
  race.start_attitude = d.start_attitude;
  race.ship_patience = d.ship_patience;
  race.bio_period = d.bio_period;
  race.bio_max_positive = d.bio_max_positive;
  race.bio_max_negative = d.bio_max_negative;
  race.bio_sin_x = d.bio_sin_x;
  race.bio_sin_half_x = d.bio_sin_half_x;
  race.bio_sin_twice_x = d.bio_sin_twice_x;
  race.neg_player_turns = d.neg_player_turns;
  race.ability = race.species;
  race.ability_charge = 0;
}

std::int32_t race_ship_count(const Cosmos& cosmos, const Race& race) {
  std::int32_t used = 0;
  std::int32_t count = 0;
  for (std::size_t i = 0; i < kMaxShips && used < cosmos.ship_count; ++i) {
    const Ship& ship = cosmos.ships[i];
    if (ship.owner != -1) {
      ++used;
      if (ship.owner == race.index) {
        ++count;
      }
    }
  }
  return count;
}

std::int32_t race_count_planets(const Cosmos& cosmos, const Race& race) {
  std::int32_t count = 0;
  for (std::int16_t i = 0; i < cosmos.planet_count; ++i) {
    if (cosmos.planets[static_cast<std::size_t>(i)].owner == race.index) {
      ++count;
    }
  }
  return count;
}

std::int32_t race_count_discoveries(const Research& research, const Race& race) {
  std::int32_t count = 0;
  for (std::size_t i = 0; i < research.count; ++i) {
    if ((research.items[i].known_mask >> (race.index & 0x1f)) & 1) {
      ++count;
    }
  }
  return count;
}

std::int32_t race_ship_allowance(const Cosmos& cosmos, const Race& race) {
  const std::uint32_t bit = 1u << (race.index & 0x1f);
  std::int32_t allowance = 1;
  for (std::int16_t i = 0; i < cosmos.star_count; ++i) {
    if (cosmos.stars[static_cast<std::size_t>(i)].owner_mask == bit) {
      ++allowance;
    }
  }
  allowance -= race_ship_count(cosmos, race);
  if (cosmos.stars.at(static_cast<std::size_t>(race.home)).owner_mask & bit) {
    ++allowance;
  }
  return allowance;
}

namespace {

// Ability recharge periods by species (`u16[21]` 0x86768); 0 = passive.
constexpr std::array<std::uint16_t, kSpeciesCount> kAbilityPeriod = {0,   0,  0,  0,  0, 60, 100, 62, 77, 90, 150,
                                                                     140, 63, 66, 72, 0, 92, 100, 0,  89, 68};

// Species abilities (`Race::ability`) with an effect in `race_use_special_ability`.
enum Ability : std::uint8_t {
  kFludentri = 5,
  kBaliflids = 6,
  kSwaparamans = 7,
  kFrutmaka = 8,
  kShevar = 9,
  kGovorom = 10,
  kUngooma = 11,
  kDubtaks = 12,
  kCapelons = 13,
  kMebes = 14,
  kArbryls = 16,
  kMarmosians = 17,
  kChamachies = 19,
  kNimbuloids = 20,
};

constexpr std::uint32_t bit_of(std::size_t race) { return 1u << (race & 0x1f); }

}  // namespace

std::vector<ShipIndex> race_list_ships(const Cosmos& cosmos, const Race& race) {
  // The original sorts the 4-byte ship pointers; the Watcom qsort's path
  // depends on the element size, so 4-byte slots are sorted here too.
  std::vector<std::int32_t> slots;
  std::int32_t used = 0;
  for (std::size_t i = 0; i < kMaxShips && used < cosmos.ship_count; ++i) {
    const Ship& ship = cosmos.ships[i];
    if (ship.owner != -1) {
      ++used;
      if (ship.owner == race.index) {
        slots.push_back(static_cast<std::int32_t>(i));
      }
    }
  }
  // `Ship_compare_built_day` 0x3c32c: the int difference of the built days.
  watcom_qsort(std::span(slots), [&cosmos](std::int32_t a, std::int32_t b) {
    return wrap_i32(static_cast<std::int64_t>(cosmos.ships[static_cast<std::size_t>(a)].built_day) -
                    cosmos.ships[static_cast<std::size_t>(b)].built_day);
  });
  return {slots.begin(), slots.end()};
}

void race_turn_update(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  if (race.extinct == -1) {
    return;
  }
  const std::int32_t allowance = race_ship_allowance(cosmos, race);
  if (allowance < 0) {
    // Scraps the most recently built ships.
    const std::vector<ShipIndex> ships = race_list_ships(cosmos, race);
    const std::int32_t last = static_cast<std::int32_t>(ships.size()) - 1;
    for (std::int16_t i = 0; i < -allowance; ++i) {
      Ship& ship = cosmos.ships.at(static_cast<std::size_t>(ships.at(static_cast<std::size_t>(last - i))));
      if (race.index == session.player_race) {
        session.post_event(EventCode::kShipScrapped, ship.hull_size);
      }
      ship_destroy(session, ship);
    }
  }
  const std::vector<ShipIndex> ships = race_list_ships(cosmos, race);
  if (ships.empty() && race_count_planets(cosmos, race) == 0) {
    race.extinct = -1;
    session.post_event(EventCode::kRaceExtinct, race.index);
    return;
  }
  for (const ShipIndex i : ships) {
    ship_new_day(session, cosmos.ships[static_cast<std::size_t>(i)]);
  }
  race_ai_turn(session, race);
  if ((cosmos.stars.at(static_cast<std::size_t>(race.home)).owner_mask & 0x80) == 0) {
    return;
  }
  race.ability_charge = wrap_i32(static_cast<std::int64_t>(race.ability_charge) + 1);
  if (race.index == session.player_race && race.ability_charge != 0 &&
      kAbilityPeriod.at(race.ability) == static_cast<std::uint32_t>(race.ability_charge)) {
    session.post_event(EventCode::kAbilityCharged, 0);
  }
}

void race_reset_ship_moves_at_star(Session& session, Race& race, const Star& star) {
  Cosmos& cosmos = session.game->cosmos;
  for (const ShipIndex i : cosmos_ships_at_star(cosmos, star)) {
    Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    if (ship.owner == race.index) {
      ship_reset_battle_moves(session, ship);
    }
  }
}

std::uint8_t race_at_war_mask(const Session& session, const Race& race) {
  const Cosmos& cosmos = session.game->cosmos;
  std::uint8_t mask = 0;
  for (std::int16_t t = 0; t < cosmos.race_count; ++t) {
    const auto ut = static_cast<std::size_t>(t);
    if (ut != race.index && cosmos.races[ut].extinct != -1 && race.relation[ut] == kWar) {
      mask = static_cast<std::uint8_t>(mask | bit_of(ut));
    }
  }
  return mask;
}

bool race_offended_by(Session& session, Race& victim, RaceIndex aggressor, const Star& star) {
  const Cosmos& cosmos = session.game->cosmos;
  if (victim.relation.at(aggressor) == kWar) {
    return false;
  }
  const std::int32_t atmosphere = static_cast<std::int32_t>(cosmos.atmosphere);
  std::int16_t loss = wrap_i16((atmosphere + 1) * 10);
  if (victim.relation[aggressor] == kAlliance) {
    loss = wrap_i16((atmosphere + 1) * 20);
  }
  const std::int16_t attitude = wrap_i16(victim.attitude[aggressor] - loss);
  victim.attitude[aggressor] = attitude;
  // An ally still above its break threshold is pushed below it.
  if (victim.relation[aggressor] == kAlliance && victim.break_alliance < attitude) {
    victim.attitude[aggressor] = wrap_i16(victim.break_alliance - 10);
  }
  std::uint8_t& flags = session.star_day_flags.at(static_cast<std::size_t>(star.index));
  flags = static_cast<std::uint8_t>(flags | bit_of(victim.index));
  return true;
}

std::uint16_t race_ability_period(const Race& race) { return kAbilityPeriod.at(race.ability); }

bool race_use_special_ability(Session& session, Race& race, bool use) {
  Cosmos& cosmos = session.game->cosmos;
  Research& research = session.game->research;
  const std::uint16_t period = kAbilityPeriod.at(race.ability);
  if (race.ability_charge < static_cast<std::int32_t>(period)) {
    return false;
  }
  if (!use) {
    return true;
  }
  const std::uint32_t own = bit_of(race.index);
  std::int32_t charge = 0;
  switch (race.ability) {
    case kFludentri:
      for (const ShipIndex i : race_list_ships(cosmos, race)) {
        Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
        ship.hit_points = ship.max_hit_points;
      }
      break;
    case kBaliflids:
      // Every other race (extinct ones too) likes us at least PROPOSE_PEACE + 20.
      for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
        Race& other = cosmos.races[static_cast<std::size_t>(r)];
        if (&other != &race) {
          const std::int32_t floor = other.propose_peace + 20;
          if (other.attitude[race.index] < floor) {
            other.attitude[race.index] = wrap_i16(floor);
          }
        }
      }
      break;
    case kSwaparamans:
      for (const ShipIndex i : race_list_ships(cosmos, race)) {
        Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
        ship.power = wrap_i32(static_cast<std::int64_t>(ship.power) * 2);
      }
      break;
    case kFrutmaka:
      // Alien ships in space at our colonies go back to their home star.
      for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
        Star& star = cosmos.stars[static_cast<std::size_t>(s)];
        if ((star.owner_mask & own) == 0) {
          continue;
        }
        for (const ShipIndex i : cosmos_ships_at_star(cosmos, star)) {
          Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
          if (ship.owner != race.index && ship.location_kind == ShipLocation::kSystem) {
            Star& home =
                cosmos.stars.at(static_cast<std::size_t>(cosmos.races.at(static_cast<std::size_t>(ship.owner)).home));
            ship_leave_star_system(session, ship);
            ship_enter_star_system(session, home, ship);
          }
        }
      }
      break;
    case kShevar:
      for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
        const Star& star = cosmos.stars[static_cast<std::size_t>(s)];
        if ((star.ships_mask & own) == 0) {
          continue;
        }
        for (const ShipIndex i : cosmos_ships_at_star(cosmos, star)) {
          Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
          if (ship.owner != race.index) {
            ship.power = 0;
          }
        }
      }
      break;
    case kGovorom: {
      // The least populated own planet (not Cornucopia): its black and white
      // squares get a random habitable colour (buildings stay).
      std::uint16_t lowest = 0xffff;
      Planet* chosen = nullptr;
      for (std::int16_t p = 0; p < cosmos.planet_count; ++p) {
        Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
        if (planet.owner == race.index && planet.type != 10 && planet.population < lowest) {
          lowest = planet.population;
          chosen = &planet;
        }
      }
      if (chosen != nullptr) {
        constexpr std::array<std::uint8_t, 3> kColors = {2, 3, 4};  // 0x86820
        const std::span<Square> squares = planet_squares(cosmos, *chosen);
        for (std::size_t i = 0; i < chosen->surface_squares; ++i) {
          if (squares[i].color == 0 || squares[i].color == 1) {
            squares[i].color = kColors[static_cast<std::size_t>(session.rng.rand() % 3)];
          }
        }
      }
      break;
    }
    case kUngooma: {
      // Every ship on a lane goes back to the star it left. Original bug,
      // fixed in peCustom: the loop stops at slot `ship_count` (the number of
      // used slots).
      const std::int32_t slots =
          session.game->variant == Variant::kPeCustom ? static_cast<std::int32_t>(kMaxShips) : cosmos.ship_count;
      for (std::int32_t i = 0; i < slots; ++i) {
        Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
        if (ship.owner != -1 && ship.location_kind == ShipLocation::kLane) {
          const Lane& lane = cosmos.lanes.at(static_cast<std::size_t>(ship.location));
          const StarIndex star = ship.position[1] == 0.0f ? lane.star_b : lane.star_a;
          ship_enter_star_system(session, cosmos.stars.at(static_cast<std::size_t>(star)), ship);
        }
      }
      break;
    }
    case kDubtaks:
      // Every item known by two races or more becomes known (not unseen).
      for (std::uint16_t item = 0; item < research.count; ++item) {
        ResearchItem& it = research.items[item];
        std::int32_t knowers = 0;
        for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
          if ((it.known_mask & bit_of(static_cast<std::uint32_t>(r))) != 0) {
            ++knowers;
          }
        }
        if (knowers > 1) {
          it.known_mask = static_cast<std::uint8_t>(it.known_mask | own);
          if (static_cast<std::uint16_t>(research.project[race.index]) == item) {
            research.project[race.index] = -1;
            research.progress[race.index] = 0;
          }
        }
      }
      break;
    case kCapelons:
      for (std::int16_t p = 0; p < cosmos.planet_count; ++p) {
        Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
        if (planet.owner == race.index) {
          planet.protected_turn = -1;
        }
      }
      charge = race.ability_charge - period;
      break;
    case kMebes:
      for (std::int16_t p = 0; p < cosmos.planet_count; ++p) {
        Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
        if (planet.owner == race.index) {
          planet.base_population = wrap_u16(planet.base_population + 2);
          planet_recalc_output(session, planet);
        }
      }
      break;
    case kArbryls:
      for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
        const Star& star = cosmos.stars[static_cast<std::size_t>(s)];
        if ((star.owner_mask & own) == 0) {
          continue;
        }
        for (std::int16_t l = 0; l < star.lane_count; ++l) {
          Lane& lane = cosmos.lanes.at(static_cast<std::size_t>(star.lanes[static_cast<std::size_t>(l)]));
          lane.flags |= 2;
        }
      }
      break;
    case kMarmosians:
      // Every third race liking one of our enemies above its DECLARE_WAR
      // drops to it.
      for (std::int16_t u = 0; u < cosmos.race_count; ++u) {
        const auto uu = static_cast<std::size_t>(u);
        if (uu == race.index || race.relation[uu] != kWar) {
          continue;
        }
        for (std::int16_t v = 0; v < cosmos.race_count; ++v) {
          const auto uv = static_cast<std::size_t>(v);
          Race& other = cosmos.races[uv];
          if (uv != uu && uv != race.index && other.declare_war < other.attitude[uu]) {
            other.attitude[uu] = other.declare_war;
          }
        }
      }
      break;
    case kChamachies:
      if (research.project[race.index] != -1) {
        research.progress[race.index] =
            research.items.at(static_cast<std::uint16_t>(research.project[race.index])).cost;
      }
      break;
    case kNimbuloids:
      for (std::int16_t p = 0; p < cosmos.planet_count; ++p) {
        Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
        if (planet.owner == race.index && planet.project_item != kNoItem) {
          planet.progress = wrap_u16(planet.progress + 100);
        }
      }
      break;
    default:
      break;
  }
  race.ability_charge = charge;
  return true;
}

void research_load(Session& session) {
  Research& r = session.game->research;
  r.self_managed = 0;
  r.basics_done = 0;
  for (ResearchItem& item : r.items) {
    item.known_mask = 0;
    item.unseen_mask = 0;
  }
  if (!session.research_tree_loaded) {
    // The global is still the zeroed BSS here (a save load sets the flag
    // first), so the parsed tree is exactly what the loader leaves there.
    r.count = session.tables->research.count;
    r.items = session.tables->research.items;
    session.research_tree_loaded = true;
  }
  r.project.fill(-1);
  r.progress.fill(0);
}

}  // namespace oa::game
