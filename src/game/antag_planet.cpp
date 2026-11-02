#include "game/antag_planet.h"

#include <array>
#include <span>
#include <vector>

#include "game/planet.h"
#include "game/raceneg.h"
#include "game/tables.h"

namespace oa::game {

namespace {

// Planet items of this module (`planitem.txt` order).
constexpr std::uint8_t kFactory = 0;
constexpr std::uint8_t kAgriplot = 1;
constexpr std::uint8_t kLaboratory = 2;
constexpr std::uint8_t kHabitat = 3;
constexpr std::uint8_t kMetroplex = 4;
constexpr std::uint8_t kIndustrialMegafacility = 6;
constexpr std::uint8_t kHydroponifer = 7;
constexpr std::uint8_t kResearchCampus = 8;
constexpr std::uint8_t kLogicFactory = 9;
constexpr std::uint8_t kEngineeringRetreat = 10;
constexpr std::uint8_t kSurfaceCloaker = 11;
constexpr std::uint8_t kCloningPlant = 15;
constexpr std::uint8_t kSurfaceShield = 18;
constexpr std::uint8_t kSurfaceMegaShield = 19;
constexpr std::uint8_t kOutpost = 20;
constexpr std::uint8_t kTransportTubes = 21;
constexpr std::uint8_t kOrbitalDocks = 24;
constexpr std::uint8_t kOrbitalCloaker = 25;
constexpr std::uint8_t kOrbitalShield = 26;
constexpr std::uint8_t kOrbitalMegaShield = 27;
constexpr std::uint8_t kLushGrowthBomb = 37;

// peCustom: the days ships of races seen as a danger stay at a star before
// its planets build planetary defences first.
constexpr std::int32_t kDangerDays = 5;

// The special buildings, in order (0x96918, the bytes of PATCH 0x867a0).
constexpr std::array<std::uint8_t, 5> kSpecials = {kItemHyperpowerPlant, kItemFertilizationPlant, kItemInternet,
                                                   kCloningPlant, kLushGrowthBomb};

std::uint32_t bit(std::uint32_t race) { return 1u << (race & 0x1f); }

const PlanItem& item_of(const Session& session, std::uint8_t item) { return session.tables->planitems.items.at(item); }

// The planet's fields as the code reads them (unsigned words).
std::int32_t u16(std::int16_t v) { return static_cast<std::uint16_t>(v); }
std::int32_t room(const Planet& p) { return static_cast<std::int32_t>(p.max_population) - p.population; }
std::int32_t idle(const Planet& p) { return static_cast<std::int32_t>(p.population) - p.used_population; }

// Tells whether the planet's owner knows the research of `item` ("known(i)").
bool known(const Session& session, const Planet& planet, std::uint8_t item) {
  const std::uint8_t req = item_of(session, item).research_requirement;
  // 254 ("never") reads past the research items: taken as unknown.
  return req == 0xff ||
         (req < kMaxResearchItems && (session.game->research.items[req].known_mask & bit(planet.owner)) != 0);
}

bool absent(const Session& session, const Planet& planet, std::uint8_t item) {
  return planet_find_item(session.game->cosmos, planet, item) == kNoSquare;
}

bool pe_custom(const Session& session) { return session.game->variant == Variant::kPeCustom; }

// Whether `square`, returned by `planet_auto_place_item`, is a placement.
// Original bug (as PATCH), fixed in peCustom: compared with 0, so "no
// square" (0xffff) counts as placed, square 0 as failed.
bool placed(const Session& session, std::uint16_t square) {
  return pe_custom(session) ? square != kNoSquare : square != 0;
}

// Whether `item` is on the planet. Original bug, fixed in peCustom: the
// square found is compared with 0, true for "no square" (0xffff).
bool present(const Session& session, const Planet& planet, std::uint8_t item) {
  const std::uint16_t square = planet_find_item(session.game->cosmos, planet, item);
  return pe_custom(session) ? square != kNoSquare : square != 0;
}

bool is_home(const Race& race, const Planet& planet) { return planet.star == race.home; }

// Tells whether a square next to surface square `square` is built (the
// orbital squares have no neighbours).
bool next_to_built(const Session& session, const Planet& planet, std::span<const Square> squares,
                   std::uint16_t square) {
  const PlanItemTable& items = session.tables->planitems;
  const std::int32_t rc = planet_square_rowcol(items, planet, square);
  const std::int32_t column = rc & 0xffff;
  const std::int32_t row = (rc >> 16) & 0xffff;
  const auto built = [&](std::int32_t r, std::int32_t c) {
    const std::uint16_t s = planet_square_at(items, planet, r, c);
    return s != kNoSquare && (squares[s].flags & kSquareBuilt) != 0;
  };
  return built(row - 1, column) || built(row + 1, column) || built(row, column - 1) || built(row, column + 1);
}

// `Planet_count_buildable_squares` / `_black_squares` (stop at 100).
std::int32_t count_buildable(const Session& session, const Planet& planet, bool black) {
  const Cosmos& cosmos = session.game->cosmos;
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  const bool orfa = cosmos.races.at(planet.owner).ability == 2;
  std::int32_t n = 0;
  for (std::uint16_t i = 0; i < planet.square_count && n < 100; ++i) {
    const Square& sq = squares[i];
    if ((sq.color & kSquareRuins) != 0 || sq.item != kNoItem) {
      continue;
    }
    if (black ? sq.color != 0 : (sq.color == 0 && !orfa)) {
      continue;
    }
    if (next_to_built(session, planet, squares, i)) {
      ++n;
    }
  }
  return n;
}

// Sums the built squares' values of `items` over the squares [first, end).
template <std::size_t N>
std::int32_t strength(const Cosmos& cosmos, const Planet& planet, std::uint16_t first, std::uint16_t end,
                      const std::array<std::pair<std::uint8_t, std::int32_t>, N>& values) {
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  std::int32_t s = 0;
  for (std::uint16_t i = first; i < end; ++i) {
    if (squares[i].item == kNoItem || (squares[i].flags & kSquareBuilt) == 0) {
      continue;
    }
    for (const auto& [item, value] : values) {
      if (squares[i].item == item) {
        s += value;
      }
    }
  }
  return s;
}

// The industry cost of the item of a square, as the original reads it: an
// empty square (0xff) reads past the table, at the bytes 10..11 of the name
// of research item 72 (unused, normally zero).
std::int32_t square_cost(const Session& session, std::uint8_t item) {
  if (item < PlanItemTable::kItems) {
    return item_of(session, item).cost;
  }
  if (item == kNoItem) {
    const auto& name = session.game->research.items[72].name;
    return static_cast<std::uint8_t>(name[10]) | static_cast<std::uint8_t>(name[11]) << 8;
  }
  return 0;
}

// `relation[race][owner]` as the original reads it, also for an unowned
// planet (0xff): the byte 0x2bf of the race record, i.e. the low byte of the
// next race's `targets[99]`, or for race 6 the low byte of star 1's lane
// count.
std::uint8_t relation_byte(const Cosmos& cosmos, RaceIndex race, std::uint8_t owner) {
  if (owner < kMaxRaces) {
    return cosmos.races[race].relation[owner];
  }
  if (race + 1u < kMaxRaces) {
    return static_cast<std::uint8_t>(cosmos.races[race + 1u].targets[99]);
  }
  return static_cast<std::uint8_t>(cosmos.stars[1].lane_count);
}

}  // namespace

// 0x40590
bool antag_race_research_complete(Session& session, const Race& race) {
  if (!session.research_complete_valid) {
    const Research& research = session.game->research;
    session.research_complete = true;
    for (std::uint16_t i = 0; i < research.count; ++i) {
      if ((research.items[i].known_mask & bit(race.index)) == 0) {
        session.research_complete = false;
        break;
      }
    }
    session.research_complete_valid = true;
  }
  return session.research_complete;
}

// 0x3d8f0
void antag_race_ai_manage_planet(Session& session, Race& race, Planet& planet) {
  const PlanItemTable& items = session.tables->planitems;
  Cosmos& cosmos = session.game->cosmos;
  FreeSquaresByColour free = planet_count_free_squares_by_colour(session, planet);
  if (antag_race_research_complete(session, race)) {
    free.industry += free.research;
    free.prosperity += free.research;
    free.research = 0;
  }
  // The first buildings, finished at once and for free.
  const std::int32_t buildings = planet_count_buildings(cosmos, planet);
  if (buildings < 3) {
    std::uint8_t basic = kAgriplot;
    if (buildings < 1) {
      basic = free.industry > 0 ? kFactory : kAgriplot;
    } else if (buildings < 2) {
      basic = free.prosperity > 0 && u16(planet.industry) > 1 ? kAgriplot : kFactory;
    } else if (free.research > 0 && u16(planet.prosperity) >= 2 && u16(planet.industry) >= 2) {
      basic = kLaboratory;
    } else if (free.industry > 0 && u16(planet.prosperity) >= 2) {
      basic = kFactory;
    }
    if (planet_auto_place_item(session, planet, basic, -1) != kNoSquare) {
      return;
    }
  }
  if (planet.industry == 0) {
    return;
  }
  const bool starved = u16(planet.prosperity) < 2 && room(planet) < 1 && idle(planet) < 1;
  const bool crowded = room(planet) < 1 && idle(planet) < 2;
  const std::int32_t orbital = antag_race_ai_choose_orbital_project(session, race, planet);
  if (orbital == 1) {
    return;
  }
  const bool buildable = planet_count_buildable_squares(session, planet) > 0;
  const bool buildable_black = planet_count_buildable_black_squares(session, planet) > 0;
  // Probes for a free ruins square: an instant dig (always refused: not a
  // surface item) demolished at once.
  const std::uint16_t dig = planet_auto_place_item(session, planet, kItemXenoDig, -1);
  planet_set_square_item(session, planet, dig, kItemXenoDig, SquareMode::kRemove);
  if (planet.free_surface == 0 || (!buildable && !buildable_black && dig == kNoSquare)) {
    antag_race_ai_rebuild_planet_square(session, race, planet);
    return;
  }
  if (dig != kNoSquare) {
    antag_race_ai_planet_specials(session, race, planet);
    return;
  }
  std::uint16_t square = kNoSquare;
  if (buildable) {
    const std::span<Square> squares = planet_squares(cosmos, planet);
    if (planet.used_population < planet.max_population) {
      // (`orbital == 2` never happens: 0x3e1cc returns 0 or 1.)
      if (planet.population <= planet.used_population || orbital == 2) {
        return;
      }
    } else {
      // Housing full: demolish the first building of the largest population
      // need that adds no housing.
      std::int32_t most = 0;
      std::uint16_t victim = kNoSquare;
      std::uint8_t last_item = kNoItem;
      for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
        last_item = squares[i].item;
        if (last_item != kNoItem && last_item != kItemColonyBase && item_of(session, last_item).max_population == 0 &&
            most < item_of(session, last_item).used_population) {
          most = item_of(session, last_item).used_population;
          victim = i;
        }
      }
      if (victim == kNoSquare) {
        // The original breaks into the debugger (int 3) first.
        planet_abandon(session, planet);
        return;
      }
      // Original quirk kept: the item passed is the last scanned square's.
      planet_set_square_item(session, planet, victim, last_item, SquareMode::kRemove);
      if (planet.population <= planet.used_population) {
        return;
      }
    }
    if (!starved && antag_race_ai_planet_specials(session, race, planet)) {
      return;
    }
    const std::int32_t focus = antag_race_ai_planet_focus(session, race, planet);
    struct Candidate {
      std::uint8_t item;
      std::int16_t focus_value;
      std::int16_t total;
    };
    std::vector<Candidate> candidates;
    for (std::uint8_t i = 0; i < PlanItemTable::kItems; ++i) {
      if (i == kItemColonyBase || i >= items.surface_count || !known(session, planet, i)) {
        continue;
      }
      const PlanItem& it = item_of(session, i);
      if (static_cast<std::int32_t>(planet.max_population) - planet.used_population < it.used_population) {
        continue;
      }
      const std::array<std::int8_t, 4> by_focus = {it.industry, it.research, it.prosperity, it.max_population};
      const std::int8_t v = by_focus.at(static_cast<std::size_t>(focus));
      if (v == 0) {
        continue;
      }
      candidates.push_back(
          {i, v, static_cast<std::int16_t>(it.industry + it.prosperity + it.max_population + it.research)});
    }
    if (candidates.empty() && antag_race_ai_rebuild_planet_square(session, race, planet)) {
      return;
    }
    const std::int32_t roll = session.rng.rand() % 100;
    std::int32_t best = 0;
    std::uint8_t chosen = kNoItem;
    for (const Candidate& c : candidates) {
      if (absent(session, planet, c.item)) {
        // The first item the planet lacks ends the walk.
        if (static_cast<std::uint32_t>(item_of(session, c.item).cost) / static_cast<std::uint16_t>(planet.industry) <
            70) {
          chosen = c.item;
        }
        break;
      }
      const std::int32_t v = roll < 66 ? c.focus_value : c.total;
      if (best < v) {
        chosen = c.item;
        best = v;
      }
    }
    if (chosen == kNoItem) {
      return;
    }
    square = planet_auto_place_item(session, planet, chosen, 0);
  } else if (!known(session, planet, kItemTerraforming)) {
    // Only black squares are reachable.
    if (planet.free_surface <= planet.free_black || crowded) {
      antag_race_ai_rebuild_planet_square(session, race, planet);
      return;
    }
    square = planet_auto_place_item(session, planet, kTransportTubes, -1);
  } else {
    const std::uint16_t tubes = planet_find_item(cosmos, planet, kTransportTubes);
    if (tubes != kNoSquare) {
      planet_set_square_item(session, planet, tubes, kTransportTubes, SquareMode::kRemove);
      if (planet_can_place_item(session, planet, kItemTerraforming, tubes)) {
        planet_place_item_if_free(session, planet, kItemTerraforming, tubes, 0);
        return;
      }
      antag_race_ai_rebuild_planet_square(session, race, planet);
      return;
    }
    square = planet_auto_place_item(session, planet, kItemTerraforming, 0);
  }
  if (square != kNoSquare) {
    return;
  }
  antag_race_ai_rebuild_planet_square(session, race, planet);
}

