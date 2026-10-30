#include "game/race_ai.h"

#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <vector>

#include "core/fixed_width.h"
#include "game/cosmos.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/race.h"
#include "game/raceneg.h"
#include "game/research.h"
#include "game/ship.h"
#include "game/tables.h"

namespace oa::game {

namespace {

inline constexpr std::uint8_t kItemFactory = 0;
inline constexpr std::uint8_t kItemSurfaceCloaker = 11;
inline constexpr std::uint8_t kItemSurfaceShield = 18;
inline constexpr std::uint8_t kItemSurfaceMegaShield = 19;
inline constexpr std::uint8_t kItemTransportTubes = 21;
inline constexpr std::uint8_t kItemOrbitalDocks = 24;
inline constexpr std::uint8_t kItemOrbitalCloaker = 25;

// Research items of the hulls (`Race_ai_design_ship`).
inline constexpr std::uint16_t kResearchMediumHull = 1;     // Interplanetary Exploration
inline constexpr std::uint16_t kResearchLargeHull = 19;     // Advanced Exploration
inline constexpr std::uint16_t kResearchEnormousHull = 31;  // Large Scale Construction
inline constexpr std::uint16_t kResearchEncapsulation = 5;  // Environmental Encapsulation

// Gizmos of the ship designs.
inline constexpr std::int8_t kGizmoHypersphereDriver = 8;
inline constexpr std::int8_t kGizmoPlasmatron = 5;
inline constexpr std::int8_t kGizmoFergnatzLens = 7;
inline constexpr std::int8_t kDefaultWeapon = 0;      // Mass Barrage Gun
inline constexpr std::int8_t kDefaultGenerator = 27;  // Proton Shaver
inline constexpr std::int8_t kDefaultDrive = 16;      // Tonklin Motor
inline constexpr std::int8_t kDefaultShield = 10;     // Ion Wrap

// Orbital defences (0x8679c shields, 0x86798 weapons), special buildings
// (0x867a0), research priorities (`u16[23]` 0x867a6) and the combat
// specials of the large designs (0x867d4).
constexpr std::array<std::uint8_t, 2> kOrbitalShields = {26, 27};
constexpr std::array<std::uint8_t, 3> kOrbitalWeapons = {28, 29, 30};
constexpr std::array<std::uint8_t, 5> kSpecialBuildings = {12, 13, 14, 15, 37};
constexpr std::array<std::uint16_t, 23> kResearchPriorities = {2,  0,  1,  3,  6,  5,  8,  9,  25, 28, 47, 18,
                                                               21, 19, 10, 14, 22, 26, 31, 52, 40, 38, 30};
constexpr std::array<std::int8_t, 22> kCombatSpecials = {69, 63, 50, 46, 45, 38, 37, 35, 34, 33, 47,
                                                         55, 52, 35, 64, 54, 50, 37, 66, 61, 48, 49};
// Peace drift per shared star by atmosphere (`i16[3]` 0x86792).
constexpr std::array<std::int16_t, 3> kPeaceDrift = {0, -1, -5};

constexpr std::uint32_t bit_of(std::size_t race) { return 1u << (race & 0x1f); }

bool knows(const Research& research, std::uint32_t item, std::uint32_t race) {
  return (research.items.at(item).known_mask & bit_of(race)) != 0;
}

const PlanItem& item_of(const Session& session, std::uint8_t item) { return session.tables->planitems.items.at(item); }

// Returns whether `race` knows the research requirement of planet item `item`.
bool knows_item(const Session& session, std::uint8_t item, std::uint32_t race) {
  return knows(session.game->research, item_of(session, item).research_requirement, race);
}

std::uint8_t hops_to_home(const Cosmos& cosmos, const Race& race, const Planet& planet) {
  return cosmos.hops.at(static_cast<std::size_t>(planet.star)).at(static_cast<std::size_t>(race.home));
}

bool has_item(const Session& session, const Planet& planet, std::uint8_t item) {
  return planet_find_item(session.game->cosmos, planet, item) != kNoSquare;
}

const RaceRank& rank_of(const Cosmos& cosmos, const Race& race) { return cosmos.ranks.at(race.index); }

}  // namespace

void race_ai_turn(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  Research& research = session.game->research;
  const bool ai = race_is_ai(session, race);
  if (ai) {
    race_update_attitude_drift(session, race);
    race_bio_rhythm(session, race);
    race_diplomacy_turn(session, race);
    race_build_targets(session, race);
  }
  for (std::int16_t p = 0; p < cosmos.planet_count; ++p) {
    Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
    if (planet.owner == race.index &&
        (planet.owner != session.player_race || planet.self_managed != 0 || session.autoplay) &&
        planet.project_item == kNoItem) {
      race_ai_manage_planet(session, race, planet);
    }
  }
  if ((race.index != session.player_race || session.autoplay || research.self_managed == -1) &&
      research.project[race.index] == -1) {
    race_ai_choose_research(session, race);
  }
  if (ai) {
    race_use_special_ability(session, race, true);
  }
  const std::vector<ShipIndex> ships = race_list_ships(cosmos, race);
  // `Race_list_ships` 0x3c338 calls `qsort`, whose first push (esi, the
  // count of ship records seen) lands on `Session::stale_dip_arg`; the ship
  // orders' calls that reach it push the race's ship count (both small: only
  // the high word, zero, stays visible under the next attitude drift).
  session.stale_dip_arg = static_cast<std::uint32_t>(cosmos.ship_count);
  for (const ShipIndex i : ships) {
    if (ai) {
      race_ai_ship_orders(session, race, cosmos.ships[static_cast<std::size_t>(i)]);
    }
  }
}

void race_build_targets(Session& session, Race& race) {
  const Cosmos& cosmos = session.game->cosmos;
  race.target_count = 0;
  const std::uint8_t war = race_at_war_mask(session, race);
  const std::uint32_t own = bit_of(race.index);
  const auto add = [&race](StarIndex star) {
    race.targets.at(static_cast<std::size_t>(race.target_count)) = star;
    ++race.target_count;
  };
  if ((cosmos.stars.at(static_cast<std::size_t>(race.home)).owner_mask & own) == 0) {
    add(race.home);
    return;
  }
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    const Star& star = cosmos.stars[static_cast<std::size_t>(s)];
    if ((star.lanes_known_mask & own) != 0 &&
        ((star.explored_mask & own) == 0 || (war & (star.ships_mask | star.owner_mask)) != 0)) {
      add(s);
    }
  }
  if (race.target_count == 0) {
    for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
      const Star& star = cosmos.stars[static_cast<std::size_t>(s)];
      if ((star.owner_mask & own) != 0 && (star.ships_mask & own) == 0) {
        add(s);
      }
    }
  }
}

