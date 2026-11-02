// peCustom's rule-based planet management ("V2" goals): the next project of
// an idle planet is decided by a few rules in a fixed order, each looking
// only at the planet as it is and at the project's direct effects (the
// game's own rules on a copy, `plan_detail::Sim`):
// 1. Housing: while the planet needs more workers than it has room for,
//    housing in time (when the room left fills before housing is built).
// 2. Growth: when workers come slower than the goal's buildings are done
//    (or as slow), or too slow for a Shipyard's ships, the growth building that brings inhabitants soonest; without
//    a worker, the least needed building makes way for one if that is
//    sooner than waiting, its rebuilding counted (always when growth has
//    stopped).
// 3. Access: Transport Tubes toward squares not reachable yet, Terraforming
//    once no usable square is left.
// 4. Industry: while workers come faster than the goal's buildings are
//    done, a factory when it saves more days on the buildings still to come
//    than it takes itself.
// 5. Goal: the building that adds most of the goal's outputs per day of
//    building, on the square of its colour.
// 6. Replacement (no empty square left): the building whose replacement
//    gains the goal most, if the gain makes up for the loss while
//    rebuilding; the prosperity a Shipyard needs (an inhabitant within a
//    ship's building time; two workers stay free for ships), a minimum of
//    industry and the housing in use stay. Each replacement raises the goal's outputs,
//    so buildings never replace each other back and forth.
// Squares: an item goes on the colour of its role (housing white), else
// white, else the colour the goal needs least; among those the square
// nearest to the unreached squares of its colour.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "game/antag_planet.h"
#include "game/planet.h"
#include "game/planet_plan.h"
#include "game/planet_plan_sim.h"
#include "game/ship_design.h"
#include "game/tables.h"

namespace oa::game {

namespace {

using namespace plan_detail;

// The items the rules build: the output and housing buildings, the
// planet-wide boosts and the Outpost (`planitem.txt` order).
constexpr std::array<std::uint8_t, 16> kBuildings{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, kOutpost};

// A day count for "never".
constexpr std::int32_t kNever = 1000000;
// The cost of a typical building (when the goal's next one is not known).
constexpr std::int32_t kTypicalCost = 50;
// A replacement must make up for the outputs lost while rebuilding within
// these days.
constexpr std::int32_t kPayback = 100;
// The industry kept when buildings make way: a Surface Shield within these
// days.
constexpr std::int32_t kShieldDays = 20;
// The cost of a well equipped ship per hull (hull and slots full of
// gizmos): a planet with a Shipyard keeps the growth for one in its time.
constexpr std::array<std::int32_t, 4> kShipCost{320, 670, 1000, 1650};
// The free workers a full planet with a Shipyard keeps: one builds a ship
// (and leaves with it), the other the next one at once, while growth fills
// the room the first left.
constexpr std::int32_t kShipWorkers = 2;

// What a building is put up for (its square's colour follows from it).
enum class Role : std::uint8_t { kHousing, kGrowth, kIndustry, kGoal };

constexpr std::uint8_t kWhite = 1;
constexpr std::uint8_t kRed = 2;
constexpr std::uint8_t kGreen = 3;
constexpr std::uint8_t kBlue = 4;

Weights v2_weights(PlanetGoal goal, bool research_complete) {
  if (research_complete) {
    return {1.0, 0.0};
  }
  switch (goal) {
    case PlanetGoal::kIndustryV2:
      return {1.0, 0.05};
    case PlanetGoal::kResearchV2:
      // Industry only for the planet's building (rule 4) and its minimum.
      return {0.0, 1.0};
    default:
      return {1.0, 1.0};
  }
}

class Rules {
 public:
  Rules(const Session& session, const Planet& planet, PlanetGoal goal, bool research_complete)
      : neighbours_(surface_neighbours(session.tables->planitems, planet)),
        b_(session, planet, neighbours_),
        session_(&session),
        goal_(goal),
        weights_(v2_weights(goal, research_complete)) {
    shipyard_ = b_.find(kItemShipyard) != kNoSquare;
    for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
      if (usable(i)) {
        ++empty_;
        reachable_ += b_.touches_built(i) ? 1 : 0;
      }
    }
  }

