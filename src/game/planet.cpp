#include "game/planet.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "core/fixed_width.h"
#include "game/address.h"
#include "game/cosmos.h"
#include "game/float_math.h"
#include "game/mood_log.h"
#include "game/planet_plan.h"
#include "game/race.h"
#include "game/research.h"
#include "game/ship.h"

namespace oa::game {

namespace {

const PlanItem& item_of(const GameTables& tables, std::uint8_t item) { return tables.planitems.items.at(item); }

// Planet item ids met only here (`planitem.txt` order).
constexpr std::uint8_t kItemLaboratory = 2;
constexpr std::uint8_t kItemCloningPlant = 15;
constexpr std::uint8_t kItemSurfaceShield = 18;
constexpr std::uint8_t kItemSurfaceMegaShield = 19;
constexpr std::uint8_t kItemOrbitalShield = 26;
constexpr std::uint8_t kItemOrbitalMegaShield = 27;
constexpr std::uint8_t kItemAlienHospitality = 31;
constexpr std::uint8_t kItemLushGrowthBomb = 37;

// Gives a weapon item's square its shots for the day in the flags' high
// byte, when the item has shots (the weapon branch of
// `Planet_set_square_item` and `Planet_daily_update`). The original's stat
// locals are uninitialised for a weapon item the table does not know; every
// weapon item of `planitem.txt` is known.
void set_weapon_shots(const GameTables& tables, Square& sq, std::uint8_t item) {
  if ((item_of(tables, item).flags & kItemWeapon) == 0) {
    return;
  }
  PlanetWeaponStats stats;
  planitem_weapon_stats(item, stats);
  if (stats.shots != 0) {
    sq.flags = static_cast<std::uint16_t>((sq.flags & 0xff) | (stats.shots << 8));
  }
}

// Returns `trunc((sum + 1) ** 0.85)` for a non-zero `sum`, else 0: the
// output curve of industry and prosperity (pow 0x73f30, `__CHP`).
std::int32_t curve(std::int32_t sum) { return sum == 0 ? 0 : chop(planet_output_curve(sum)); }

// The first square of `squares` holding `item`, `kNoSquare` if none.
std::uint16_t find_item(std::span<const Square> squares, std::uint8_t item) {
  for (std::size_t i = 0; i < squares.size(); ++i) {
    if (squares[i].item == item) {
      return static_cast<std::uint16_t>(i);
    }
  }
  return kNoSquare;
}

bool square_built(std::span<const Square> squares, std::uint16_t square) {
  return square != kNoSquare && (squares[square].flags & kSquareBuilt) != 0;
}

// The Antagonizer's square choice (ANTAG 0x347cc): a red, green or blue
// square scores 3 for an item whose matching output (industry, prosperity,
// research) is positive and the largest, 2 when positive but not the
// largest, 0 when not positive; blue scores 1 once the owner knows every
// item; other colour bytes (ruins included) 1. The first best candidate wins.
std::uint16_t pick_scored_square(const Session& session, const Planet& planet, std::uint8_t item,
                                 const std::vector<std::uint16_t>& candidates) {
  const Cosmos& cosmos = session.game->cosmos;
  const Research& research = session.game->research;
  const Race& owner = cosmos.races.at(planet.owner);
  const bool all_known = research.count > 0 &&
                         ((research.items[research.count - 1u].known_mask >> (planet.owner & 0x1f)) & 1) != 0 &&
                         race_count_discoveries(research, owner) == research.count;
  const PlanItem& it = item_of(*session.tables, item);
  const auto score = [](std::int8_t mine, std::int8_t other1, std::int8_t other2) {
    return mine <= 0 ? 0 : (mine < other1 || mine < other2) ? 2 : 3;
  };
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  std::int32_t best = -1;
  std::uint16_t chosen = kNoSquare;
  for (const std::uint16_t square : candidates) {
    std::int32_t s = 1;
    switch (squares[square].color) {
      case 2:
        s = score(it.industry, it.research, it.prosperity);
        break;
      case 3:
        s = score(it.prosperity, it.industry, it.research);
        break;
      case 4:
        if (!all_known) {
          s = score(it.research, it.industry, it.prosperity);
        }
        break;
      default:
        break;
    }
    if (best < s) {
      best = s;
      chosen = square;
    }
  }
  return chosen;
}

}  // namespace

std::span<Square> planet_squares(Cosmos& cosmos, const Planet& planet) {
  return std::span(cosmos.squares).subspan(static_cast<std::size_t>(planet.first_square), planet.square_count);
}

std::span<const Square> planet_squares(const Cosmos& cosmos, const Planet& planet) {
  return std::span(cosmos.squares).subspan(static_cast<std::size_t>(planet.first_square), planet.square_count);
}

std::uint16_t planet_init_surface(Session& session, Planet& planet, std::uint16_t size, std::uint16_t type,
                                  SquareIndex first) {
  const PlanItemTable& items = session.tables->planitems;
  Cosmos& cosmos = session.game->cosmos;
  planet.first_square = first;
  planet.size = size < 5 ? size : 4;
  planet.surface_squares = 0;
  for (const std::uint8_t height : items.column_height[planet.size]) {
    planet.surface_squares = static_cast<std::uint16_t>(planet.surface_squares + height);
  }
  planet.square_count = static_cast<std::uint16_t>(planet.surface_squares + kOrbitalSquares);
  planet.type = type < PlanItemTable::kTypes ? type : 2;
  // Cumulative thresholds of the raw rand() (0..0x7fff): black, red, green, blue.
  const auto& weights = items.type_weights[planet.type];
  const auto share = [](std::uint8_t pct) { return static_cast<std::int32_t>(pct * 0x7fffu / 100); };
  const std::int32_t black = share(weights[0]);
  const std::int32_t red = black + share(weights[1]);
  const std::int32_t green = red + share(weights[2]);
  const std::int32_t blue = green + share(weights[3]);
  std::int32_t habitable = 0;
  const std::span<Square> squares = planet_squares(cosmos, planet);
  for (std::size_t i = 0; i < squares.size(); ++i) {
    Square& sq = squares[i];
    sq.item = kNoItem;
    sq.flags = 0;
    sq.color = 1;
    if (i < planet.surface_squares) {
      const int r = session.rng.rand();
      if (r < black) {
        sq.color = 0;
      } else {
        ++habitable;
        sq.color = r < red ? 2 : r < green ? 3 : r < blue ? 4 : 1;
      }
    }
  }
  planet.flags = 0;
  planet.progress = 0;
  planet.growth = 0;
  planet.used_population = 0;
  planet.prosperity = 0;
  planet.research = 0;
  planet.industry = 0;
  planet.population = 0;
  if (session.rng.rand() % 7 == 0) {
    squares[static_cast<std::size_t>(session.rng.rand() % planet.surface_squares)].color |= kSquareRuins;
    planet.flags |= kPlanetRuins;
  }
  planet.project_square = kNoSquare;
  planet.project_item = kNoItem;
  planet.base_population = static_cast<std::uint16_t>(habitable / 3 + 5);
  planet.max_population = planet.base_population;
  planet.owner = kNoOwner;
  planet_recalc_output(session, planet);
  planet.unknown_5e = 0;
  return planet.square_count;
}

void planet_recalc_output(const Session& session, Planet& planet) {
  planet_recalc_output(session, planet, planet_squares(session.game->cosmos, planet));
}

double planet_output_curve(std::int32_t sum) { return sum == 0 ? 0.0 : std::pow(static_cast<double>(sum + 1), 0.85); }

void planet_recalc_output(const Session& session, Planet& planet, std::span<const Square> squares,
                          std::int32_t* industry_sum) {
  const GameTables& tables = *session.tables;
  const Cosmos& cosmos = session.game->cosmos;
  const bool antagonizer = antagonizer_rules(session.game->variant);
  std::int32_t industry = 0;
  std::int32_t research = 0;
  std::int32_t prosperity = 0;
  std::int32_t used = 0;
  std::uint32_t max_population = planet.base_population;
  std::uint16_t free_surface = 0;
  std::uint16_t free_black = 0;
  std::uint16_t free_orbital = 0;
  std::uint8_t weapons = 0;
  std::uint8_t shields = 0;
  for (std::size_t i = 0; i < squares.size(); ++i) {
    const Square& sq = squares[i];
    if (sq.item == kNoItem) {
      if (i < planet.surface_squares) {
        if ((sq.color & kSquareRuins) == 0) {
          ++free_surface;
          if (sq.color == 0) {
            ++free_black;
          }
        }
      } else {
        ++free_orbital;
      }
      continue;
    }
    const PlanItem& item = item_of(tables, sq.item);
    if (sq.flags & kSquareBuilt) {
      // The colour bonus compares the whole colour byte (not on ruins).
      if (item.industry != 0) {
        industry += item.industry + (sq.color == 2 ? 1 : 0);
      }
      if (item.prosperity != 0) {
        prosperity += item.prosperity + (sq.color == 3 ? 1 : 0);
      }
      if (item.research != 0) {
        research += item.research + (sq.color == 4 ? 1 : 0);
      }
      const bool tractor_shield = antagonizer && sq.item == kItemTractorBeam;
      if ((item.flags & kItemWeapon) && !tractor_shield) {
        ++weapons;
      } else if ((item.flags & kItemShield) || tractor_shield) {
        ++shields;
      }
      max_population += static_cast<std::uint32_t>(static_cast<std::int32_t>(item.max_population));
    }
    if (sq.item != kItemShip && (sq.flags & kSquareAutomated) == 0 && (item.flags & kItemNoPopulation) == 0) {
      used += item.used_population;
    }
  }
  if (square_built(squares, find_item(squares, kItemHyperpowerPlant))) {
    industry += industry >> 1;
  }
  if (square_built(squares, find_item(squares, kItemFertilizationPlant))) {
    prosperity += prosperity >> 1;
  }
  if (square_built(squares, find_item(squares, kItemInternet))) {
    research += research >> 1;
  }
  switch (planet.project_item) {
    case kItemEndlessParty:
      prosperity += industry >> 2;
      break;
    case kItemScientistTakeover:
      research += industry >> 2;
      break;
    case kItemAutomation:
      used += item_of(tables, kItemAutomation).used_population;
      break;
    default:
      break;
  }
  if (industry_sum != nullptr) {
    *industry_sum = industry;
  }
  industry = curve(industry);
  prosperity = curve(prosperity) - (planet.population >> 2);
  if (prosperity < 0) {
    prosperity = 0;
  }
  planet.free_surface = free_surface;
  planet.free_black = free_black;
  planet.free_orbital = free_orbital;
  planet.industry = wrap_i16(industry);
  planet.research = wrap_i16(research);
  if (antagonizer && planet.owner != session.player_race && !session.autoplay &&
      cosmos.atmosphere != Atmosphere::kPeaceful) {
    // The stored words, read back unsigned, times the double 1.3 (0x91db5).
    const auto boost = [](std::int16_t& v) {
      v = wrap_i16(chop(static_cast<double>(static_cast<std::uint16_t>(v)) * 1.3));
    };
    boost(planet.industry);
    if (cosmos.atmosphere == Atmosphere::kHostile) {
      boost(planet.research);
    }
  }
  planet.prosperity = wrap_i16(prosperity);
  planet.max_population = wrap_u16(max_population);
  planet.used_population = wrap_u16(used);
  planet.weapon_count = weapons;
  planet.shield_count = shields;
  if (planet.max_population > 100) {
    planet.max_population = 100;
  }
  if (planet.max_population < planet.population) {
    planet.population = planet.max_population;
  }
  if (planet.population < planet.used_population) {
    planet.used_population = planet.population;
  }
}

std::uint16_t planet_find_item(const Cosmos& cosmos, const Planet& planet, std::uint8_t item) {
  return find_item(planet_squares(cosmos, planet), item);
}

std::uint16_t planet_square_at(const PlanItemTable& items, const Planet& planet, std::int32_t row,
                               std::int32_t column) {
  if (row < 0 || row >= static_cast<std::int32_t>(PlanItemTable::kColumns) || column < 0) {
    return kNoSquare;
  }
  const auto& heights = items.column_height[planet.size];
  std::int32_t first = 0;
  for (std::int32_t r = 0; r < row; ++r) {
    first += heights[static_cast<std::size_t>(r)];
  }
  const std::int32_t start = items.column_start[planet.size][static_cast<std::size_t>(row)];
  if (start <= column && column < start + heights[static_cast<std::size_t>(row)]) {
    return static_cast<std::uint16_t>(first + column - start);
  }
  return kNoSquare;
}

std::int32_t planet_square_rowcol(const PlanItemTable& items, const Planet& planet, std::uint16_t square) {
  if (square >= planet.surface_squares) {
    return -1;
  }
  std::int32_t column = 0;
  std::int32_t row = 0;
  std::uint16_t end = 0;
  const auto& heights = items.column_height[planet.size];
  const auto& starts = items.column_start[planet.size];
  while (end < planet.surface_squares) {
    column = square + starts[static_cast<std::size_t>(row)] - end;
    end = static_cast<std::uint16_t>(end + heights[static_cast<std::size_t>(row)]);
    if (square < end) {
      break;
    }
    ++row;
  }
  return column + row * 0x10000;
}

bool planet_can_build_ship(const Session& session, const Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  const std::uint16_t yard = planet_find_item(cosmos, planet, kItemShipyard);
  return square_built(planet_squares(cosmos, planet), yard) &&
         race_ship_allowance(cosmos, cosmos.races.at(planet.owner)) > 0;
}

bool planet_can_place_item(const Session& session, const Planet& planet, std::uint8_t item, std::uint16_t square) {
  return planet_can_place_item(session, planet, planet_squares(session.game->cosmos, planet), item, square);
}

bool planet_can_place_item(const Session& session, const Planet& planet, std::span<const Square> squares,
                           std::uint8_t item, std::uint16_t square) {
  const GameTables& tables = *session.tables;
  const Cosmos& cosmos = session.game->cosmos;
  const Research& research = session.game->research;
  if (planet.owner == kNoOwner && item != kItemColonyBase) {
    return false;
  }
  // The item itself: researched by the owner and enough free population (no
  // check for ids past the table).
  bool item_ok = true;
  if (item < PlanItemTable::kItems) {
    item_ok = false;
    const PlanItem& it = item_of(tables, item);
    const std::uint8_t req = it.research_requirement;
    if (req == 0xff || (req < research.count && ((research.items[req].known_mask >> (planet.owner & 0x1f)) & 1))) {
      std::uint32_t free = static_cast<std::uint32_t>(planet.population) - planet.used_population;
      if (planet.project_item != kNoItem) {
        free +=
            static_cast<std::uint16_t>(static_cast<std::int16_t>(item_of(tables, planet.project_item).used_population));
      }
      if (it.used_population <= static_cast<std::int32_t>(free & 0xffff)) {
        item_ok = true;
      }
      if (item_ok && item == kItemShip) {
        item_ok = planet_can_build_ship(session, planet);
      }
    }
    if (item == kItemColonyBase) {
      item_ok = true;
    }
  }
  // The square (no check past the planet's squares).
  bool square_ok = true;
  if (square < planet.square_count) {
    square_ok = false;
    const Square& sq = squares[square];
    const std::uint16_t flags = item < PlanItemTable::kItems ? item_of(tables, item).flags : 0;
    if (item < PlanItemTable::kItems && ((flags & kItemAnySquare) || cosmos.races.at(planet.owner).ability == 2)) {
      square_ok = true;
    }
    if (sq.color != 0 || item == kNoItem || (flags & kItemWholePlanet)) {
      square_ok = true;
    }
    if (item != kNoItem && item < 31 && (square < planet.surface_squares) != (item < tables.planitems.surface_count)) {
      square_ok = false;
    }
    if (item != kNoItem && ((sq.color & kSquareRuins) != 0) != (item == kItemXenoDig)) {
      square_ok = false;
    }
    if (item == kItemTerraforming && (sq.color != 0 || sq.item != kNoItem)) {
      square_ok = false;
    }
    if (item == kItemAutomation &&
        ((sq.flags & kSquareBuilt) == 0 || sq.item == kItemShip || (sq.flags & kSquareAutomated) != 0)) {
      square_ok = false;
    }
    // A new surface building must touch a built square.
    if (square < planet.surface_squares && (sq.flags & kSquareBuilt) == 0 && item != kItemColonyBase &&
        item != kItemAutomation) {
      const std::int32_t rc = planet_square_rowcol(tables.planitems, planet, square);
      const std::int32_t column = rc & 0xffff;
      const std::int32_t row = (rc >> 16) & 0xffff;
      const auto built = [&](std::int32_t r, std::int32_t c) {
        return square_built(squares, planet_square_at(tables.planitems, planet, r, c));
      };
      if (!built(row - 1, column) && !built(row + 1, column) && !built(row, column - 1) && !built(row, column + 1)) {
        square_ok = false;
      }
    }
  }
  return item_ok && square_ok;
}

bool planet_build_item_now(Session& session, Planet& planet, std::uint16_t square, std::uint8_t item) {
  return planet_set_square_item(session, planet, square, item, SquareMode::kPlaceBuilt);
}

std::uint16_t planet_pick_random_free_square(Session& session, const Planet& planet, std::uint8_t item) {
  const std::span<const Square> squares = planet_squares(std::as_const(session.game->cosmos), planet);
  std::vector<std::uint16_t> candidates;
  for (std::uint16_t i = 0; i < planet.square_count && candidates.size() < 100; ++i) {
    if (squares[i].item == kNoItem && planet_can_place_item(session, planet, item, i)) {
      candidates.push_back(i);
    }
  }
  if (candidates.empty()) {
    return kNoSquare;
  }
  if (!antagonizer_rules(session.game->variant)) {
    return candidates[static_cast<std::size_t>(session.rng.rand()) % candidates.size()];
  }
  return pick_scored_square(session, planet, item, candidates);
}

std::uint16_t planet_auto_build_item(Session& session, Planet& planet, std::uint8_t item) {
  return planet_auto_place_item(session, planet, item, -1);
}

bool planet_set_square_item(Session& session, Planet& planet, std::uint16_t square, std::uint8_t item,
                            SquareMode mode) {
  const GameTables& tables = *session.tables;
  Cosmos& cosmos = session.game->cosmos;
  bool done = false;
  const bool whole_planet = item < PlanItemTable::kItems && (item_of(tables, item).flags & kItemWholePlanet) != 0;
  if (square != kNoSquare || (whole_planet && mode == SquareMode::kProject)) {
    const std::span<Square> squares = planet_squares(cosmos, planet);
    // A planet-wide project points at the planet's first square.
    Square& sq = squares[square == kNoSquare ? 0 : square];
    switch (mode) {
      case SquareMode::kPlaceBuilt:
        // Original bug kept: the kind test compares `item >= surface_count`
        // (0 or 1) with `(square < surface_squares) - 1` (0 or -1), so only
        // surface items on surface squares pass.
        if (sq.item == kNoItem && item < PlanItemTable::kItems &&
            item_of(tables, item).used_population <=
                static_cast<std::int32_t>(planet.population) - static_cast<std::int32_t>(planet.used_population) &&
            item < tables.planitems.surface_count && square < planet.surface_squares) {
          sq.item = item;
          sq.flags = kSquareBuilt;
          done = true;
          set_weapon_shots(tables, sq, item);
        }
        break;
      case SquareMode::kRemove:
        if (sq.item != kNoItem &&
            (item_of(tables, sq.item).max_population <=
                 static_cast<std::int32_t>(planet.max_population) - static_cast<std::int32_t>(planet.population) ||
             (sq.flags & kSquareBuilt) == 0)) {
          sq.item = kNoItem;
          sq.flags = 0;
          if (square == planet.project_square) {
            planet.project_square = kNoSquare;
            planet.project_item = kNoItem;
          }
          done = true;
        }
        break;
      case SquareMode::kProject:
      case SquareMode::kForceProject:
        if (mode == SquareMode::kForceProject || whole_planet ||
            (planet_can_place_item(session, planet, item, square) &&
             ((sq.flags & kSquareBuilt) == 0 || (item == kItemAutomation && (sq.flags & kSquareAutomated) == 0)))) {
          // The previous project is cancelled (its progress is kept).
          if (planet.project_square != kNoSquare) {
            Square& old = squares[planet.project_square];
            if (old.item == kItemShip) {
              ship_destroy(
                  session,
                  cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, planet.project_square))]);
            }
            if ((old.flags & kSquareBuilt) == 0) {
              old.item = kNoItem;
            }
          }
          planet.project_square = kNoSquare;
          if (!whole_planet) {
            planet.project_square = square;
            if (item != kItemAutomation) {
              sq.item = item;
            }
          }
          planet.project_item = item;
          set_weapon_shots(tables, sq, item);
          done = true;
        }
        break;
    }
  }
  planet_recalc_output(session, planet);
  return done;
}

