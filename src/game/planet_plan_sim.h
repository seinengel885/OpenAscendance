// peCustom's planet management, shared by its two planners (`planet_plan.cpp`,
// `planet_plan_v2.cpp`): a planet's copy whose buildings and days can be
// tried out with the game's own rules. Internal to those files.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "game/planet.h"
#include "game/session.h"
#include "game/tables.h"

namespace oa::game::plan_detail {

// Planet items of the planners (`planitem.txt` order).
inline constexpr std::uint8_t kCloningPlant = 15;
inline constexpr std::uint8_t kSurfaceShield = 18;
inline constexpr std::uint8_t kSurfaceMegaShield = 19;
inline constexpr std::uint8_t kOutpost = 20;
inline constexpr std::uint8_t kTransportTubes = 21;

// What one unit of each output is worth for a goal.
struct Weights {
  double industry = 1.0;
  double research = 1.0;
};

// A project: `item` on `square`, after demolishing the building on `victim`
// (if any; it frees a worker and possibly the square itself). No item: wait.
struct Action {
  std::uint8_t item = kNoItem;
  std::uint16_t square = kNoSquare;
  std::uint16_t victim = kNoSquare;
};

inline std::int32_t u16(std::int16_t v) { return static_cast<std::uint16_t>(v); }

// Whether the planet's owner has researched `item`.
inline bool race_knows_item(const Session& session, const Planet& planet, std::uint8_t item) {
  const Research& research = session.game->research;
  const std::uint8_t req = session.tables->planitems.items.at(item).research_requirement;
  return req == 0xff || (req < research.count && ((research.items[req].known_mask >> (planet.owner & 0x1f)) & 1) != 0);
}

// The surface squares' neighbours (up to 4, `kNoSquare` for none).
using Neighbours = std::vector<std::array<std::uint16_t, 4>>;

inline Neighbours surface_neighbours(const PlanItemTable& items, const Planet& planet) {
  Neighbours out(planet.surface_squares);
  for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
    const std::int32_t rc = planet_square_rowcol(items, planet, i);
    const std::int32_t row = (rc >> 16) & 0xffff;
    const std::int32_t column = rc & 0xffff;
    out[i] = {planet_square_at(items, planet, row - 1, column), planet_square_at(items, planet, row + 1, column),
              planet_square_at(items, planet, row, column - 1), planet_square_at(items, planet, row, column + 1)};
  }
  return out;
}

// A copy of a planet and its squares whose days are played forward with
// the game's rules (`planet_daily_update` for the planet's own project and
// growth).
class Sim {
 public:
  Sim(const Session& session, const Planet& real_planet, const Neighbours& neighbours)
      : planet(real_planet), session_(&session), neighbours_(&neighbours) {
    const std::span<const Square> real = planet_squares(session.game->cosmos, real_planet);
    squares.assign(real.begin(), real.end());
    recalc();
  }

  const Neighbours& neighbours() const { return *neighbours_; }
  const PlanItem& item(std::uint8_t i) const { return session_->tables->planitems.items.at(i); }

  std::int32_t free_population() const {
    return static_cast<std::int32_t>(planet.population) - static_cast<std::int32_t>(planet.used_population);
  }
  std::int32_t room() const {
    return static_cast<std::int32_t>(planet.max_population) - static_cast<std::int32_t>(planet.population);
  }
  // The goal's outputs a day. The industry counts its fraction lost to the
  // rounding down: one more Factory often adds less than 1, but always some.
  // The industry a day before it is rounded down (without the AI's boost).
  double exact_industry() const { return planet_output_curve(industry_sum_); }
  double rate(const Weights& w) const {
    const double exact = planet_output_curve(industry_sum_);
    return w.industry * (u16(planet.industry) + (exact - std::floor(exact))) + w.research * u16(planet.research);
  }

  bool built(std::uint16_t square) const {
    return square != kNoSquare && squares[square].item != kNoItem && (squares[square].flags & kSquareBuilt) != 0;
  }
  // An empty surface square (not ruins).
  bool empty(std::uint16_t square) const {
    return square < planet.surface_squares && squares[square].item == kNoItem &&
           (squares[square].color & kSquareRuins) == 0;
  }
  // Whether `square` touches a built square (where a new surface building
  // may go).
  bool touches_built(std::uint16_t square) const {
    return std::ranges::any_of(neighbours()[square], [this](std::uint16_t n) { return built(n); });
  }
  bool can_place(std::uint8_t it, std::uint16_t square) const {
    return planet_can_place_item(*session_, planet, squares, it, square);
  }
  // Whether the building on `square` may be demolished (housing that the
  // population needs may not).
  bool removable(std::uint16_t square) const {
    const Square& sq = squares[square];
    return sq.item != kNoItem && item(sq.item).max_population <= room();
  }