  Action decide() {
    for (auto rule : {&Rules::housing, &Rules::growth, &Rules::access, &Rules::productivity, &Rules::goal,
                      &Rules::replacement}) {
      if (const std::optional<Action> a = (this->*rule)()) {
        return *a;
      }
    }
    return {};
  }

 private:
  // An empty square a building may go on (not black, not ruins).
  bool usable(std::uint16_t square) const { return b_.empty(square) && b_.squares[square].color != 0; }

  std::int32_t industry() const { return u16(b_.planet.industry); }
  std::int32_t days(std::uint8_t item) const { return build_days(b_.item(item).cost, industry()); }
  // The goal's outputs a day. Balanced: both, the one lagging behind
  // counting more.
  double value(const Sim& s) const {
    const double i = weights_.industry * s.exact_industry();
    const double r = weights_.research * u16(s.planet.research);
    return goal_ == PlanetGoal::kBalancedV2 ? i + r - 0.5 * std::abs(i - r) : i + r;
  }

  // The workers kept for ships by a planet with a Shipyard (`kShipWorkers`).
  std::int32_t ship_workers() const { return shipyard_ ? kShipWorkers : 0; }
  // The workers the planet wants: one per empty usable square more, and
  // those kept for ships.
  std::int32_t wanted_population() const {
    return std::min(100, static_cast<std::int32_t>(b_.planet.used_population) + empty_ + ship_workers());
  }
  // Free workers for a new building (a Shipyard's planet keeps its ship
  // workers once it is full).
  std::int32_t free_workers() const { return b_.free_population() - (empty_ <= 1 ? ship_workers() : 0); }

  // Days until the next inhabitant, the growth gathered and its rate
  // given (`kNever` without room or growth).
  std::int32_t next_inhabitant(const Sim& s) const {
    const std::int32_t rate = s.growth_rate();
    if (s.room() <= 0 || rate <= 0) {
      return kNever;
    }
    return std::max(0, (s.pop_growth() - static_cast<std::int32_t>(s.planet.growth) + rate - 1) / rate);
  }
  // The prosperity of the buildings before the population's share is taken
  // (`planet_recalc_output`).
  static std::int32_t prosperity_sum(const Sim& s) {
    std::int32_t sum = 0;
    for (std::size_t i = 0; i < s.planet.surface_squares; ++i) {
      const Square& sq = s.squares[i];
      if (sq.item != kNoItem && (sq.flags & kSquareBuilt) != 0 && s.item(sq.item).prosperity != 0) {
        sum += s.item(sq.item).prosperity + (sq.color == kGreen ? 1 : 0);
      }
    }
    return s.built(s.find(kItemFertilizationPlant)) ? sum + (sum >> 1) : sum;
  }
  // The days between inhabitants at the growth rate of now.
  std::int32_t interval(const Sim& s) const {
    const std::int32_t rate = s.growth_rate();
    return rate > 0 ? (s.pop_growth() + rate - 1) / rate : kNever;
  }
  // The days the goal's next building takes (a typical one without a
  // worker for it).
  std::int32_t next_days() {
    if (const std::optional<Action> a = goal()) {
      return days(a->item);
    }
    return build_days(kTypicalCost, industry());
  }
  // The inhabitants born within `days`.
  std::int32_t births(const Sim& s, std::int32_t within) const {
    const std::int32_t per = s.built(s.find(kCloningPlant)) ? 2 : 1;
    return (static_cast<std::int32_t>(s.planet.growth) + s.growth_rate() * within) / s.pop_growth() * per;
  }