void planet_place_item_if_free(Session& session, Planet& planet, std::uint8_t item, std::uint16_t square,
                               std::int32_t mode) {
  if (square != kNoSquare && planet_squares(session.game->cosmos, planet)[square].item == kNoItem) {
    planet_set_square_item(session, planet, square, item, mode == -1 ? SquareMode::kPlaceBuilt : SquareMode::kProject);
  }
}

std::uint16_t planet_auto_place_item(Session& session, Planet& planet, std::uint8_t item, std::int32_t mode) {
  const std::uint16_t square = planet_pick_random_free_square(session, planet, item);
  planet_place_item_if_free(session, planet, item, square, mode);
  return square;
}

namespace {

// The cosmos offset of the shared work buffer (`Session::scratch`).
constexpr std::size_t kScratchCosmosOffset = 0x35e34;

// A completed planet-wide project (Lush Growth Bomb) handles `squares[0xffff]`
// like a built square (0x343f8): 0x3fffc bytes past the planet's squares,
// inside the shared work buffer for most planets (`logo.gif` there in a game
// without modal windows). A "ruins" byte there is cleared, the item byte set
// to empty and a random technology granted; else the flags word gets "built".
// Past the buffer (planets whose squares start beyond index 24923) the
// memory is not modelled.
void planet_wide_project_square(Session& session, const Planet& planet) {
  const std::size_t at = kSquaresOffset + static_cast<std::size_t>(planet.first_square) * kSquareSize +
                         0xffffu * kSquareSize - kScratchCosmosOffset;
  if (at + kSquareSize > session.scratch.size()) {
    return;
  }
  std::uint8_t* sq = &session.scratch[at];
  if ((sq[0] & kSquareRuins) != 0) {
    sq[0] = static_cast<std::uint8_t>(sq[0] & ~kSquareRuins);
    sq[1] = kNoItem;
    research_grant_random(session, planet.owner, planet);
  } else {
    sq[2] = static_cast<std::uint8_t>(sq[2] | kSquareBuilt);
  }
}

}  // namespace