void race_update_attitude_drift(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  const auto atmosphere = static_cast<std::uint8_t>(cosmos.atmosphere);
  const bool antagonizer = antagonizer_rules(session.game->variant);
  for (std::int16_t t = 0; t < cosmos.race_count; ++t) {
    const auto ut = static_cast<std::size_t>(t);
    if (ut == race.index || race.relation[ut] == kNotMet || cosmos.races[ut].extinct == -1) {
      continue;
    }
    std::int16_t d = 0;
    if (race.relation[ut] == kWar) {
      // Toward a weaker enemy the attitude rises.
      d = wrap_i16(cosmos.ranks[ut].rank - rank_of(cosmos, race).rank);
      if (d < 0) {
        d = 0;
      } else if (d > 3) {
        d = 3;
      }
    } else if (race.relation[ut] == kPeace) {
      for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
        const Star& star = cosmos.stars[static_cast<std::size_t>(s)];
        if ((star.owner_mask & bit_of(race.index)) != 0 && (star.ships_mask & bit_of(ut)) != 0) {
          d = wrap_i16(d + kPeaceDrift.at(atmosphere));
        }
      }
    }
    // Original dead code not ported: a fleet and rank comparison between the
    // war sets of `t` and of this race, guarded by two identical
    // `Race_at_war_mask(t)` calls that must differ.
    if (antagonizer && ut == session.player_race && !session.autoplay) {
      // ANTAG 0x3c670: the bias toward the player.
      if (d < 0) {
        if (atmosphere == 1) {
          d = wrap_i16(d * 2);
        } else if (atmosphere == 2) {
          d = wrap_i16(d * 3);
        }
      } else if (atmosphere == 1) {
        d = static_cast<std::int16_t>(d / 2);
      } else if (atmosphere == 2) {
        d = static_cast<std::int16_t>(d / 3);
      }
    }
    if (d != 0) {
      std::int16_t& a = race.attitude[ut];
      a = wrap_i16(a + d);
      if (a < 50) {
        a = 50;
      } else if (a > 950) {
        a = 950;
      }
    }
  }
  // The loop counter's word stays in the frame where `Race_diplomacy_turn`
  // reads its argument next (`Session::stale_dip_arg`).
  session.stale_dip_arg = (session.stale_dip_arg & 0xffff0000u) | static_cast<std::uint16_t>(cosmos.race_count);
}