// 0x3e1cc
std::int32_t antag_race_ai_choose_orbital_project(Session& session, Race& race, Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  std::uint32_t research = static_cast<std::uint16_t>(planet.research);
  if (antag_race_research_complete(session, race)) {
    research = 1000;
  }
  const std::int32_t shields = planet_orbital_shield_strength(cosmos, planet);
  const std::int32_t weapons = planet_orbital_weapon_strength(cosmos, planet);
  const std::uint8_t best_shield = planet_best_orbital_shield_item(session, planet);
  const std::uint8_t best_weapon = planet_best_orbital_weapon_item(session, planet);
  // peCustom: under a lingering danger the planetary defences come first.
  if (session.game->variant == Variant::kPeCustom && pe_danger_at(session, race, planet) &&
      pe_build_orbital_defence(session, race, planet)) {
    return 1;
  }
  if (planet_count_buildings(cosmos, planet) < 5) {
    return 0;
  }
  const std::uint32_t industry = static_cast<std::uint16_t>(planet.industry);
  const bool full = planet.free_orbital < 2 || (room(planet) < 1 && idle(planet) < 2) || u16(planet.prosperity) < 2;
  std::uint8_t c = kNoItem;
  if (shields < 1 && best_shield != kNoItem) {
    c = best_shield;
  }
  if (c == kNoItem && weapons < 1 && best_weapon != kNoItem) {
    c = best_weapon;
  }
  if (c == kNoItem && absent(session, planet, kItemShipyard) && planet.size > 1 &&
      (industry > 3 || static_cast<std::int32_t>(planet.free_surface) - planet.free_black < 5)) {
    if (known(session, planet, kItemShipyard)) {
      c = kItemShipyard;
    }
  } else if (industry < 4 && planet_count_buildable_squares(session, planet) > 0) {
    return 0;
  }
  if ((industry < 20 || research < 20) && planet_count_buildable_squares(session, planet) > 0 &&
      (shields > 0 || best_shield == kNoItem) && (weapons > 0 || best_weapon == kNoItem)) {
    if (static_cast<std::int32_t>(std::min(industry, research)) < shields + weapons) {
      return 0;
    }
  }
  const std::int32_t home = is_home(race, planet) ? 6 : 0;
  if (c == kNoItem && weapons < shields && best_weapon != kNoItem && weapons < home + 6) {
    c = best_weapon;
  } else if (c == kNoItem && best_shield != kNoItem && shields < home + 6) {
    c = best_shield;
  }
  const std::int32_t roll = session.rng.rand() % 100;
  if (c == kNoItem && roll < 20 && planet_can_place_item(session, planet, kOrbitalCloaker, kNoSquare) &&
      absent(session, planet, kOrbitalCloaker)) {
    c = kOrbitalCloaker;
  }
  if (c == kNoItem && weapons < shields && best_weapon != kNoItem) {
    c = best_weapon;
  } else if (c == kNoItem && best_shield != kNoItem) {
    c = best_shield;
  }
  if (c == kNoItem) {
    return 0;
  }
  if (!full && planet_auto_place_item(session, planet, c, 0) != kNoSquare) {
    return 1;
  }
  if (antag_race_ai_replace_orbital_defence(session, race, planet, c)) {
    return 1;
  }
  if (c == best_shield && best_weapon != kNoItem) {
    if (antag_race_ai_replace_orbital_defence(session, race, planet, best_weapon)) {
      return 1;
    }
  } else if (c == best_weapon && best_shield != kNoItem &&
             antag_race_ai_replace_orbital_defence(session, race, planet, best_shield)) {
    return 1;
  }
  return 0;
}