  // Days until the next inhabitant if `a` is started now (the growth goes
  // on at the rate of now, after the demolition, while it is built), and
  // the days between inhabitants afterwards.
  std::int32_t growth_days(const Action& a) {
    Sim before = b_;
    if (a.victim != kNoSquare) {
      before.clear(a.victim);
      before.recalc();
    }
    const std::int32_t d = days(a.item);
    const std::int32_t r0 = before.growth_rate();
    const std::int32_t need = before.pop_growth() - static_cast<std::int32_t>(before.planet.growth);
    const std::int32_t r1 = b_.with(a, [](const Sim& s) { return s.growth_rate(); });
    if (r1 <= 0) {
      return kNever;
    }
    const std::int32_t first = r0 > 0 && r0 * d >= need ? (need + r0 - 1) / r0 : d + (need - r0 * d + r1 - 1) / r1;
    return first + (before.pop_growth() + r1 - 1) / r1;
  }

  // The colours an item for `role` goes on, best first.
  std::array<std::uint8_t, 5> colours(std::uint8_t item, Role role) const {
    const PlanItem& it = b_.item(item);
    std::uint8_t own = kWhite;
    switch (role) {
      case Role::kHousing:
        own = kWhite;
        break;
      case Role::kGrowth:
        own = kGreen;
        break;
      case Role::kIndustry:
        own = kRed;
        break;
      case Role::kGoal:
        // The colour of the output the goal values most.
        own = weights_.research * it.research > weights_.industry * it.industry ? kBlue
              : it.industry > 0                                                  ? kRed
              : it.prosperity > 0                                                ? kGreen
                                                                                 : kWhite;
        break;
    }
    // The others: the one whose bonus the goal needs least first (green
    // feeds growth, worth about half a building).
    std::array<std::pair<double, std::uint8_t>, 3> rest{
        {{weights_.industry, kRed}, {0.5, kGreen}, {weights_.research, kBlue}}};
    std::ranges::stable_sort(rest);
    std::array<std::uint8_t, 5> out{own, kWhite, 0, 0, 0};
    std::size_t n = own == kWhite ? 1 : 2;
    for (const auto& [w, c] : rest) {
      if (c != own) {
        out[n++] = c;
      }
    }
    // Black (Colony Base) for housing first: it takes no usable square.
    if (role == Role::kHousing && (it.flags & kItemAnySquare) != 0) {
      std::rotate(out.begin(), out.begin() + 4, out.end());
      out[0] = 0;
    }
    return out;
  }

  // Squares to the nearest unreachable usable square of `colour` (over
  // empty squares), per square.
  const std::vector<std::int32_t>& distances(std::uint8_t colour) {
    std::vector<std::int32_t>& d = distance_[colour];
    if (!d.empty()) {
      return d;
    }
    d.assign(b_.planet.surface_squares, kNever);
    std::vector<std::uint16_t> queue;
    for (std::uint16_t i = 0; i < b_.planet.surface_squares; ++i) {
      if (usable(i) && !b_.touches_built(i) && (b_.squares[i].color & 7u) == colour) {
        d[i] = 0;
        queue.push_back(i);
      }
    }
    for (std::size_t q = 0; q < queue.size(); ++q) {
      for (const std::uint16_t n : neighbours_[queue[q]]) {
        if (n != kNoSquare && d[n] == kNever && b_.empty(n)) {
          d[n] = d[queue[q]] + 1;
          queue.push_back(n);
        }
      }
    }
    return d;
  }

  // Where `item` for `role` goes: the reachable empty square of the best
  // colour, nearest to the unreached squares of that colour.
  std::uint16_t place(std::uint8_t item, Role role) {
    for (const std::uint8_t colour : colours(item, role)) {
      std::uint16_t best = kNoSquare;
      std::int32_t best_d = kNever + 1;
      for (std::uint16_t i = 0; i < b_.planet.surface_squares; ++i) {
        if (b_.empty(i) && (b_.squares[i].color & 7u) == colour && b_.can_place(item, i)) {
          const std::int32_t d = colour == 0 ? 0 : distances(colour)[i];
          if (d < best_d) {
            best_d = d;
            best = i;
          }
        }
      }
      if (best != kNoSquare) {
        return best;
      }
    }
    return kNoSquare;
  }