void planet_daily_update(Session& session, Planet& planet) {
  const GameTables& tables = *session.tables;
  Cosmos& cosmos = session.game->cosmos;
  planet.protected_turn = 0;
  if (planet.owner != kNoOwner) {
    const std::span<Square> squares = planet_squares(cosmos, planet);
    for (Square& sq : squares) {
      if (sq.item != kNoItem) {
        set_weapon_shots(tables, sq, sq.item);
      }
    }
    const PlanetIndex index = planet_index(cosmos, planet);
    bool completed = false;
    // Alien Hospitality: every 10 days the races' attitude toward the player
    // rises by a tenth of the industry + 1. Only in peCustom: the original
    // tests it among the non-continuous projects, but it is continuous, so
    // it never acts (original bug).
    if (session.game->variant == Variant::kPeCustom && planet.project_item == kItemAlienHospitality &&
        cosmos.day % 10 == 0) {
      const MoodLogScope log(session, "Gastfreundschaft (Projekt)");
      for (std::int32_t r = 0; r < cosmos.race_count; ++r) {
        std::int16_t& attitude = cosmos.races[static_cast<std::size_t>(r)].attitude[session.player_race];
        attitude = wrap_i16(attitude + planet.industry / 10 + 1);
      }
    }
    if (planet.project_item != kNoItem && (item_of(tables, planet.project_item).flags & kItemContinuous) == 0) {
      // Hostile: +1 production for a weak AI planet (not in the Antagonizer,
      // ANTAG 0x352e0).
      if (!antagonizer_rules(session.game->variant) && planet.owner != session.player_race &&
          cosmos.atmosphere == Atmosphere::kHostile && static_cast<std::uint16_t>(planet.industry) < 4) {
        planet.progress = wrap_u16(planet.progress + 1);
      }
      planet.progress = wrap_u16(planet.progress + planet.industry);
      const std::uint16_t cost =
          wrap_u16(planet_project_cost(session, planet, planet.project_square, planet.project_item));
      if (planet.progress >= cost) {
        const std::uint8_t item = planet.project_item;
        if (planet.owner == session.player_race && item == kItemLaboratory &&
            session.game->research.project[session.player_race] == -1 &&
            research_race_rate(session, session.player_race) == 0) {
          session.post_event(EventCode::kLabWithoutResearch, index);
        }
        // peCustom: a Shipyard is reported also on a managed planet (its
        // ships are the player's to build), as is a project with its bell on.
        const bool bell = pe_take_project_bell(session, planet);
        if (planet.owner == session.player_race && session.screen != 17 && item != kItemShip &&
            (planet.self_managed == 0 || bell ||
             (item == kItemShipyard && session.game->variant == Variant::kPeCustom))) {
          completed = true;
          session.post_event(EventCode::kBuildingComplete, index, item);
        }
        if (planet.project_square == kNoSquare) {
          planet_wide_project_square(session, planet);
        } else {
          Square& sq = squares[planet.project_square];
          if (sq.color & kSquareRuins) {
            sq.color = static_cast<std::uint8_t>(sq.color & ~kSquareRuins);
            sq.item = kNoItem;
            research_grant_random(session, planet.owner, planet);
          } else {
            sq.flags |= kSquareBuilt;
          }
          if (item == kItemShip) {
            Ship& ship =
                cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, planet.project_square))];
            if (ship.location_kind == ShipLocation::kBuilding) {
              planet.population = wrap_u16(planet.population - item_of(tables, kItemShip).used_population);
            }
            ship_launch(session, ship);
          }
          if (item == kItemTerraforming) {
            sq.flags = 0;
            sq.item = kNoItem;
            sq.color = 1;
          }
          if (item == kItemAutomation) {
            sq.flags |= kSquareAutomated;
          }
        }
        if (item == kItemLushGrowthBomb && (planet.flags & 1) == 0) {
          planet.base_population = wrap_u16(planet.base_population + 10);
          planet.flags |= 1;
        }
        if ((item_of(tables, item).flags & kItemShield) && planet.project_square != kNoSquare &&
            planet.project_square >= planet.surface_squares) {
          Square& sq = squares[planet.project_square];
          const std::uint16_t strength = item == kItemOrbitalMegaShield ? 35 : 15;
          sq.flags = static_cast<std::uint16_t>((sq.flags & 0xff) | (strength << 8));
        }
        planet.project_square = kNoSquare;
        planet.project_item = kNoItem;
        planet.progress = 0;
      }
    }
    bool reset_growth = planet.population >= planet.max_population;
    if (!reset_growth) {
      planet.growth = wrap_u16(planet.growth + planet.prosperity);
      if (planet.owner != session.player_race && static_cast<std::uint16_t>(planet.prosperity) < 4) {
        planet.growth = wrap_u16(planet.growth + 1);
      }
      if (tables.planitems.pop_growth <= static_cast<std::int32_t>(planet.growth)) {
        if (planet.owner == session.player_race && session.screen != 17 &&
            planet.population == planet.used_population && !completed && planet.self_managed == 0 &&
            (planet.project_item == kNoItem || (item_of(tables, planet.project_item).flags & kItemContinuous) != 0)) {
          session.post_event(EventCode::kFreePopulation, index);
        }
        planet.population = wrap_u16(planet.population + 1);
        if (square_built(squares, planet_find_item(cosmos, planet, kItemCloningPlant))) {
          planet.population = wrap_u16(planet.population + 1);
        }
        if (planet.max_population < planet.population) {
          planet.population = planet.max_population;
        }
        reset_growth = true;
      }
    }
    if (reset_growth) {
      planet.growth = 0;
    }
    planet_recalc_output(session, planet);
  }
  // The rotation around the star: 4000000 / r^2 degrees a day.
  Vec3 pos = planet.position;
  const float r = f32(length(pos));
  normalize(pos);
  rotate_y(pos, f32(4000000.0 / (static_cast<double>(r) * r)));
  planet.position = {f32(static_cast<double>(pos[0]) * r), f32(static_cast<double>(pos[1]) * r),
                     f32(static_cast<double>(pos[2]) * r)};
}