void race_bio_rhythm(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  if (cosmos.day % 10 != 0 || race.bio_period == 0) {
    // (A zero period would divide by zero in the original.)
    return;
  }
  // t = (day % period) / period * 2pi (double 0x82000), stored as a float.
  const double two_pi = std::bit_cast<double>(std::uint64_t{0x401921fb5443d6f4});
  float t = f32(static_cast<double>(cosmos.day % race.bio_period) / static_cast<double>(race.bio_period) * two_pi);
  if (t == 0.0f) {
    t = std::bit_cast<float>(std::uint32_t{0x3c23d70a});  // 0.01f
  }
  const double t1 = std::sin(static_cast<double>(t)) * static_cast<double>(race.bio_sin_x);
  const double t2 = std::sin(static_cast<double>(t) * 0.5) * static_cast<double>(race.bio_sin_half_x) + t1;
  // The partial sum is stored as a double at esp+8 (0x3a447), whose high
  // dword lies where `Race_diplomacy_turn` reads its argument local next
  // (`Session::stale_dip_arg`, overwriting the attitude drift's counter).
  session.stale_dip_arg = static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(t2) >> 32);
  float m = f32(std::sin(static_cast<double>(t) * 2.0) * static_cast<double>(race.bio_sin_twice_x) + t2);
  m = f32(static_cast<double>(m) + (0.0f > m ? -0.5 : 0.5));
  const float max_positive = static_cast<float>(race.bio_max_positive);
  if (m > max_positive) {
    m = max_positive;
  } else {
    const float max_negative = static_cast<float>(race.bio_max_negative);
    if (m < max_negative) {
      m = max_negative;
    }
  }
  if (race.index == session.player_race) {
    session.player_mood = wrap_i16(chop(m));
  }
  // The atmosphere's offset: PATCH +2 / +1 / -2, ANTAG 0x3c968 -4 / -2 / -6.
  const bool antagonizer = antagonizer_rules(session.game->variant);
  double offset = antagonizer ? -2.0 : 1.0;
  if (cosmos.atmosphere == Atmosphere::kHostile) {
    offset = antagonizer ? -6.0 : -2.0;
  } else if (cosmos.atmosphere == Atmosphere::kPeaceful) {
    offset = antagonizer ? -4.0 : 2.0;
  }
  m = f32(static_cast<double>(m) + offset);
  // No 50..950 clamp here (the next drift clamps).
  for (std::int16_t i = 0; i < cosmos.race_count; ++i) {
    const auto ui = static_cast<std::size_t>(i);
    if (race.relation[ui] != kNotMet && cosmos.races[ui].extinct == 0) {
      race.attitude[ui] = wrap_i16(chop(static_cast<double>(race.attitude[ui]) + static_cast<double>(m)));
    }
  }
}

void race_ai_manage_planet(Session& session, Race& race, Planet& planet) {
  const PlanItemTable& items = session.tables->planitems;
  Cosmos& cosmos = session.game->cosmos;
  if (planet.industry == 0) {
    return;
  }
  const std::int32_t orbital = race_ai_choose_orbital_project(session, race, planet);
  if (orbital == 1) {
    return;
  }
  const bool orfa = cosmos.races.at(planet.owner).ability == 2;
  if (planet.used_population < planet.max_population) {
    if (planet.population <= planet.used_population || orbital == 2) {
      return;
    }
  } else {
    // Housing full: demolishes the surface building (not a Colony Base nor
    // housing) using the most workers. Original bug kept: the item passed
    // is that of the last surface square scanned (mode 1 ignores it).
    std::int32_t best = 0;
    std::int32_t chosen = -1;
    std::uint8_t last_item = kNoItem;
    const std::span<const Square> squares = planet_squares(std::as_const(cosmos), planet);
    for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
      last_item = squares[i].item;
      if (last_item == kNoItem || last_item == kItemColonyBase || item_of(session, last_item).max_population != 0) {
        continue;
      }
      if (item_of(session, last_item).used_population > best) {
        best = item_of(session, last_item).used_population;
        chosen = i;
      }
    }
    if (chosen == -1) {
      // An `int 3` (0x258b4), then the colony is given up.
      planet_abandon(session, planet);
      return;
    }
    planet_set_square_item(session, planet, static_cast<std::uint16_t>(chosen), last_item, SquareMode::kRemove);
    if (planet.used_population >= planet.population) {
      return;
    }
  }
  bool crowded = planet.free_surface == planet.free_black;
  if (orfa) {
    crowded = false;
  }
  if (planet.free_surface < 2 || crowded) {
    if (knows_item(session, kItemTerraforming, planet.owner)) {
      const std::uint16_t tubes = planet_find_item(cosmos, planet, kItemTransportTubes);
      if (tubes != kNoSquare) {
        planet_set_square_item(session, planet, tubes, kItemTransportTubes, SquareMode::kRemove);
        if (planet_can_place_item(session, planet, kItemTerraforming, tubes)) {
          planet_place_item_if_free(session, planet, kItemTerraforming, tubes, 0);
        }
        return;
      }
      if (planet_auto_place_item(session, planet, kItemTerraforming, 0) != kNoSquare) {
        return;
      }
    }
    // Demolishes the building with the lowest research requirement among
    // those whose housing can go (same last-square item bug).
    std::int32_t lowest = 99999;
    std::int32_t chosen = -1;
    std::uint8_t last_item = kNoItem;
    const std::span<const Square> squares = planet_squares(std::as_const(cosmos), planet);
    for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
      last_item = squares[i].item;
      if (last_item == kNoItem) {
        continue;
      }
      const PlanItem& it = item_of(session, last_item);
      if (it.max_population < static_cast<std::int32_t>(planet.max_population) - planet.population &&
          it.research_requirement < lowest) {
        lowest = it.research_requirement;
        chosen = i;
      }
    }
    if (chosen != -1) {
      planet_set_square_item(session, planet, static_cast<std::uint16_t>(chosen), last_item, SquareMode::kRemove);
      if (static_cast<std::int32_t>(planet.population) - planet.used_population <= 0) {
        return;
      }
    }
  }
  if (planet.free_surface == 0 || (planet.free_black == planet.free_surface && !orfa)) {
    return;
  }
  // Probe: a Factory built at once and demolished again (it stays when the
  // demolition is refused: original quirk).
  const std::uint16_t probe = planet_auto_place_item(session, planet, kItemFactory, -1);
  if (probe == kNoSquare) {
    planet_auto_place_item(session, planet, kItemTransportTubes, 0);
    return;
  }
  planet_set_square_item(session, planet, probe, kItemFactory, SquareMode::kRemove);
  if (race_ai_planet_specials(session, race, planet)) {
    return;
  }
  const std::int32_t focus = race_ai_planet_focus(session, race, planet);
  // The candidates (jump table 0x3a590: industry, research, prosperity,
  // housing).
  struct Candidate {
    std::uint8_t item;
    std::int16_t focus_value;
    std::int16_t total;
  };
  std::vector<Candidate> candidates;
  for (std::uint8_t i = 0; i < PlanItemTable::kItems; ++i) {
    if (i == kItemColonyBase || i >= items.surface_count) {
      continue;
    }
    const PlanItem& it = items.items[i];
    if (it.research_requirement != 0xff && !knows(session.game->research, it.research_requirement, planet.owner)) {
      continue;
    }
    if (static_cast<std::int32_t>(planet.max_population) - planet.used_population < it.used_population) {
      continue;
    }
    std::int8_t value = 0;
    switch (focus) {
      case 0:
        value = it.industry;
        break;
      case 1:
        value = it.research;
        break;
      case 2:
        value = it.prosperity;
        break;
      default:
        value = it.max_population;
        break;
    }
    if (value == 0) {
      continue;
    }
    candidates.push_back({i, value, wrap_i16(it.max_population + it.prosperity + it.research + it.industry)});
  }
  if (candidates.empty()) {
    return;
  }
  const std::int32_t roll = session.rng.rand() % 100;
  std::uint8_t chosen = kNoItem;
  std::int32_t best = 0;
  for (const Candidate& c : candidates) {
    if (planet_find_item(cosmos, planet, c.item) == kNoSquare) {
      // The first item the planet lacks ends the scan; it is taken when it
      // costs less than 70 days (the best so far stays chosen otherwise).
      if (items.items[c.item].cost / planet.industry < 70) {
        chosen = c.item;
      }
      break;
    }
    const std::int32_t v = roll > 65 ? c.total : c.focus_value;
    if (v > best) {
      best = v;
      chosen = c.item;
    }
  }
  if (chosen != kNoItem) {
    planet_auto_place_item(session, planet, chosen, 0);
  }
}