  // The new buildings for `role` among those `fits`, each on its square.
  template <typename F>
  std::vector<Action> candidates(Role role, F fits) {
    std::vector<Action> out;
    for (const std::uint8_t it : kBuildings) {
      if (!fits(b_.item(it)) || !b_.can_place(it, kNoSquare) || (is_boost(it) && b_.find(it) != kNoSquare)) {
        continue;
      }
      // Outposts take no worker, the others need one left for them.
      if (it != kOutpost && free_workers() < 1) {
        continue;
      }
      if (const std::uint16_t square = place(it, role); square != kNoSquare) {
        out.push_back({it, square});
      }
    }
    return out;
  }

  static bool is_boost(std::uint8_t it) {
    return it == kItemHyperpowerPlant || it == kItemFertilizationPlant || it == kItemInternet || it == kCloningPlant;
  }

  // 1. Housing in time.
  std::optional<Action> housing() {
    if (wanted_population() <= b_.planet.max_population) {
      return std::nullopt;
    }
    std::optional<Action> best;
    double best_score = 0.0;
    for (const Action& a :
         candidates(Role::kHousing, [](const PlanItem& it) { return it.max_population > 0; })) {
      // Due when the room left fills before it is built.
      const std::int32_t d = days(a.item);
      if (b_.room() > births(b_, d)) {
        continue;
      }
      const double score = (b_.item(a.item).max_population + b_.with(a, [&](const Sim& s) { return value(s); }) -
                            value(b_)) /
                           d;
      if (!best || score > best_score) {
        best = a;
        best_score = score;
      }
    }
    return best;
  }

  // 2. Growth before the workers run out.
  std::optional<Action> growth() {
    // Growth for workers still wanted, or for the ships of a Shipyard.
    const bool workers = b_.planet.population < wanted_population() && b_.room() > 0;
    const bool ships = b_.free_population() <= ship_workers() && b_.growth_rate() < ship_growth();
    if (!workers && !ships) {
      return std::nullopt;
    }
    const std::int32_t free = free_workers();
    // Workers: only when they come slower than the buildings are done.
    if (!ships && (free >= 2 || (free == 1 && interval(b_) < next_days()))) {
      return std::nullopt;
    }
    const std::int32_t wait = next_inhabitant(b_);
    const auto grows = [](const PlanItem& it) { return it.prosperity > 0; };
    std::vector<Action> options;
    if (free >= 1) {
      options = candidates(Role::kGrowth, grows);
    }
    if (options.empty()) {
      // No worker or no square: the least needed buildings may make way.
      for (const std::uint16_t victim : victims()) {
        if (grows(b_.item(b_.squares[victim].item))) {
          continue;
        }
        const Square there = b_.squares[victim];
        b_.clear(victim);
        b_.recalc();
        for (Action a : candidates(Role::kGrowth, grows)) {
          a.victim = victim;
          options.push_back(a);
        }
        b_.squares[victim] = there;
        b_.recalc();
        if (!options.empty()) {
          break;
        }
      }
    }
    std::optional<Action> best;
    if (ships || wait == kNever) {
      // Growth too slow for ships or stopped: the most prosperity, even if
      // more buildings must follow (they make way again once not needed).
      std::int32_t most = prosperity_sum(b_);
      for (const Action& a : options) {
        if (const std::int32_t p = b_.with(a, [&](const Sim& s) { return prosperity_sum(s); }); p > most) {
          best = a;
          most = p;
        }
      }
      return best;
    }
    // Else the next inhabitants soonest, a building that makes way counted
    // with its rebuilding.
    std::int32_t best_days = wait + interval(b_);
    for (const Action& a : options) {
      const std::int32_t rebuild = a.victim != kNoSquare ? days(b_.squares[a.victim].item) : 0;
      if (const std::int32_t d = growth_days(a) + rebuild; d < best_days) {
        best = a;
        best_days = d;
      }
    }
    return best;
  }