std::uint32_t planet_project_cost(const Session& session, const Planet& planet, std::uint16_t square,
                                  std::uint8_t item) {
  const Cosmos& cosmos = session.game->cosmos;
  std::uint32_t cost = 0xffff;
  if (square != kNoSquare) {
    item = planet_squares(cosmos, planet)[square].item;
  }
  if (item != kNoItem) {
    if (item == kItemShip) {
      const Ship& ship = cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, square))];
      // The refit cost is the low word of the design cost (the high word is
      // stale register bits the callers mask).
      cost = ship.location_kind == ShipLocation::kRefit ? static_cast<std::uint32_t>(planet.ship_cost) & 0xffff
                                                        : static_cast<std::uint32_t>(ship_industry_cost(session, ship));
    } else {
      cost = item_of(*session.tables, item).cost;
    }
    // Automating costs three times the building. Never true but in peCustom:
    // the item was re-read from the square, where Automation is never
    // written (original bug: automating costs the building's cost).
    const std::uint8_t project =
        session.game->variant == Variant::kPeCustom && square != kNoSquare ? planet.project_item : item;
    if (project == kItemAutomation && square == planet.project_square) {
      cost *= 3;
    }
  }
  return cost;
}

std::int32_t planet_project_eta(const Session& session, const Planet& planet) {
  const std::uint32_t cost = planet_project_cost(session, planet, planet.project_square, planet.project_item);
  return research_days_left(static_cast<std::int32_t>(cost & 0xffff), planet.progress,
                            static_cast<std::uint16_t>(planet.industry));
}