std::int32_t race_ai_choose_orbital_project(Session& session, Race& race, Planet& planet) {
  Cosmos& cosmos = session.game->cosmos;
  // Shipyard.
  if (!has_item(session, planet, kItemShipyard) &&
      (planet.industry > 3 || static_cast<std::int32_t>(planet.free_surface) - planet.free_black < 5) &&
      (cosmos.day > 170 || cosmos.atmosphere == Atmosphere::kHostile) &&
      knows_item(session, kItemShipyard, race.index)) {
    // Original bug kept: a failed placement (0xffff) returns 1 too.
    return planet_auto_place_item(session, planet, kItemShipyard, 0) != 0 ? 1 : 2;
  }
  // A new ship (never for the player's self-managed planets).
  if (planet_can_build_ship(session, planet) && (planet.owner != session.player_race || session.autoplay)) {
    const std::uint16_t square = planet_auto_place_item(session, planet, kItemShip, 0);
    if (square != kNoSquare) {
      Ship* ship = race_ai_new_ship(session, race, planet);
      if (ship != nullptr) {
        race_ai_design_ship(session, race, *ship, planet);
        planet_set_square_ship(session, planet, square, ship_index(cosmos, *ship));
      }
      return 1;
    }
  }
  if (planet.free_orbital > 2 && planet.owner % 2 == 0 && planet.industry > 20 && session.rng.rand() % 100 < 3 &&
      planet_can_place_item(session, planet, kItemOrbitalCloaker, kNoSquare) &&
      !has_item(session, planet, kItemOrbitalCloaker)) {
    planet_auto_place_item(session, planet, kItemOrbitalCloaker, 0);
    return 1;
  }
  if (planet.free_orbital > 2 && planet.owner % 3 != 0 && planet.industry > 25 && session.rng.rand() % 100 < 2 &&
      planet_can_place_item(session, planet, kItemOrbitalDocks, kNoSquare) &&
      !has_item(session, planet, kItemOrbitalDocks)) {
    planet_auto_place_item(session, planet, kItemOrbitalDocks, 0);
    return 1;
  }
  if (planet.free_orbital <= 1) {
    return 0;
  }
  const std::uint8_t h = hops_to_home(cosmos, race, planet);
  const std::int32_t penalty = h < 3 ? (3 - h) * 2 : 0;
  // Starts the first missing known defence of `table`, else a random known
  // one; returns whether one was started.
  const auto defend = [&](std::span<const std::uint8_t> table) {
    std::vector<std::uint8_t> known;
    for (const std::uint8_t item : table) {
      if (knows_item(session, item, race.index)) {
        known.push_back(item);
      }
    }
    for (const std::uint8_t item : known) {
      if (!has_item(session, planet, item)) {
        planet_auto_place_item(session, planet, item, 0);
        return true;
      }
    }
    if (!known.empty()) {
      const auto pick = static_cast<std::size_t>(session.rng.rand() % static_cast<std::int32_t>(known.size()));
      planet_auto_place_item(session, planet, known[pick], 0);
      return true;
    }
    return false;
  };
  const std::uint32_t output = static_cast<std::uint32_t>(static_cast<std::uint16_t>(planet.research)) +
                               static_cast<std::uint16_t>(planet.industry);
  // Original quirk: empty counts become 1 on the planet (until the next
  // recompute).
  if (planet.shield_count == 0) {
    planet.shield_count = 1;
  }
  if (race.ai_build_threshold_a - penalty < static_cast<std::int32_t>(output / planet.shield_count) &&
      defend(kOrbitalShields)) {
    return 1;
  }
  if (planet.weapon_count == 0) {
    planet.weapon_count = 1;
  }
  if (race.ai_build_threshold_b - penalty < static_cast<std::int32_t>(output / planet.weapon_count) &&
      defend(kOrbitalWeapons)) {
    return 1;
  }
  return 0;
}