  // 3. Access to squares not reachable yet, and new squares by
  // Terraforming.
  std::optional<Action> access() {
    if (reachable_ > 0) {
      return std::nullopt;
    }
    if (empty_ > 0) {
      // Transport Tubes on the way to the nearest unreached square.
      std::uint16_t best = kNoSquare;
      std::int32_t best_d = kNever;
      for (const std::uint8_t colour : {kWhite, kRed, kGreen, kBlue}) {
        const std::vector<std::int32_t>& d = distances(colour);
        for (std::uint16_t i = 0; i < b_.planet.surface_squares; ++i) {
          if (d[i] < best_d && b_.empty(i) && b_.can_place(kTransportTubes, i)) {
            best_d = d[i];
            best = i;
          }
        }
      }
      return best == kNoSquare ? std::nullopt : std::optional<Action>(Action{kTransportTubes, best});
    }
    if (free_workers() < 1 || !b_.can_place(kItemTerraforming, kNoSquare)) {
      return std::nullopt;
    }
    for (std::uint16_t i = 0; i < b_.planet.surface_squares; ++i) {
      if (b_.can_place(kItemTerraforming, i)) {
        return Action{kItemTerraforming, i};
      }
    }
    return std::nullopt;
  }

  // The goal's building: the best output gain per day of building.
  std::optional<Action> best_goal_building(std::vector<Action> options) {
    std::optional<Action> best;
    double best_score = 0.0;
    for (const Action& a : options) {
      const double gain = b_.with(a, [&](const Sim& s) { return value(s); }) - value(b_);
      if (const double score = gain / days(a.item); score > best_score + 1e-9) {
        best = a;
        best_score = score;
      }
    }
    return best;
  }

  // 4. Industry while it speeds up the buildings still to come.
  std::optional<Action> productivity() {
    if (goal_ == PlanetGoal::kIndustryV2 || reachable_ == 0) {
      return std::nullopt;
    }
    const std::optional<Action> next = best_goal_building(candidates(Role::kGoal, [](const PlanItem&) { return true; }));
    if (!next) {
      return std::nullopt;
    }
    // Only while industry holds the planet back: workers come faster than
    // its buildings are done.
    if (b_.free_population() < 2 && interval(b_) >= days(next->item)) {
      return std::nullopt;
    }
    const double cost = b_.item(next->item).cost;
    const double now = b_.exact_industry();
    const double remaining = empty_ * cost;
    std::optional<Action> best;
    double best_saved = 0.0;
    for (const Action& a : candidates(Role::kIndustry, [](const PlanItem& it) { return it.industry > 0; })) {
      const double after = b_.with(a, [](const Sim& s) { return s.exact_industry(); });
      if (after <= now) {
        continue;
      }
      // The factory takes a square of the goal's buildings, for good if the
      // goal wants industry, else until it makes way for one.
      const double later = weights_.industry > 0.0 ? remaining - cost : remaining;
      const double saved = remaining / now - (b_.item(a.item).cost / now + later / after);
      if (saved > best_saved) {
        best = a;
        best_saved = saved;
      }
    }
    return best;
  }

  // 5. The goal's buildings.
  std::optional<Action> goal() {
    return best_goal_building(candidates(Role::kGoal, [](const PlanItem&) { return true; }));
  }

  // The prosperity a day the planet needs: its growth while it wants more
  // workers, with a Shipyard an inhabitant within a ship's building time.
  std::int32_t needed_growth() const {
    return b_.planet.population < wanted_population() ? std::numeric_limits<std::int32_t>::max() : ship_growth();
  }
  // The prosperity a day for an inhabitant within a ship's building time
  // (0 without a Shipyard).
  std::int32_t ship_growth() const {
    if (!shipyard_) {
      return 0;
    }
    std::int32_t cost = kShipCost[0];
    for (std::uint8_t hull = 0; hull < kShipCost.size(); ++hull) {
      if (ship_design_hull_known(session_->game->research, b_.planet.owner, hull)) {
        cost = kShipCost[hull];
      }
    }
    const std::int32_t d = build_days(cost, industry());
    return (b_.pop_growth() + d - 1) / d;
  }
  std::int32_t minimum_industry() const {
    return build_days(b_.item(kSurfaceShield).cost, kShieldDays);
  }