ShipIndex planet_square_ship(const Session& session, const Planet& planet, std::uint16_t square) {
  return static_cast<ShipIndex>((planet_squares(session.game->cosmos, planet)[square].flags >> 8) & 0x7f);
}

void planet_set_square_ship(Session& session, Planet& planet, std::uint16_t square, ShipIndex ship) {
  Square& sq = planet_squares(session.game->cosmos, planet)[square];
  sq.flags = static_cast<std::uint16_t>((sq.flags & 0xff) | (static_cast<std::uint16_t>(ship) << 8));
}

bool planet_dock_ship(Session& session, Planet& planet, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  if (planet.owner != ship.owner && planet_has_orbital_guard(session, planet)) {
    return false;
  }
  const std::span<Square> squares = planet_squares(cosmos, planet);
  for (std::uint16_t i = planet.surface_squares; i < planet.square_count; ++i) {
    if (squares[i].item == kNoItem) {
      ship.location_kind = ShipLocation::kOrbit;
      ship.location = planet_index(cosmos, planet);
      squares[i].item = kItemShip;
      squares[i].flags = kSquareBuilt;
      planet_set_square_ship(session, planet, i, ship_index(cosmos, ship));
      return true;
    }
  }
  return false;
}

bool planet_undock_ship(Session& session, Planet& planet, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  const std::span<Square> squares = planet_squares(cosmos, planet);
  const ShipIndex slot = ship_index(cosmos, ship);
  for (std::uint16_t i = planet.surface_squares; i < planet.square_count; ++i) {
    if (squares[i].flags != 0 && squares[i].item == kItemShip && planet_square_ship(session, planet, i) == slot) {
      squares[i].item = kNoItem;
      squares[i].flags = 0;
      ship.location_kind = ShipLocation::kSystem;
      ship.location = planet.star;
      Vec3 offset{};
      offset[0] = static_cast<float>(session.rng.rand() % 1000 - 500);
      offset[1] = static_cast<float>(session.rng.rand() % 1000 - 500);
      offset[2] = static_cast<float>(session.rng.rand() % 1000 - 500);
      set_length(offset, 250.0f);
      const Vec3 position = {f32(static_cast<double>(planet.position[0]) + offset[0]),
                             f32(static_cast<double>(planet.position[1]) + offset[1]),
                             f32(static_cast<double>(planet.position[2]) + offset[2])};
      ship_set_position(session, ship, position);
      return true;
    }
  }
  return false;
}