bool race_ai_planet_specials(Session& session, Race& race, Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  // Original quirk: the ruins flag is cleared when the dig starts.
  if ((planet.flags & kPlanetRuins) != 0 && planet_auto_place_item(session, planet, kItemXenoDig, 0) != kNoSquare) {
    planet.flags = static_cast<std::uint8_t>(planet.flags & ~kPlanetRuins);
    return true;
  }
  if (planet.industry <= 3 || planet.prosperity == 0 || !has_item(session, planet, kItemShipyard) ||
      static_cast<std::int32_t>(planet.free_surface) - planet.free_black <= 2) {
    return false;
  }
  if (planet.shield_count < 6 && planet_can_place_item(session, planet, kItemSurfaceShield, kNoSquare)) {
    if (!has_item(session, planet, kItemSurfaceShield)) {
      planet_auto_place_item(session, planet, kItemSurfaceShield, 0);
      return true;
    }
    const std::uint8_t h = hops_to_home(cosmos, race, planet);
    const std::uint32_t period = h == 0 ? 200 : h == 1 ? 225 : h < 4 ? 300 : 400;
    if (planet.shield_count < static_cast<std::uint32_t>(cosmos.day - planet.acquired_day) / period) {
      const std::uint8_t item = planet_can_place_item(session, planet, kItemSurfaceMegaShield, kNoSquare)
                                    ? kItemSurfaceMegaShield
                                    : kItemSurfaceShield;
      planet_auto_place_item(session, planet, item, 0);
      return true;
    }
  }
  if (planet.owner % 2 != 0 && planet.industry > 20 && session.rng.rand() % 100 < 4 &&
      planet_can_place_item(session, planet, kItemSurfaceCloaker, kNoSquare) &&
      !has_item(session, planet, kItemSurfaceCloaker)) {
    planet_auto_place_item(session, planet, kItemSurfaceCloaker, 0);
    return true;
  }
  for (const std::uint8_t item : kSpecialBuildings) {
    if (planet_can_place_item(session, planet, item, kNoSquare) && !has_item(session, planet, item)) {
      planet_auto_place_item(session, planet, item, 0);
      return true;
    }
  }
  return false;
}

std::int32_t race_ai_planet_focus(const Session& session, const Race&, const Planet& planet) {
  const Research& research = session.game->research;
  const std::int32_t room = static_cast<std::int32_t>(planet.max_population) - planet.population;
  const std::int32_t idle = static_cast<std::int32_t>(planet.population) - planet.used_population;
  const auto industry = static_cast<std::uint16_t>(planet.industry);
  const auto prosperity = static_cast<std::uint16_t>(planet.prosperity);
  if (room < 2 && idle < 2) {
    return 3;
  }
  if (prosperity < 2) {
    return 2;
  }
  if (industry <= 1) {
    return 0;
  }
  if (planet.research == 0) {
    return 1;
  }
  if (industry <= 3) {
    return 0;
  }
  if (prosperity == 0 && planet.population < planet.max_population) {
    return 2;  // dead: prosperity >= 2 here
  }
  if (session.tables->planitems.pop_growth / static_cast<std::int32_t>(prosperity) > 15 && room > 3 && idle != 1) {
    return 2;
  }
  if (!knows(research, research.count - 1u, planet.owner)) {
    std::uint32_t i = industry;
    std::uint32_t s = static_cast<std::uint16_t>(planet.research);
    if (has_item(session, planet, kItemHyperpowerPlant)) {
      s += static_cast<std::uint32_t>(static_cast<std::int32_t>(i) >> 1);
    }
    // Original bug kept: the Internet test compares with 0xfff, so it always
    // holds.
    i += static_cast<std::uint32_t>(static_cast<std::int32_t>(s) >> 1);
    if (s < i) {
      return 1;
    }
  }
  return 0;
}