  // The buildings that may make way, the one whose loss costs the goal the
  // least first: not the Transport Tubes, not housing in use, not the
  // prosperity needed (`needed_growth`) nor the minimum industry.
  std::vector<std::uint16_t> victims() {
    struct Victim {
      std::uint16_t square;
      double loss;
    };
    std::vector<Victim> all;
    const std::int32_t need = needed_growth();
    const std::int32_t rate = b_.growth_rate();
    const std::int32_t min_industry = std::min(minimum_industry(), industry());
    const double now = value(b_);
    for (std::uint16_t i = 0; i < b_.planet.surface_squares; ++i) {
      const std::uint8_t it = b_.squares[i].item;
      if (!b_.built(i) || std::ranges::find(kBuildings, it) == kBuildings.end() || !b_.removable(i)) {
        continue;
      }
      const auto [loss, ok] = b_.with(Action{kNoItem, i, i}, [&](const Sim& s) {
        const bool keeps = (b_.item(it).prosperity <= 0 || s.growth_rate() >= std::min(need, rate)) &&
                           (b_.item(it).industry <= 0 || u16(s.planet.industry) >= min_industry);
        return std::pair{now - value(s), keeps};
      });
      if (ok) {
        all.push_back({i, loss});
      }
    }
    std::ranges::stable_sort(all, [](const Victim& a, const Victim& b) { return a.loss < b.loss; });
    std::vector<std::uint16_t> out;
    for (const Victim& v : all) {
      out.push_back(v.square);
    }
    return out;
  }

  // 6. Replacement once no empty square is left: the best gain per day of
  // building that pays back.
  std::optional<Action> replacement() {
    if (reachable_ > 0 || b_.planet.project_item != kNoItem) {
      return std::nullopt;
    }
    std::optional<Action> best;
    double best_score = 0.0;
    const double now = value(b_);
    for (const std::uint16_t victim : victims()) {
      const Square there = b_.squares[victim];
      b_.clear(victim);
      b_.recalc();
      const double without = value(b_);
      ++reachable_;
      distance_ = {};
      for (Action a : candidates(Role::kGoal, [&](const PlanItem&) { return true; })) {
        if (a.item == there.item) {
          continue;
        }
        const double gain = b_.with(a, [&](const Sim& s) { return value(s); }) - now;
        const std::int32_t d = days(a.item);
        a.victim = victim;
        if (gain > 0.0 && gain * kPayback > (now - without) * d && gain / d > best_score) {
          best = a;
          best_score = gain / d;
        }
      }
      --reachable_;
      b_.squares[victim] = there;
      b_.recalc();
      distance_ = {};
    }
    return best;
  }

  Neighbours neighbours_;
  Sim b_;
  const Session* session_;
  PlanetGoal goal_;
  Weights weights_;
  bool shipyard_ = false;
  // The empty usable squares, and those reachable now.
  std::int32_t empty_ = 0;
  std::int32_t reachable_ = 0;
  std::array<std::vector<std::int32_t>, 5> distance_{};
};

}  // namespace

bool planet_goal_v2(PlanetGoal goal) {
  return goal == PlanetGoal::kBalancedV2 || goal == PlanetGoal::kIndustryV2 || goal == PlanetGoal::kResearchV2;
}

void pe_manage_planet_v2(Session& session, Planet& planet, PlanetGoal goal) {
  const bool complete = antag_race_research_complete(session, session.game->cosmos.races.at(planet.owner));
  const Action a = Rules(session, planet, goal, complete).decide();
  if (a.item == kNoItem) {
    return;
  }
  if (a.victim != kNoSquare) {
    const std::uint8_t there = planet_squares(session.game->cosmos, planet)[a.victim].item;
    if (!planet_set_square_item(session, planet, a.victim, there, SquareMode::kRemove)) {
      return;
    }
  }
  planet_set_square_item(session, planet, a.square, a.item, SquareMode::kProject);
}

}  // namespace oa::game