bool planet_has_orbital_guard(const Session& session, const Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  for (std::uint16_t i = planet.surface_squares; i < planet.square_count; ++i) {
    const Square& sq = squares[i];
    if (sq.item == kNoItem || (sq.flags & kSquareBuilt) == 0) {
      continue;
    }
    if (sq.item == kItemShip &&
        cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, i))].owner == planet.owner) {
      return true;
    }
    if (sq.item == kItemOrbitalShield || sq.item == kItemOrbitalMegaShield) {
      return true;
    }
  }
  return false;
}

std::int32_t planet_habitability(const Cosmos& cosmos, const Planet& planet, bool count_black) {
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  std::int32_t value = 0;
  for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
    // The whole colour byte: a ruins square counts 2.
    const std::uint8_t color = squares[i].color;
    if (color >= 2) {
      value += 2;
    } else if (color == 1 || count_black) {
      value += 1;
    }
  }
  return value;
}

void planitem_weapon_stats(std::uint8_t item, PlanetWeaponStats& stats) {
  switch (item) {
    case kItemMissileBase:
      stats = {45, 7, 1};
      break;
    case kItemWhopper:
      stats = {45, 7, 3};
      break;
    case kItemMegaWhopper:
      stats = {90, 8, 3};
      break;
    case kItemTractorBeam:
      stats = {200, 0, 2};
      break;
    default:
      break;
  }
}