  void recalc() { planet_recalc_output(*session_, planet, squares, &industry_sum_); }

  // The growth needed per inhabitant, and the growth a day: the
  // prosperity, +1 for an AI planet of prosperity under 4
  // (`planet_daily_update`).
  std::int32_t pop_growth() const { return session_->tables->planitems.pop_growth; }
  std::int32_t growth_rate() const {
    const std::int32_t p = u16(planet.prosperity);
    return p + (planet.owner != session_->player_race && p < 4 ? 1 : 0);
  }

  // Starts `a` as the project.
  void start(const Action& a) {
    squares[a.square].item = a.item;
    squares[a.square].flags = 0;
    planet.project_item = a.item;
    planet.project_square = a.square;
    planet.progress = 0;
    recalc();
  }

  // Empties `square` (no recalc).
  void clear(std::uint16_t square) { squares[square] = Square{squares[square].color, kNoItem, 0}; }

  // Applies `a` finished (no item: only the demolition), calls `look` and
  // undoes it all.
  template <typename F>
  auto with(const Action& a, F look) {
    const Planet saved = planet;
    const std::int32_t saved_sum = industry_sum_;
    const std::uint16_t victim = a.victim != kNoSquare ? a.victim : a.square;
    const Square old_victim = squares[victim];
    const Square square = squares[a.square];
    if (a.victim != kNoSquare) {
      clear(a.victim);
    }
    if (a.item == kItemTerraforming) {
      squares[a.square] = Square{1, kNoItem, 0};
    } else if (a.item != kNoItem) {
      squares[a.square].item = a.item;
      squares[a.square].flags = kSquareBuilt;
    }
    recalc();
    auto result = look(*this);
    planet = saved;
    industry_sum_ = saved_sum;
    squares[a.square] = square;
    squares[victim] = old_victim;
    return result;
  }

  // One day of the project and the growth; returns whether the population
  // or the buildings changed.
  bool day() {
    bool changed = false;
    if (planet.project_item != kNoItem) {
      planet.progress = static_cast<std::uint16_t>(planet.progress + planet.industry);
      if (planet.progress >= item(planet.project_item).cost) {
        Square& sq = squares[planet.project_square];
        if (planet.project_item == kItemTerraforming) {
          sq = Square{1, kNoItem, 0};
        } else {
          sq.flags |= kSquareBuilt;
        }
        planet.project_item = kNoItem;
        planet.project_square = kNoSquare;
        planet.progress = 0;
        changed = true;
      }
    }
    bool reset = planet.population >= planet.max_population;
    if (!reset) {
      planet.growth = static_cast<std::uint16_t>(planet.growth + growth_rate());
      if (pop_growth() <= static_cast<std::int32_t>(planet.growth)) {
        planet.population = static_cast<std::uint16_t>(planet.population + 1);
        if (built(find(kCloningPlant))) {
          planet.population = static_cast<std::uint16_t>(planet.population + 1);
        }
        planet.population = std::min(planet.population, planet.max_population);
        reset = true;
        changed = true;
      }
    }
    if (reset) {
      planet.growth = 0;
    }
    if (changed) {
      recalc();
    }
    return changed;
  }

  std::uint16_t find(std::uint8_t it) const {
    for (std::size_t i = 0; i < squares.size(); ++i) {
      if (squares[i].item == it) {
        return static_cast<std::uint16_t>(i);
      }
    }
    return kNoSquare;
  }

  Planet planet;
  std::vector<Square> squares;

 private:
  const Session* session_;
  const Neighbours* neighbours_;
  std::int32_t industry_sum_ = 0;
};

// Days to build `cost` at `industry`.
inline std::int32_t build_days(std::int32_t cost, std::int32_t industry) {
  return industry > 0 ? (cost + industry - 1) / industry : 100000;
}

}  // namespace oa::game::plan_detail