void race_ai_choose_research(Session& session, Race& race) {
  Research& research = session.game->research;
  std::vector<std::uint16_t> candidates;
  for (std::int16_t i = 0; i < static_cast<std::int32_t>(research.count) && candidates.size() < 32; ++i) {
    const auto item = static_cast<std::uint16_t>(i);
    if (!knows(research, item, race.index) && research_can_research(research, race.index, item)) {
      candidates.push_back(item);
    }
  }
  if (candidates.empty()) {
    return;
  }
  bool skip_priorities = false;
  if (knows(research, kResearchEncapsulation, race.index) && session.rng.rand() % 100 < 45) {
    skip_priorities = true;
  }
  std::int32_t chosen = -1;
  if (!skip_priorities) {
    // The priority list is used as a set, in item order (original quirk).
    for (const std::uint16_t c : candidates) {
      for (const std::uint16_t p : kResearchPriorities) {
        if (p == c) {
          chosen = c;
          break;
        }
      }
      if (chosen != -1) {
        break;
      }
    }
  }
  if (chosen < 0) {
    chosen = candidates[static_cast<std::size_t>(session.rng.rand() % static_cast<std::int32_t>(candidates.size()))];
  }
  research.project[race.index] = static_cast<std::int16_t>(chosen);
}

void race_ai_ship_orders(Session& session, Race& race, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  const ShipLocation loc = ship.location_kind;
  if (loc == ShipLocation::kBuilding || loc == ShipLocation::kRefit || loc == ShipLocation::kLane) {
    return;
  }
  if (ship.owner == session.player_race && !session.autoplay) {
    ship.destination_kind = ShipOrder::kNothing;
    ship.destination = 0;
    ship.stale_destination = 0;
    return;
  }
  if (ship.destination_kind != ShipOrder::kNothing) {
    return;
  }
  if (ship.location_kind == ShipLocation::kOrbit) {
    planet_undock_ship(session, cosmos.planets.at(static_cast<std::size_t>(ship.location)), ship);
  }
  const Star& here = cosmos.stars.at(static_cast<std::size_t>(ship.location));
  if (ship.has_colonizer == -1) {
    const PlanetIndex site = race_ai_choose_colony_site(session, race, here);
    if (site != kNone) {
      ship.destination_kind = ShipOrder::kColonize;
      ship.destination = site;
      return;
    }
  }
  if (race.target_count == 0 || (ship.lane_speed <= 0 && ship.hyper_speed <= 0)) {
    return;
  }
  // Original bugs kept: each probe heads the ship for the probed target
  // (unless it is the current star), and with no reachable target the last
  // probe stands.
  std::int16_t best_hops = 100;
  StarIndex best = kNone;
  for (std::int16_t i = 0; i < race.target_count; ++i) {
    const StarIndex target = race.targets[static_cast<std::size_t>(i)];
    const std::int16_t h = ship_set_destination_star(session, ship, cosmos.stars.at(static_cast<std::size_t>(target)));
    if (h == 0) {
      best = target;
      break;
    }
    if (h < best_hops) {
      best = target;
      best_hops = h;
    }
  }
  if (best != kNone) {
    ship_set_destination_star(session, ship, cosmos.stars.at(static_cast<std::size_t>(best)));
  }
}