std::uint32_t planet_fire_weapon(Session& session, Planet& planet, bool apply) {
  Cosmos& cosmos = session.game->cosmos;
  const std::optional<std::int32_t> global = square_at(session, planet.fire_square);
  Square& sq = cosmos.squares[static_cast<std::size_t>(*global)];
  const std::optional<std::int32_t> target = ship_at(session, planet.fire_target);
  Ship* ship = target ? &cosmos.ships[static_cast<std::size_t>(*target)] : nullptr;
  PlanetWeaponStats stats{.range = 0, .damage = 0, .shots = 1};
  planitem_weapon_stats(sq.item, stats);
  bool valid = true;
  if (ship != nullptr) {
    if (ship->location_kind != ShipLocation::kSystem || ship->location != planet.star) {
      valid = false;
    }
    if (stats.range > 0 && static_cast<double>(stats.range << 5) < length(sub(ship->position, planet.position))) {
      valid = false;
    }
  }
  std::int32_t shots_left = 1;
  if (stats.shots != 0) {
    shots_left = sq.flags >> 8;
  }
  std::uint32_t result = valid && shots_left != 0 ? kActionPossible : 0;
  if ((result & kActionPossible) && ship != nullptr) {
    const std::uint8_t owner = planet.owner;
    if (stats.damage != 0) {
      result |= ship_take_damage(session, *ship, apply, stats.damage, owner);
    }
    if (apply) {
      if (sq.item == kItemTractorBeam) {
        // The ship is pulled to 3200 closer, at least 64 from the planet
        // (1280 in the Antagonizer, ANTAG 0x3676c).
        Vec3 d = sub(ship->position, planet.position);
        const double pulled = length(d) + -3200.0;
        const float minimum = antagonizer_rules(session.game->variant) ? 1280.0f : 64.0f;
        const float distance = pulled < minimum ? minimum : f32(pulled);
        set_length(d, distance);
        ship->position = {f32(static_cast<double>(planet.position[0]) + d[0]),
                          f32(static_cast<double>(planet.position[1]) + d[1]),
                          f32(static_cast<double>(planet.position[2]) + d[2])};
      }
      if (stats.shots != 0 && shots_left > 0) {
        sq.flags = static_cast<std::uint16_t>((sq.flags & 0xff) | ((shots_left - 1) << 8));
      }
    }
  }
  planet.fire_result = result;
  return result;
}