bool pe_build_orbital_defence(Session& session, Race& race, Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  const std::int32_t shields = planet_orbital_shield_strength(cosmos, planet);
  const std::int32_t weapons = planet_orbital_weapon_strength(cosmos, planet);
  const std::uint8_t best_shield = planet_best_orbital_shield_item(session, planet);
  const std::uint8_t best_weapon = planet_best_orbital_weapon_item(session, planet);
  const std::uint8_t item =
      best_weapon != kNoItem && (weapons <= shields || best_shield == kNoItem) ? best_weapon : best_shield;
  return item != kNoItem && (planet_auto_place_item(session, planet, item, 0) != kNoSquare ||
                             antag_race_ai_replace_orbital_defence(session, race, planet, item));
}

bool pe_danger_at(const Session& session, const Race& race, const Planet& planet) {
  return race_danger_lingers_at(session, race, session.game->cosmos.stars[static_cast<std::size_t>(planet.star)],
                                kDangerDays);
}

// 0x3e020
bool antag_race_ai_replace_orbital_defence(Session& session, Race&, Planet& planet, std::uint8_t item) {
  const Cosmos& cosmos = session.game->cosmos;
  if (item >= 31 || item <= 21 || item == kItemShip) {
    return false;
  }
  const std::int32_t weapons = planet_orbital_weapon_strength(cosmos, planet);
  const std::int32_t shields = planet_orbital_shield_strength(cosmos, planet);
  const bool is_shield = item == kOrbitalShield || item == kOrbitalMegaShield;
  const bool is_weapon = item == kItemMissileBase || item == kItemWhopper || item == kItemMegaWhopper;
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  std::int32_t best = 0;
  std::uint16_t victim = kNoSquare;
  for (std::uint16_t i = planet.surface_squares; i < planet.square_count; ++i) {
    const std::uint8_t there = squares[i].item;
    std::int32_t cost = -1;
    if (there == kOrbitalShield && item != kOrbitalShield) {
      if (!is_weapon || weapons < shields - 3) {
        cost = shields < weapons ? 6 : 3;
      }
    } else if (there == kItemMissileBase && item != kItemMissileBase) {
      if (!is_shield || shields < weapons - 2) {
        cost = weapons < shields ? 4 : 2;
      }
    } else if (there == kItemWhopper && item != kItemWhopper) {
      if (!is_shield || shields < weapons - 4) {
        cost = weapons < shields ? 8 : 4;
      }
    }
    if (cost != -1 && (victim == kNoSquare || cost < best)) {
      best = cost;
      victim = i;
    }
  }
  if (victim == kNoSquare) {
    return false;
  }
  if (!planet_set_square_item(session, planet, victim, squares[victim].item, SquareMode::kRemove)) {
    return false;
  }
  planet_place_item_if_free(session, planet, item, victim, 0);
  return true;
}