void race_ai_design_ship(Session& session, Race& race, Ship& ship, Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  const Research& research = session.game->research;
  std::uint8_t largest = 0;
  if (knows(research, kResearchMediumHull, race.index)) {
    largest = 1;
    if (knows(research, kResearchLargeHull, race.index)) {
      largest = 2;
      if (knows(research, kResearchEnormousHull, race.index)) {
        largest = 3;
      }
    }
  }
  std::int16_t k = static_cast<std::int16_t>(cosmos.day / 250);
  if (k < 1) {
    k = 1;
  } else if (k > 8) {
    k = 8;
  }
  // The largest hull whose estimated build time is below SHIP_PATIENCE.
  std::int16_t hull = largest;
  for (; hull >= 0; --hull) {
    ship_set_hull_size(session, ship, static_cast<std::uint8_t>(hull));
    const std::int32_t cost = ship_industry_cost(session, ship);
    if (planet.industry == 0) {
      planet.industry = 1;  // original quirk: written to the planet
    }
    if ((k * ship.slot_count + cost) / static_cast<std::uint16_t>(planet.industry) + 1 < race.ship_patience) {
      break;
    }
  }
  if (hull < 0) {
    hull = 0;
  }
  ship_set_hull_size(session, ship, static_cast<std::uint8_t>(hull));
  std::int8_t w = ship_best_gizmo_for_race(session, 0, race.index);
  std::int8_t g = ship_best_gizmo_for_race(session, 4, race.index);
  std::int8_t d = ship_best_gizmo_for_race(session, 2, race.index);
  std::int8_t s = ship_best_gizmo_for_race(session, 1, race.index);
  const std::int8_t l =
      ship_race_has_gizmo(session, ship, kGizmoStarLaneHyperdrive) ? kGizmoStarLaneHyperdrive : kGizmoStarLaneDrive;
  if (g == -1) {
    g = kDefaultGenerator;
  }
  if (d == -1) {
    d = kDefaultDrive;
  }
  if (w == -1) {
    w = kDefaultWeapon;
  }
  if (s == -1) {
    s = kDefaultShield;
  }
  const auto install = [&](std::int8_t gizmo, std::int32_t slot) { ship_install_gizmo(session, ship, gizmo, slot); };
  const auto has = [&](std::int8_t gizmo) { return ship_race_has_gizmo(session, ship, gizmo); };
  const std::int16_t rank = rank_of(cosmos, race).rank;
  const std::int32_t day = cosmos.day;
  std::int32_t n = 0;
  // The remaining slots of the large hulls (`Race_ai_fill_specials` 0x3c194):
  // random known combat specials, then generators.
  const auto fill_specials = [&]() {
    std::vector<std::int8_t> candidates;
    for (const std::int8_t gizmo : kCombatSpecials) {
      if (has(gizmo)) {
        candidates.push_back(gizmo);
      }
    }
    while (n < ship.slot_count && !candidates.empty()) {
      const auto i = static_cast<std::size_t>(session.rng.rand() % static_cast<std::int32_t>(candidates.size()));
      install(candidates[i], n++);
      candidates[i] = candidates.back();
      candidates.pop_back();
    }
    while (n < ship.slot_count) {
      install(g, n++);
    }
  };
  switch (hull) {
    case 0: {
      // `Race_ai_fill_small_hull` 0x3ba70 (the shield is unused).
      install(g, 0);
      install(d, 1);
      install(l, 2);
      const std::int8_t last = race_ai_wants_colony_ship(session, race) ? kGizmoColonizer : w;
      install(last, 3);
      install(last, 4);
      break;
    }
    case 1:
      // `Race_ai_fill_medium_hull` 0x3baec. Original quirk: `n` may reach
      // 10, and `ship_install_gizmo` accepts slot 10.
      install(g, 0);
      install(g, 1);
      install(d, 2);
      install(l, 3);
      install(w, 4);
      n = 5;
      if (race_ai_wants_colony_ship(session, race)) {
        install(kGizmoColonizer, 5);
        install(kGizmoColonizer, 6);
        n = 7;
        if (day > 400) {
          install(kGizmoInvasionModule, 7);
          n = 8;
        }
      }
      if (has(kGizmoRecaller)) {
        install(kGizmoRecaller, n++);
      }
      if (day > 800) {
        install(kGizmoInvasionModule, n++);
      }
      if (l == kGizmoStarLaneDrive || day > 800 || rank >= 3) {
        install(l, n++);
      }
      while (n < 10) {
        install(session.rng.rand() % 100 > 35 ? w : s, n);
        ++n;
      }
      break;
    case 2:
      // `Race_ai_fill_large_hull` 0x3bc6c.
      install(g, 0);
      install(g, 1);
      install(g, 2);
      install(d, 3);
      install(l, 4);
      install(w, 5);
      n = 6;
      if (w != kGizmoHypersphereDriver && has(kGizmoHypersphereDriver)) {
        install(kGizmoHypersphereDriver, n++);
      } else {
        install(w != kGizmoPlasmatron && has(kGizmoPlasmatron) ? kGizmoPlasmatron : w, n++);
      }
      if (session.rng.rand() % 100 > 50) {
        install(s, n++);
      } else {
        install(w != kGizmoFergnatzLens && has(kGizmoFergnatzLens) ? kGizmoFergnatzLens : w, n++);
      }
      if (race_ai_wants_colony_ship(session, race)) {
        install(kGizmoColonizer, n++);
        install(kGizmoColonizer, n++);
        if (session.game->cosmos.atmosphere == Atmosphere::kHostile) {
          install(kGizmoInvasionModule, n++);
        }
        if (day > 400) {
          install(kGizmoInvasionModule, n++);
        }
      }
      if (day > 1000) {
        install(kGizmoInvasionModule, n++);
      }
      if (l == kGizmoStarLaneDrive || day > 800) {
        install(l, n++);
      }
      if (has(kGizmoRecaller)) {
        install(kGizmoRecaller, n++);
      }
      if (has(kGizmoAccutron)) {
        install(kGizmoAccutron, n++);
      }
      if (rank > 3) {
        install(l, n++);
      }
      if (has(kGizmoReplenisher)) {
        install(kGizmoReplenisher, n++);
      }
      fill_specials();
      break;
    case 3:
      // `Race_ai_fill_enormous_hull` 0x3bee4.
      install(g, 0);
      install(g, 1);
      install(g, 2);
      install(g, 3);
      install(d, 4);
      install(d, 5);
      install(l, 6);
      install(w, 7);
      install(s, 8);
      n = 9;
      if (has(kGizmoHypersphereDriver)) {
        install(kGizmoHypersphereDriver, n++);
      }
      install(has(kGizmoFergnatzLens) ? kGizmoFergnatzLens : w, n++);
      install(has(kGizmoPlasmatron) ? kGizmoPlasmatron : w, n++);
      if (race_ai_wants_colony_ship(session, race)) {
        install(kGizmoColonizer, n++);
        install(kGizmoColonizer, n++);
        install(kGizmoColonizer, n++);
        if (session.game->cosmos.atmosphere == Atmosphere::kHostile) {
          install(kGizmoInvasionModule, n++);
        }
        if (day > 400) {
          install(kGizmoInvasionModule, n++);
          install(kGizmoInvasionModule, n++);
        }
      } else if (day > 1000) {
        install(kGizmoInvasionModule, n++);
        install(kGizmoInvasionModule, n++);
      }
      if (l == kGizmoStarLaneDrive || day > 800) {
        install(l, n++);
      }
      if (has(kGizmoRecaller)) {
        install(kGizmoRecaller, n++);
      }
      if (has(kGizmoAccutron)) {
        install(kGizmoAccutron, n++);
      }
      if (has(kGizmoReplenisher)) {
        install(kGizmoReplenisher, n++);
      }
      if (rank > 3) {
        install(l, n++);
      }
      fill_specials();
      break;
    default:
      break;
  }
}