void planet_bombard(Session& session, Planet& planet, std::int32_t damage, RaceIndex race) {
  Cosmos& cosmos = session.game->cosmos;
  const GameTables& tables = *session.tables;
  if (planet.protected_turn == -1) {
    return;
  }
  Star& star = cosmos.stars[static_cast<std::size_t>(planet.star)];
  if (planet.owner != kNoOwner) {
    race_offended_by(session, cosmos.races[planet.owner], race, star, AttackKind::kBombard);
  }
  const std::span<Square> squares = planet_squares(cosmos, planet);
  std::vector<std::uint16_t> occupied;
  std::uint16_t shield = kNoSquare;
  const bool antagonizer = antagonizer_rules(session.game->variant);
  for (std::uint16_t i = planet.surface_squares; i < planet.square_count; ++i) {
    if (squares[i].item != kNoItem) {
      occupied.push_back(i);
      // The last shield absorbs; the Antagonizer (ANTAG 0x36a5c) skips one
      // under construction (the project square).
      if ((item_of(tables, squares[i].item).flags & kItemShield) && (!antagonizer || i != planet.project_square)) {
        shield = i;
      }
    }
  }
  if (occupied.empty()) {
    return;
  }
  std::uint16_t hit = shield;
  bool destroy = true;
  if (shield == kNoSquare) {
    hit = occupied[static_cast<std::size_t>(session.rng.rand() % static_cast<std::int32_t>(occupied.size()))];
  } else {
    const std::int32_t strength = (squares[shield].flags >> 8) - damage;
    // A shield under construction (the project square) is destroyed.
    if (strength > 0 && shield != planet.project_square) {
      squares[shield].flags = static_cast<std::uint16_t>((squares[shield].flags & 0xff) | (strength << 8));
      destroy = false;
    }
  }
  if (squares[hit].item == kItemShip) {
    Ship& ship = cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, hit))];
    destroy = (ship_take_damage(session, ship, true, damage * 2, race) & kActionDestroyed) != 0;
  }
  if (destroy) {
    planet_set_square_item(session, planet, hit, kNoItem, SquareMode::kRemove);
    if (hit == planet.project_square) {
      planet.project_square = kNoSquare;
      planet.project_item = kNoItem;
      planet.progress = 0;
    }
  }
  planet_recalc_output(session, planet);
}

bool planet_invade(Session& session, Planet& planet, std::uint16_t ship_square, std::int32_t& used) {
  Cosmos& cosmos = session.game->cosmos;
  Ship& ship = cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, ship_square))];
  const RaceIndex attacker = static_cast<RaceIndex>(ship.owner);
  Race& defender = cosmos.races[planet.owner];
  Star& star = cosmos.stars[static_cast<std::size_t>(planet.star)];
  const bool minions = cosmos.races[attacker].ability == 0;
  const MoodLogScope log(session, "Angriff: Invasion");
  if (race_offended_by(session, defender, attacker, star, AttackKind::kInvasion) &&
      defender.declare_war < defender.attitude[attacker]) {
    defender.attitude[attacker] = wrap_i16(defender.declare_war - 10);
  }
  std::int32_t modules = 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    if (ship.slots[static_cast<std::size_t>(i)].gizmo == kGizmoInvasionModule) {
      ++modules;
    }
  }
  const std::span<Square> squares = planet_squares(cosmos, planet);
  std::int32_t shields = 0;
  for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
    if ((squares[i].flags & kSquareBuilt) == 0) {
      continue;
    }
    if (squares[i].item == kItemSurfaceShield) {
      shields += 1;
    } else if (squares[i].item == kItemSurfaceMegaShield) {
      shields += 2;
    }
  }
  const std::int32_t destroyed = shields < modules ? shields + 1 : modules;
  used = minions ? 1 : destroyed;
  std::int32_t removed = 0;
  for (std::int32_t i = 0; i < ship.slot_count && removed < used; ++i) {
    if (ship.slots[static_cast<std::size_t>(i)].gizmo == kGizmoInvasionModule) {
      ++removed;
      ship_remove_gizmo(session, ship, i);
    }
  }
  ship_recompute_stats(session, ship);
  std::int32_t points = 0;
  for (std::uint16_t i = 0; i < planet.surface_squares && points < destroyed; ++i) {
    if (squares[i].item == kItemSurfaceShield && (squares[i].flags & kSquareBuilt) != 0) {
      ++points;
      planet_set_square_item(session, planet, i, kNoItem, SquareMode::kRemove);
    }
  }
  // A Mega Shield counts 2 and is only removed while the total fits.
  for (std::uint16_t i = 0; i < planet.surface_squares && points < destroyed; ++i) {
    if (squares[i].item == kItemSurfaceMegaShield && (squares[i].flags & kSquareBuilt) != 0) {
      points += 2;
      if (points <= destroyed) {
        planet_set_square_item(session, planet, i, kNoItem, SquareMode::kRemove);
      }
    }
  }
  bool taken = false;
  if (shields < modules || minions) {
    if (planet.owner == session.player_race && session.screen != 16) {
      session.post_event(EventCode::kColonyInvaded, planet_index(cosmos, planet), attacker);
    }
    star_set_planet_owner(session, star, planet, attacker);
    taken = true;
    planet.acquired_day = cosmos.day;
  }
  planet_recalc_output(session, planet);
  return taken;
}

void planet_abandon(Session& session, Planet& planet) {
  Cosmos& cosmos = session.game->cosmos;
  star_set_planet_owner(session, cosmos.stars[static_cast<std::size_t>(planet.star)], planet, kNoOwner);
  for (Square& sq : planet_squares(cosmos, planet)) {
    if (sq.item != kItemShip) {
      sq.item = kNoItem;
      sq.flags = 0;
    }
  }
  planet.used_population = 0;
  planet.population = 0;
  planet.owner = kNoOwner;
  planet_recalc_output(session, planet);
}

}  // namespace oa::game