// 0x3e520
bool antag_race_ai_planet_specials(Session& session, Race& race, Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  if ((planet.flags & kPlanetRuins) != 0 && planet_auto_place_item(session, planet, kItemXenoDig, 0) != kNoSquare) {
    planet.flags = static_cast<std::uint8_t>(planet.flags & ~kPlanetRuins);
    return true;
  }
  const bool complete = antag_race_research_complete(session, race);
  const std::int32_t min_research = complete ? 0 : 3;
  const std::int32_t industry = u16(planet.industry);
  const std::int32_t research = u16(planet.research);
  if (industry <= 2 || research < min_research || u16(planet.prosperity) <= 1 ||
      (room(planet) <= 0 && idle(planet) <= 1) ||
      static_cast<std::int32_t>(planet.free_surface) - planet.free_black <= 2) {
    return false;
  }
  // Surface shields up to a fifth of the surface (two fifths away from home).
  const std::int32_t shield = planet_surface_shield_strength(cosmos, planet);
  std::uint32_t cap = (is_home(race, planet) ? planet.surface_squares : planet.surface_squares * 2u) / 5;
  if (cap < 4) {
    cap = 4;
  }
  if ((industry < 20 || research < 20) && shield > 0 && std::min(industry, research) * 2 < shield * 3) {
    cap = static_cast<std::uint32_t>(shield);
  }
  if (shield < static_cast<std::int32_t>(cap)) {
    std::uint8_t item = kNoItem;
    if (planet_can_place_item(session, planet, kSurfaceMegaShield, kNoSquare)) {
      item = kSurfaceMegaShield;
    } else if (planet_can_place_item(session, planet, kSurfaceShield, kNoSquare)) {
      item = kSurfaceShield;
    }
    if (item != kNoItem && placed(session, planet_auto_place_item(session, planet, item, 0))) {
      return true;
    }
  }
  if (planet.owner % 2 != 0 && industry > 20 && session.rng.rand() % 100 < 4 &&
      planet_can_place_item(session, planet, kSurfaceCloaker, kNoSquare) && absent(session, planet, kSurfaceCloaker) &&
      placed(session, planet_auto_place_item(session, planet, kSurfaceCloaker, 0))) {
    return true;
  }
  for (const std::uint8_t item : kSpecials) {
    if ((item != kLushGrowthBomb || (planet.flags & 1) == 0) &&
        planet_can_place_item(session, planet, item, kNoSquare) &&
        (antag_race_research_complete(session, race) || item != kItemInternet) && absent(session, planet, item) &&
        placed(session, planet_auto_place_item(session, planet, item, 0))) {
      return true;
    }
  }
  return false;
}