Ship* race_ai_new_ship(Session& session, Race& race, Planet& planet) {
  Cosmos& cosmos = session.game->cosmos;
  if (race_ship_allowance(cosmos, race) < 1) {
    return nullptr;
  }
  Ship* ship = cosmos_alloc_ship(session, race.index);
  if (ship == nullptr) {
    return nullptr;
  }
  const auto cost = static_cast<std::uint16_t>(ship_industry_cost(session, *ship));
  ship->location_kind = ShipLocation::kBuilding;
  ship->location = planet_index(cosmos, planet);
  ship->built_day = static_cast<std::int32_t>(cost / static_cast<std::uint16_t>(planet.industry)) + cosmos.day + 1;
  return ship;
}

bool race_ai_wants_colony_ship(Session& session, const Race& race) {
  const Cosmos& cosmos = session.game->cosmos;
  const std::int16_t rank = rank_of(cosmos, race).rank;
  const std::int16_t colonies = rank_of(cosmos, race).colonies;
  if (rank == 0 && colonies >= 6 && cosmos.day < 600) {
    return false;
  }
  if (cosmos.day > 1800) {
    return true;
  }
  const std::int32_t r = session.rng.rand() % 100;
  if (rank == 1 && colonies > 8 && cosmos.day < 1000) {
    return r < 51;
  }
  return r > 20 || colonies < 10;
}

PlanetIndex race_ai_choose_colony_site(const Session& session, const Race& race, const Star& star) {
  const Cosmos& cosmos = session.game->cosmos;
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    const auto ur = static_cast<std::size_t>(r);
    if (cosmos.races[ur].extinct != -1 && ur != race.index && race.relation[ur] == kAlliance &&
        (star.owner_mask & bit_of(ur)) != 0) {
      return kNone;  // an ally lives here
    }
  }
  std::uint16_t best_score = 0;
  PlanetIndex best = kNone;
  for (std::int16_t i = 0; i < star.planet_count; ++i) {
    const PlanetIndex p = star.planets[static_cast<std::size_t>(i)];
    const Planet& planet = cosmos.planets.at(static_cast<std::size_t>(p));
    if (planet.owner == kNoOwner) {
      const auto score = static_cast<std::uint16_t>(planet_habitability(cosmos, planet, race.ability == 2));
      if (best_score < score) {
        best = p;
        best_score = score;
      }
    }
  }
  std::uint16_t threshold = 30;
  if ((star.owner_mask & bit_of(race.index)) == 0) {
    // No colony here yet: lowered when a lane neighbour holds one.
    for (std::int16_t l = 0; l < star.lane_count; ++l) {
      const Lane& lane = cosmos.lanes.at(static_cast<std::size_t>(star.lanes[static_cast<std::size_t>(l)]));
      const StarIndex far = lane.star_a == star.index ? lane.star_b : lane.star_a;
      if ((cosmos.stars.at(static_cast<std::size_t>(far)).owner_mask & bit_of(race.index)) != 0) {
        threshold = 15;
        break;
      }
    }
  } else {
    // A colony here and an enemy owning the rest alone.
    for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
      const auto ur = static_cast<std::size_t>(r);
      if (race.relation[ur] == kWar && star.owner_mask == bit_of(ur)) {
        threshold = 15;
        break;
      }
    }
  }
  return best_score > threshold ? best : kNone;
}

}  // namespace oa::game