// 0x3e800
std::int32_t antag_race_ai_planet_focus(Session& session, Race& race, const Planet& planet) {
  FreeSquaresByColour free = planet_count_free_squares_by_colour(session, planet);
  const bool complete = antag_race_research_complete(session, race);
  if (complete) {
    free.industry += free.research;
    free.prosperity += free.research;
    free.research = 0;
  }
  const std::int32_t industry = u16(planet.industry);
  const std::int32_t research = u16(planet.research);
  const std::int32_t prosperity = u16(planet.prosperity);
  if (room(planet) <= 1 && idle(planet) <= 1) {
    return 3;
  }
  if (industry < 3 && free.industry > 0) {
    return 0;
  }
  if (research < 3 && free.research > 0 && !complete) {
    return 1;
  }
  if (prosperity < 2 && free.prosperity > 0) {
    return 2;
  }
  if (industry < 4 && free.industry > 0) {
    return 0;
  }
  if (prosperity < 3 && free.prosperity > 0) {
    return 2;
  }
  if (research < 5 && free.research > 0 && !complete) {
    return 1;
  }
  if (prosperity == 0 || (prosperity < 2 && room(planet) < 1 && idle(planet) < 1)) {
    return 2;
  }
  if (free.prosperity > 0 && session.tables->planitems.pop_growth / prosperity > 15 && room(planet) > 3 &&
      idle(planet) != 1) {
    return 2;
  }
  if (complete) {
    if (free.industry > 0) {
      return 0;
    }
    if (free.prosperity > 0 && prosperity < 5) {
      return 2;
    }
    return 3;
  }
  std::uint32_t i = static_cast<std::uint32_t>(industry);
  std::uint32_t r = static_cast<std::uint32_t>(research);
  const Cosmos& cosmos = session.game->cosmos;
  if (planet_find_item(cosmos, planet, kItemHyperpowerPlant) != kNoSquare) {
    r += i >> 1;
  }
  // Original bug (as PATCH), fixed in peCustom: the Internet test compares
  // with 0xfff, so it always holds.
  if (planet_find_item(cosmos, planet, kItemInternet) != (pe_custom(session) ? kNoSquare : 0xfff)) {
    i += r >> 1;
  }
  if ((r < i || free.industry < 1) && free.research > 0) {
    return 1;
  }
  if (free.industry > 0) {
    return 0;
  }
  if (free.prosperity > 0) {
    return 2;
  }
  return 3;
}

// 0x3cce4
bool antag_race_ai_rebuild_planet_square(Session& session, Race& race, Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  // Weights of the outputs (floats) and the planet's most urgent need.
  float w_industry = 1.0f;
  float w_research = 1.0f;
  float w_prosperity = 1.0f;
  float w_housing = 1.0f;
  float w_shield = 1.0f;
  std::uint32_t need = 0;
  bool low_prosperity = false;
  bool habitats = false;
  const std::int32_t shield = planet_surface_shield_strength(cosmos, planet);
  // Both read the Surface Mega Shield's requirement (the second probably
  // meant the Habitat's).
  const bool mega_shield = known(session, planet, kSurfaceMegaShield);
  const bool housing_room = room(planet) > 0;
  const bool complete = antag_race_research_complete(session, race);
  const std::int32_t industry = u16(planet.industry);
  const std::int32_t research = u16(planet.research);
  const std::int32_t prosperity = u16(planet.prosperity);
  if (prosperity < 2) {
    low_prosperity = true;
    w_prosperity = 2.0f;
    need = 4;
  }
  if (planet.free_surface == 0 && room(planet) > 3 && mega_shield) {
    w_housing = 0.5f;
    habitats = true;
  } else if (planet.free_surface > 1 && room(planet) < 2) {
    need = 8;
    if (housing_room) {
      w_housing = 0.0f;
    }
  }
  if (shield < 4) {
    w_shield = 2.0f;
    if (need == 0) {
      need = 0x10;
    }
  }
  if (complete) {
    w_research = 0.0f;
  }
  if (prosperity < idle(planet) && prosperity < 3) {
    w_prosperity = 2.0f;
    if (need == 0) {
      need = 4;
    }
  } else if (idle(planet) < prosperity || prosperity > 5) {
    w_prosperity = 0.5f;
  }
  if (industry < 25) {
    w_industry = industry == 0 ? 25.0f : static_cast<float>(25 / industry);
    if (need == 0) {
      need = 2;
    }
  }
  if (!complete && research < 25) {
    // Original bug, fixed in peCustom: the research weight is written to the
    // industry's.
    (pe_custom(session) ? w_research : w_industry) = research == 0 ? 25.0f : static_cast<float>(25 / research);
    if (need == 0) {
      need = 1;
    }
  }
  if (shield * 5 < planet.surface_squares * 2) {
    w_shield = 1.5f;
    if (need == 0) {
      need = 0x10;
    }
  }
  // The victim: the surface building of the lowest weighted output.
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  float lowest = 0.0f;
  std::int32_t victim = -1;
  for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
    const Square& sq = squares[i];
    const std::uint8_t it = sq.item;
    switch (it) {
      case kNoItem:
      case kMetroplex:
      case kItemColonyBase:
      case kIndustrialMegafacility:
      case kHydroponifer:
      case kResearchCampus:
      case kSurfaceCloaker:
      case kItemHyperpowerPlant:
      case kItemFertilizationPlant:
      case kItemInternet:
      case kCloningPlant:
      case 16:  // Observation Installation
      case kItemTractorBeam:
      case kSurfaceMegaShield:
        continue;
      default:
        break;
    }
    if ((it == kSurfaceShield && !(mega_shield && (need == 0x10 || need == 0))) || (it == kHabitat && !habitats) ||
        (it == kOutpost && !housing_room) || ((it == kLogicFactory || it == kEngineeringRetreat) && !complete)) {
      continue;
    }
    std::int32_t ind = 0;
    std::int32_t res = 0;
    std::int32_t pros = 0;
    std::int32_t housing = 0;
    std::int32_t shields = 0;
    if ((sq.flags & kSquareBuilt) != 0) {
      const PlanItem& p = item_of(session, it);
      if (p.industry != 0) {
        ind = p.industry + (sq.color == 2 ? 1 : 0);
      }
      if (p.prosperity != 0) {
        pros = p.prosperity + (sq.color == 3 ? 1 : 0);
      }
      if (p.research != 0) {
        res = p.research + (sq.color == 4 ? 1 : 0);
      }
      if (it == kSurfaceShield) {
        shields = 2;
      } else if (it == kSurfaceMegaShield) {
        shields = 4;
      }
      housing = p.max_population;
    }
    // Summed as floats in this order (each step stored).
    float score = f32(static_cast<double>(ind) * w_industry);
    score = f32(static_cast<double>(res) * w_research + score);
    score = f32(static_cast<double>(pros) * w_prosperity + score);
    score = f32(static_cast<double>(housing) * w_housing + score);
    score = f32(static_cast<double>(shields) * w_shield + score);
    if ((sq.flags & kSquareAutomated) != 0) {
      score = f32(static_cast<double>(score) + w_prosperity);
    }
    if (victim == -1 || score < lowest) {
      lowest = score;
      victim = i;
    }
  }
  if (victim == -1) {
    return false;
  }
  const std::int32_t roll = session.rng.rand() % 100;
  std::array<bool, PlanItemTable::kItems> knows{};
  for (std::uint8_t i = 0; i < PlanItemTable::kItems; ++i) {
    knows[i] = known(session, planet, i);
  }
  const std::uint8_t old = squares[static_cast<std::size_t>(victim)].item;
  std::uint8_t category = old;
  if ((old == kLaboratory || old == kLogicFactory || old == kEngineeringRetreat) && complete) {
    category = low_prosperity ? kAgriplot : roll < 25 ? kSurfaceShield : kFactory;
  }
  switch (need) {
    case 1:
      category = complete ? kFactory : kLaboratory;
      break;
    case 2:
      category = kFactory;
      break;
    case 4:
      category = kAgriplot;
      break;
    case 8:
      category = kOutpost;
      break;
    case 0x10:
      category = kSurfaceShield;
      break;
    default:
      break;
  }
  const auto missing = [&](std::uint8_t item) { return absent(session, planet, item) && knows[item]; };
  std::uint8_t chosen = kNoItem;
  switch (category) {
    case kFactory:
      if (missing(kItemHyperpowerPlant)) {
        chosen = kItemHyperpowerPlant;
      } else if (missing(kItemTractorBeam)) {
        chosen = kItemTractorBeam;
      } else if (missing(kSurfaceCloaker)) {
        chosen = kSurfaceCloaker;
      } else if (knows[kMetroplex] && knows[kIndustrialMegafacility]) {
        chosen = roll < 50 && !complete ? kMetroplex : kIndustrialMegafacility;
      } else if (knows[kMetroplex] && !complete) {
        chosen = kMetroplex;
      } else if (knows[kIndustrialMegafacility]) {
        chosen = kIndustrialMegafacility;
      } else if (knows[kEngineeringRetreat] && !complete) {
        chosen = kEngineeringRetreat;
      } else if (old != kFactory && knows[kFactory]) {
        chosen = kFactory;
      }
      break;
    case kAgriplot:
      if (missing(kItemFertilizationPlant)) {
        chosen = kItemFertilizationPlant;
      } else if (!low_prosperity && missing(kCloningPlant)) {
        chosen = kCloningPlant;
      } else if (!low_prosperity && missing(kItemTractorBeam)) {
        chosen = kItemTractorBeam;
      } else if (!low_prosperity && missing(kSurfaceCloaker)) {
        chosen = kSurfaceCloaker;
      } else if (knows[kMetroplex] && knows[kHydroponifer]) {
        chosen = roll < 50 && !complete ? kMetroplex : kHydroponifer;
      } else if (knows[kMetroplex] && !complete) {
        chosen = kMetroplex;
      } else if (knows[kHydroponifer]) {
        chosen = kHydroponifer;
      } else if (knows[kLogicFactory] && !complete) {
        chosen = kLogicFactory;
      } else if (old != kAgriplot && knows[kAgriplot]) {
        chosen = kAgriplot;
      }
      break;
    case kLaboratory:
      if (missing(kItemInternet)) {
        chosen = kItemInternet;
      } else if (missing(kItemTractorBeam)) {
        chosen = kItemTractorBeam;
      } else if (missing(kSurfaceCloaker)) {
        chosen = kSurfaceCloaker;
      } else if (knows[kResearchCampus]) {
        chosen = kResearchCampus;
      } else if (knows[kMetroplex]) {
        chosen = kMetroplex;
      } else if (knows[kEngineeringRetreat]) {
        chosen = kEngineeringRetreat;
      } else if (knows[kLogicFactory]) {
        chosen = kLogicFactory;
      } else if (old != kLaboratory && knows[kLaboratory]) {
        chosen = kLaboratory;
      }
      break;
    case kHabitat:
      if (knows[kMetroplex]) {
        chosen = kMetroplex;
      } else if (old != kHabitat && knows[kHabitat]) {
        chosen = kHabitat;
      }
      break;
    case kSurfaceShield:
      if (knows[kSurfaceMegaShield]) {
        chosen = kSurfaceMegaShield;
      } else if (old != kSurfaceShield && knows[kSurfaceShield]) {
        chosen = kSurfaceShield;
      }
      break;
    case kOutpost:
      if (knows[kMetroplex]) {
        chosen = kMetroplex;
      } else if (knows[kHabitat]) {
        chosen = kHabitat;
      } else if (old != kOutpost && knows[kOutpost]) {
        chosen = kOutpost;
      }
      break;
    default:
      return false;
  }
  if (chosen == kNoItem) {
    return false;
  }
  const auto square = static_cast<std::uint16_t>(victim);
  if (!planet_set_square_item(session, planet, square, old, SquareMode::kRemove)) {
    return false;
  }
  planet_place_item_if_free(session, planet, chosen, square, 0);
  return true;
}

// 0x405f4
bool antag_race_ai_demolish_research_buildings(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  if (!antag_race_research_complete(session, race)) {
    return false;
  }
  bool any = false;
  for (std::int16_t i = 0; i < cosmos.planet_count; ++i) {
    Planet& planet = cosmos.planets[static_cast<std::size_t>(i)];
    if (planet.owner == race.index &&
        (planet.owner != session.player_race || planet.self_managed != 0 || session.autoplay) &&
        planet_demolish_research_buildings(session, planet)) {
      any = true;
    }
  }
  return any;
}

// 0x3623c
bool planet_demolish_research_buildings(Session& session, Planet& planet) {
  bool any = false;
  for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
    const Square sq = planet_squares(session.game->cosmos, planet)[i];
    if (sq.item != kNoItem && (sq.flags & kSquareBuilt) != 0 &&
        (sq.item == kLaboratory || sq.item == kResearchCampus) &&
        planet_set_square_item(session, planet, i, sq.item, SquareMode::kRemove)) {
      any = true;
    }
  }
  return any;
}

// 0x358f0
std::int32_t planet_count_buildings(const Cosmos& cosmos, const Planet& planet) {
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  std::int32_t n = 0;
  for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
    if (squares[i].item != kNoItem && squares[i].item != kItemColonyBase) {
      ++n;
    }
  }
  return n;
}

// 0x34a44
FreeSquaresByColour planet_count_free_squares_by_colour(const Session& session, const Planet& planet) {
  const std::span<const Square> squares = planet_squares(session.game->cosmos, planet);
  FreeSquaresByColour n;
  for (std::uint16_t i = 0; i < planet.square_count; ++i) {
    if (squares[i].item != kNoItem || !planet_can_place_item(session, planet, kFactory, i)) {
      continue;
    }
    switch (squares[i].color) {
      case 2:
        ++n.industry;
        break;
      case 3:
        ++n.prosperity;
        break;
      case 4:
        ++n.research;
        break;
      default:
        ++n.research;
        ++n.industry;
        ++n.prosperity;
        break;
    }
  }
  return n;
}

// 0x3407c
std::int32_t planet_count_buildable_squares(const Session& session, const Planet& planet) {
  return count_buildable(session, planet, false);
}

// 0x3420c
std::int32_t planet_count_buildable_black_squares(const Session& session, const Planet& planet) {
  return count_buildable(session, planet, true);
}

// 0x35da4
std::uint8_t planet_best_orbital_shield_item(const Session& session, const Planet& planet) {
  for (const std::uint8_t item : {kOrbitalMegaShield, kOrbitalShield}) {
    if (known(session, planet, item)) {
      return item;
    }
  }
  return kNoItem;
}

// 0x35e24
std::uint8_t planet_best_orbital_weapon_item(const Session& session, const Planet& planet) {
  for (const std::uint8_t item : {kItemMegaWhopper, kItemWhopper, kItemMissileBase}) {
    if (known(session, planet, item)) {
      return item;
    }
  }
  return kNoItem;
}

// 0x35fe0
std::int32_t planet_orbital_shield_strength(const Cosmos& cosmos, const Planet& planet) {
  return strength(cosmos, planet, planet.surface_squares, planet.square_count,
                  std::array<std::pair<std::uint8_t, std::int32_t>, 2>{{{kOrbitalShield, 3}, {kOrbitalMegaShield, 6}}});
}

// 0x36050
std::int32_t planet_orbital_weapon_strength(const Cosmos& cosmos, const Planet& planet) {
  return strength(cosmos, planet, planet.surface_squares, planet.square_count,
                  std::array<std::pair<std::uint8_t, std::int32_t>, 3>{
                      {{kItemMissileBase, 2}, {kItemWhopper, 4}, {kItemMegaWhopper, 6}}});
}

// 0x36158
std::int32_t planet_surface_shield_strength(const Cosmos& cosmos, const Planet& planet) {
  return strength(cosmos, planet, 0, planet.surface_squares,
                  std::array<std::pair<std::uint8_t, std::int32_t>, 2>{{{kSurfaceShield, 2}, {kSurfaceMegaShield, 4}}});
}

// 0x360d8
std::int32_t planet_orbital_defence_rating(const Cosmos& cosmos, const Planet& planet) {
  return strength(cosmos, planet, planet.surface_squares, planet.square_count,
                  std::array<std::pair<std::uint8_t, std::int32_t>, 5>{{{kOrbitalShield, 1},
                                                                        {kOrbitalMegaShield, 2},
                                                                        {kItemMissileBase, 2},
                                                                        {kItemWhopper, 4},
                                                                        {kItemMegaWhopper, 6}}});
}

std::int32_t planet_orbital_defence_count(const Cosmos& cosmos, const Planet& planet) {
  return strength(cosmos, planet, planet.surface_squares, planet.square_count,
                  std::array<std::pair<std::uint8_t, std::int32_t>, 5>{{{kOrbitalShield, 1},
                                                                        {kOrbitalMegaShield, 1},
                                                                        {kItemMissileBase, 1},
                                                                        {kItemWhopper, 1},
                                                                        {kItemMegaWhopper, 1}}});
}

// 0x364b4
std::int32_t planet_ai_target_value(const Session& session, const Planet& planet, RaceIndex race) {
  const Cosmos& cosmos = session.game->cosmos;
  const std::uint8_t owner = planet.owner;
  const bool at_war = relation_byte(cosmos, race, owner) == kWar;
  std::int32_t value = 0;
  if (owner == kNoOwner || (owner != race && at_war)) {
    value = static_cast<std::uint16_t>(planet_habitability(cosmos, planet, cosmos.races[race].ability == 2));
    if ((planet.flags & kPlanetRuins) != 0) {
      value += 40;
    }
    if (value < 71 && !at_war) {
      value = 0;
    } else if (at_war) {
      const std::span<const Square> squares = planet_squares(cosmos, planet);
      std::int32_t costs = 0;
      if (absent(session, planet, kSurfaceCloaker)) {
        for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
          costs += square_cost(session, squares[i].item);
        }
      }
      if (absent(session, planet, kOrbitalCloaker)) {
        // Original bug (0x3663b), fixed in peCustom: the orbital loop counts
        // from `surface_squares` but its pointer restarts at square 0, so it
        // sums the first `square_count - surface_squares` surface squares.
        const std::uint16_t shift = pe_custom(session) ? 0 : planet.surface_squares;
        for (std::uint16_t i = planet.surface_squares; i < planet.square_count; ++i) {
          costs += square_cost(session, squares[i - shift].item);
        }
      }
      std::int32_t worth = u16(planet.industry) + u16(planet.prosperity) + u16(planet.research) + planet.population +
                           room(planet) / 2 + costs;
      if (present(session, planet, kItemShipyard)) {
        worth += 50;
      }
      if (present(session, planet, kOrbitalDocks)) {
        worth += 50;
      }
      value = (value + worth / 10) * 2;
    }
  }
  return value < 0 ? 0 : value;
}

// 0x363b0
std::int32_t planet_ai_own_value(const Session& session, const Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  std::int32_t value =
      static_cast<std::uint16_t>(planet_habitability(cosmos, planet, cosmos.races.at(planet.owner).ability == 2)) +
      u16(planet.industry) + u16(planet.prosperity) + u16(planet.research) + planet.population + room(planet) / 2;
  if ((planet.flags & kPlanetRuins) != 0) {
    value += 100;
  }
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  for (std::uint16_t i = 0; i < planet.square_count; ++i) {
    value += square_cost(session, squares[i].item);
  }
  if (present(session, planet, kItemShipyard)) {
    value += 50;
  }
  if (present(session, planet, kOrbitalDocks)) {
    value += 50;
  }
  return value < 0 ? 0 : value;
}

}  // namespace oa::game
