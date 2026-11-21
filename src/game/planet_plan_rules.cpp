// peCustom's rule-based planet management (the strategies `PlanetGoal`, the
// settings of `PlanetManagement`): the next project of an idle planet is decided by a
// few rules in a fixed order, each looking only at the planet as it is and
// at the project's direct effects (the game's own rules on a copy,
// `plan_detail::Sim`):
// 1. Housing: while the planet needs more workers than it has room for and
//    no worker waits for a building, housing in time (when the room left
//    fills before housing is built); without room and worker, the least
//    needed building makes way for it.
// 2. Growth: when workers come slower than the goal's buildings are done
//    (or as slow), or too slow for a Shipyard's ships or the growth kept,
//    the growth building that brings inhabitants soonest; without a worker,
//    the least needed building makes way for one if that is sooner than
//    waiting, its rebuilding counted (always when growth has stopped).
// 3. First: the buildings set to come first (defences up to their counts,
//    Shipyard, Orbital Dock, the dig), making way if need be; a factory
//    before a Shipyard or Orbital Dock that has it done sooner.
// 4. Access: Transport Tubes toward squares not reachable yet, Terraforming
//    once no usable square is left.
// 5. Along: the buildings set to come along with the growth: defences in
//    the share of the planet built, single buildings once half of it is.
// 6. Industry: while workers come faster than the goal's buildings are
//    done, a factory when it saves more days on the buildings still to come
//    than it takes itself.
// 7. Goal: the building that adds most of the goal's outputs per day of
//    building, on the square of its colour.
// 8. Replacement (no empty square left): the building whose replacement
//    gains the goal most, if the gain makes up for the loss while
//    rebuilding; the prosperity needed (a Shipyard's: an inhabitant within a
//    ship's building time, two workers free for ships; the growth kept), the
//    industry and research kept and the housing in use stay. Each
//    replacement raises the goal's outputs, so buildings never replace each
//    other back and forth.
// Orbital squares: one each for the Shipyard, a ship and the Orbital Dock
// unless set to never, the rest for the orbital defence (up to its count);
// the workers for
// those still to come are wanted from growth (so "reserve" keeps them).
// Squares: an item goes on the colour of its role (housing and defences
// white), else white, else the colour the goal needs least; among those
// the square nearest to the unreached squares of its colour.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vector>

#include "game/address.h"
#include "game/antag_planet.h"
#include "game/events.h"
#include "game/planet.h"
#include "game/planet_final.h"
#include "game/planet_plan.h"
#include "game/planet_plan_sim.h"
#include "game/ship_design.h"
#include "game/tables.h"

namespace oa::game {

namespace {

using namespace plan_detail;

// The items the rules build: the output and housing buildings, the
// planet-wide boosts and the Outpost (`planitem.txt` order; not the Colony
// Base, which only founds a colony).
constexpr std::array<std::uint8_t, 15> kBuildings{0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 12, 13, 14, 15, kOutpost};

// A day count for "never".
constexpr std::int32_t kNever = 1000000;
// The cost of a typical building (when the goal's next one is not known).
constexpr std::int32_t kTypicalCost = 50;
// A replacement must make up for the outputs lost while rebuilding within
// these days.
constexpr std::int32_t kPayback = 100;
// The share of the usable squares (at least `kTargetLeadMin`) left empty
// from which the planet builds only what its final layout wants: any other
// building would make way again soon.
constexpr double kTargetLead = 0.2;
constexpr std::int32_t kTargetLeadMin = 2;
// Close to the final layout, growth waits up to these days for the next
// inhabitant before it takes a square the layout wants for more.
constexpr std::int32_t kShortWait = 5;
// The share of the planet built from which single buildings set to come
// along are built.
constexpr double kAlongShare = 0.5;
// The cost of a well equipped ship per hull (hull and slots full of
// gizmos): a planet with a Shipyard keeps the growth for one in its time.
constexpr std::array<std::int32_t, 4> kShipCost{320, 670, 1000, 1650};
// The free workers a full planet with a Shipyard keeps: one builds a ship
// (and leaves with it), the other the next one at once, while growth fills
// the room the first left.
constexpr std::int32_t kShipWorkers = 2;

// What a building is put up for (its square's colour follows from it).
enum class Role : std::uint8_t { kHousing, kGrowth, kIndustry, kGoal, kDefence };

// Planet items of the defences and orbital buildings (`planitem.txt`).
constexpr std::uint8_t kSurfaceCloaker = 11;
constexpr std::uint8_t kObservation = 16;
constexpr std::uint8_t kOrbitalCloaker = 25;
constexpr std::uint8_t kOrbitalShield = 26;
constexpr std::uint8_t kOrbitalMegaShield = 27;

bool orbital_weapon(std::uint8_t it) { return it == kItemMissileBase || it == kItemWhopper || it == kItemMegaWhopper; }
bool orbital_shield(std::uint8_t it) { return it == kOrbitalShield || it == kOrbitalMegaShield; }

constexpr std::uint8_t kBlack = 0;
constexpr std::uint8_t kWhite = 1;
constexpr std::uint8_t kRed = 2;
constexpr std::uint8_t kGreen = 3;
constexpr std::uint8_t kBlue = 4;

Weights goal_weights(PlanetGoal goal, bool research_complete) {
  if (research_complete) {
    return {1.0, 0.0};
  }
  switch (goal) {
    case PlanetGoal::kEarlyIndustry:
    case PlanetGoal::kEarlyResearch:
      // Industry only while it speeds up the planet (rule 4) and for the
      // final build-out.
      return {0.0, 1.0};
    case PlanetGoal::kBalanced:
      break;
  }
  return {1.0, 1.0};
}

class Rules {
 public:
  Rules(const Session& session, const Planet& planet, PlanetGoal goal, const PlanetManagement& management,
        bool research_complete)
      : neighbours_(surface_neighbours(session.tables->planitems, planet)),
        b_(session, planet, neighbours_),
        session_(&session),
        goal_(goal),
        m_(management),
        weights_(goal_weights(goal, research_complete)),
        black_usable_(session.game->cosmos.races.at(planet.owner).ability == 2) {
    shipyard_ = b_.find(kItemShipyard) != kNoSquare;
    std::int32_t built = 0;
    for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
      if (usable(i)) {
        ++empty_;
        reachable_ += b_.touches_built(i) ? 1 : 0;
      } else if (b_.squares[i].item != kNoItem && b_.squares[i].item != kTransportTubes) {
        ++built;
      }
    }
    fill_ = built + empty_ > 0 ? static_cast<double>(built) / (built + empty_) : 1.0;
    lead_ = std::max(kTargetLeadMin, static_cast<std::int32_t>(std::lround(kTargetLead * (built + empty_))));
    // The squares still missing to the final layout.
    const FinalLayout target = planet_final_layout(session, planet, m_);
    std::array<std::int32_t, PlanetManagement::kOutputs> outputs{};
    std::int32_t housing = 0;
    for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
      if (const std::optional<PlanetManagement::Output> o = planet_final_output(b_.squares[i].item)) {
        ++outputs[*o];
      }
      housing += planet_final_housing(b_.squares[i].item) ? 1 : 0;
    }
    for (std::size_t o = 0; o < outputs.size(); ++o) {
      missing_[o] = std::max(0, target.squares[o] - outputs[o]);
    }
    missing_housing_ = std::max(0, target.housing - housing);
  }

  // Whether the planet is close to full (`kTargetLead`): a building the
  // final layout does not want would have to make way again soon.
  bool must_follow_target() const { return empty_ <= lead_; }
  // Whether the final layout wants `item` on one more square.
  bool target_wants(std::uint8_t item) const {
    if (const std::optional<PlanetManagement::Output> o = planet_final_output(item)) {
      return missing_[*o] > 0;
    }
    return planet_final_housing(item) && missing_housing_ > 0;
  }
  // `options` without what the final layout does not want, once it must be
  // followed.
  std::vector<Action> toward_target(std::vector<Action> options) const {
    if (must_follow_target()) {
      std::erase_if(options, [&](const Action& a) { return !target_wants(a.item); });
    }
    return options;
  }

  // Whether no usable square is left empty.
  bool built_up() const { return empty_ == 0; }

  // The project, none to wait.
  Action decide() {
    for (auto rule : {&Rules::housing, &Rules::growth, &Rules::first, &Rules::access, &Rules::along,
                      &Rules::productivity, &Rules::goal, &Rules::replacement}) {
      if (const std::optional<Action> a = (this->*rule)()) {
        return *a;
      }
    }
    return {};
  }

 private:
  // An empty square a building may go on (not ruins; black only for the
  // race that builds there).
  bool usable(std::uint16_t square) const {
    return b_.empty(square) && (b_.squares[square].color != 0 || black_usable_);
  }

  std::int32_t industry() const { return u16(b_.planet.industry); }
  std::int32_t days(std::uint8_t item) const { return build_days(b_.item(item).cost, industry()); }
  // The goal's outputs a day (balanced: both alike, so each square gets the
  // building of its colour).
  double value(const Sim& s) const {
    return weights_.industry * s.exact_industry() + weights_.research * u16(s.planet.research);
  }

  // The workers kept for ships by a planet with a Shipyard (`kShipWorkers`).
  std::int32_t ship_workers() const { return shipyard_ ? kShipWorkers : 0; }
  // The workers the planet wants: one per empty usable square more, and
  // those kept for ships.
  std::int32_t wanted_population() const {
    return std::min(100, static_cast<std::int32_t>(b_.planet.used_population) + empty_ + ship_workers() +
                             orbital_workers());
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
      case Role::kDefence:
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
    // Black gives no bonus either: as good as white for the race that builds
    // there, last for the others (only buildings for any square go there).
    std::array<std::uint8_t, 5> out{};
    std::size_t n = 0;
    out[n++] = own;
    if (own != kWhite) {
      out[n++] = kWhite;
    }
    if (black_usable_) {
      out[n++] = kBlack;
    }
    for (const auto& [w, c] : rest) {
      if (c != own) {
        out[n++] = c;
      }
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
          const std::int32_t d = distances(colour)[i];
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

  using PM = PlanetManagement;

  std::int32_t count(std::initializer_list<std::uint8_t> items) const {
    std::int32_t n = 0;
    for (const Square& sq : b_.squares) {
      n += std::ranges::find(items, sq.item) != items.end() ? 1 : 0;
    }
    return n;
  }
  bool has(std::uint8_t item) const { return b_.find(item) != kNoSquare; }
  bool wanted(PM::Building building) const { return m_.levels[building] != BuildLevel::kNo; }

  // The orbital cloakers, weapons and shields wanted, in that order as far
  // as the orbit has room (`planet_orbital_defence_room`).
  std::array<std::int32_t, 3> orbital_counts() const {
    std::int32_t room = planet_orbital_defence_room(*session_, b_.planet, m_);
    std::array<std::int32_t, 3> out{};
    const std::array<PM::Count, 3> kinds{PM::kOrbitalCloaker, PM::kOrbitalWeapons, PM::kOrbitalShields};
    for (std::size_t k = 0; k < kinds.size(); ++k) {
      out[k] = std::clamp<std::int32_t>(m_.counts[kinds[k]], 0, room);
      room -= out[k];
    }
    return out;
  }
  // The workers for the orbital buildings still to come (not set to never).
  std::int32_t orbital_workers() const {
    std::int32_t n = 0;
    if (wanted(PM::kOrbitalDefence)) {
      const std::array<std::int32_t, 3> wanted_counts = orbital_counts();
      n += std::max(0, wanted_counts[0] + wanted_counts[1] + wanted_counts[2] -
                           count({kItemMissileBase, kItemWhopper, kItemMegaWhopper, kOrbitalShield, kOrbitalMegaShield,
                                  kOrbitalCloaker}));
    }
    n += wanted(PM::kShipyard) && !shipyard_ ? 1 : 0;
    n += wanted(PM::kOrbitalDock) && !has(kOrbitalDocks) ? 1 : 0;
    return n;
  }

  // How many of `n` are due at `level`: all coming first, the share of the
  // planet built coming along.
  std::int32_t due(std::int32_t n, BuildLevel level) const {
    return level == BuildLevel::kFirst ? n : static_cast<std::int32_t>(n * fill_);
  }
  bool single_due(BuildLevel level) const { return level == BuildLevel::kFirst || fill_ >= kAlongShare; }

  // Whether a building due may take another's square or worker: no square
  // left, or no worker and none to come.
  bool may_make_way() const {
    return reachable_ == 0 || (free_workers() < 1 && next_inhabitant(b_) == kNever);
  }

  // `item` (a defence) on the surface: on a square of its own, else where a
  // building makes way.
  std::optional<Action> surface(std::uint8_t item) {
    if (!race_knows_item(*session_, b_.planet, item)) {
      return std::nullopt;
    }
    if (free_workers() >= 1 && reachable_ > 0) {
      if (const std::uint16_t square = place(item, Role::kDefence); square != kNoSquare) {
        return Action{item, square};
      }
    }
    if (!may_make_way()) {
      return std::nullopt;
    }
    for (const std::uint16_t victim : victims()) {
      const bool fits = b_.with(Action{kNoItem, victim, victim}, [&](const Sim& s) { return s.can_place(item, victim); });
      if (fits) {
        return Action{item, victim, victim};
      }
    }
    return std::nullopt;
  }

  // `item` in orbit: on a free orbital square, a building making way for
  // its worker if need be.
  std::optional<Action> orbital(std::uint8_t item) {
    if (item == kNoItem || !race_knows_item(*session_, b_.planet, item)) {
      return std::nullopt;
    }
    std::uint16_t square = kNoSquare;
    for (std::uint16_t i = b_.planet.surface_squares; i < b_.planet.square_count && square == kNoSquare; ++i) {
      square = b_.squares[i].item == kNoItem ? i : square;
    }
    if (square == kNoSquare) {
      return std::nullopt;
    }
    if (free_workers() >= 1 && b_.can_place(item, square)) {
      return Action{item, square};
    }
    if (!may_make_way()) {
      return std::nullopt;
    }
    for (const std::uint16_t victim : victims()) {
      if (b_.with(Action{kNoItem, victim, victim}, [&](const Sim& s) { return s.can_place(item, square); })) {
        return Action{item, square, victim};
      }
    }
    return std::nullopt;
  }

  // The orbit's defences complete: the cheapest one of which a better one
  // of its kind is known makes way for it.
  std::optional<Action> orbital_upgrade() const {
    const std::uint8_t shield = planet_best_orbital_shield_item(*session_, b_.planet);
    const std::uint8_t weapon = planet_best_orbital_weapon_item(*session_, b_.planet);
    std::optional<Action> best;
    for (std::uint16_t i = b_.planet.surface_squares; i < b_.planet.square_count; ++i) {
      const std::uint8_t it = b_.squares[i].item;
      const std::uint8_t better = orbital_weapon(it) ? weapon : orbital_shield(it) ? shield : kNoItem;
      if (better != kNoItem && b_.item(better).cost > b_.item(it).cost &&
          (!best || b_.item(it).cost < b_.item(b_.squares[best->square].item).cost)) {
        best = Action{better, i, i};
      }
    }
    return best;
  }

  // `item` in orbit, or first the industry building that has it done
  // sooner (its own days and those of `item` after it).
  std::optional<Action> sooner(std::uint8_t item) {
    const std::optional<Action> direct = orbital(item);
    if (!direct || direct->victim != kNoSquare) {
      return direct;
    }
    const std::int32_t cost = b_.item(item).cost;
    std::int32_t best_days = days(item);
    std::optional<Action> best = direct;
    for (const Action& a : candidates(Role::kIndustry, [](const PlanItem& it) { return it.industry > 0; })) {
      const std::int32_t after = b_.with(a, [](const Sim& s) { return u16(s.planet.industry); });
      if (const std::int32_t d = days(a.item) + build_days(cost, after); d < best_days) {
        best_days = d;
        best = a;
      }
    }
    return best;
  }

  // The dig: Transport Tubes to the ruins, then the dig itself.
  std::optional<Action> dig() {
    std::uint16_t ruins = kNoSquare;
    for (std::uint16_t i = 0; i < b_.planet.surface_squares; ++i) {
      ruins = (b_.squares[i].color & kSquareRuins) != 0 && b_.squares[i].item == kNoItem ? i : ruins;
    }
    if (ruins == kNoSquare || !race_knows_item(*session_, b_.planet, kItemXenoDig)) {
      return std::nullopt;
    }
    if (b_.touches_built(ruins)) {
      return free_workers() >= 1 && b_.can_place(kItemXenoDig, ruins) ? std::optional<Action>(Action{kItemXenoDig, ruins})
                                                                     : std::nullopt;
    }
    // Breadth first from the ruins over empty squares to the nearest one
    // Transport Tubes may go on now.
    std::vector<std::uint16_t> queue{ruins};
    std::vector<bool> seen(b_.planet.surface_squares, false);
    seen[ruins] = true;
    for (std::size_t q = 0; q < queue.size(); ++q) {
      for (const std::uint16_t n : neighbours_[queue[q]]) {
        if (n == kNoSquare || seen[n] || !b_.empty(n)) {
          continue;
        }
        seen[n] = true;
        if (b_.can_place(kTransportTubes, n)) {
          return Action{kTransportTubes, n};
        }
        queue.push_back(n);
      }
    }
    return std::nullopt;
  }

  // The buildings set to `level` that are due, in this order: planetary
  // defences, orbital defences, Shipyard, Orbital Dock, dig.
  std::optional<Action> due_buildings(BuildLevel level) {
    if (m_.levels[PM::kPlanetaryDefence] == level) {
      const std::uint8_t shield = race_knows_item(*session_, b_.planet, kSurfaceMegaShield) ? kSurfaceMegaShield
                                                                                            : kSurfaceShield;
      const std::array<std::pair<PM::Count, std::uint8_t>, 4> defences{
          {{PM::kSurfaceShields, shield}, {PM::kObservation, kObservation}, {PM::kSurfaceCloaker, kSurfaceCloaker},
           {PM::kTractorBeam, kItemTractorBeam}}};
      for (const auto& [kind, item] : defences) {
        const std::int32_t built = kind == PM::kSurfaceShields ? count({kSurfaceShield, kSurfaceMegaShield}) : count({item});
        if (built < due(m_.counts[kind], level)) {
          if (const std::optional<Action> a = surface(item)) {
            return a;
          }
        }
      }
    }
    if (m_.levels[PM::kOrbitalDefence] == level) {
      // Cloakers, then weapons and shields, the kind with fewer first.
      const auto [cloakers, weapons, shields] = orbital_counts();
      const std::int32_t weapons_built = count({kItemMissileBase, kItemWhopper, kItemMegaWhopper});
      const std::int32_t shields_built = count({kOrbitalShield, kOrbitalMegaShield});
      const std::uint8_t weapon = planet_best_orbital_weapon_item(*session_, b_.planet);
      const std::uint8_t shield = planet_best_orbital_shield_item(*session_, b_.planet);
      std::vector<std::uint8_t> due_items;
      if (count({kOrbitalCloaker}) < due(cloakers, level)) {
        due_items.push_back(kOrbitalCloaker);
      }
      const bool weapon_due = weapons_built < due(weapons, level);
      const bool shield_due = shields_built < due(shields, level);
      if (weapon_due && (!shield_due || weapons_built <= shields_built)) {
        due_items.push_back(weapon);
      }
      if (shield_due) {
        due_items.push_back(shield);
      }
      if (weapon_due) {
        due_items.push_back(weapon);
      }
      for (const std::uint8_t item : due_items) {
        if (const std::optional<Action> a = orbital(item)) {
          return a;
        }
      }
      if (due_items.empty() && weapons + shields > 0 && due(weapons, level) == weapons &&
          due(shields, level) == shields) {
        if (const std::optional<Action> a = orbital_upgrade()) {
          return a;
        }
      }
    }
    if (m_.levels[PM::kShipyard] == level && !shipyard_ && single_due(level)) {
      if (const std::optional<Action> a = sooner(kItemShipyard)) {
        return a;
      }
    }
    if (m_.levels[PM::kOrbitalDock] == level && !has(kOrbitalDocks) && single_due(level)) {
      if (const std::optional<Action> a = sooner(kOrbitalDocks)) {
        return a;
      }
    }
    if (m_.levels[PM::kDig] == level && single_due(level)) {
      return dig();
    }
    return std::nullopt;
  }

  // 3. and 5.: the buildings coming first, those coming along.
  std::optional<Action> first() { return due_buildings(BuildLevel::kFirst); }
  std::optional<Action> along() { return due_buildings(BuildLevel::kAlong); }

  // The buildings for `role` among those `fits` on the squares or with the
  // workers of the least needed building that may make way (not one that
  // fits itself).
  template <typename F>
  std::vector<Action> make_way(Role role, F fits) {
    std::vector<Action> out;
    for (const std::uint16_t victim : victims()) {
      if (fits(b_.item(b_.squares[victim].item))) {
        continue;
      }
      const Square there = b_.squares[victim];
      b_.clear(victim);
      b_.recalc();
      for (Action a : candidates(role, fits)) {
        a.victim = victim;
        out.push_back(a);
      }
      b_.squares[victim] = there;
      b_.recalc();
      if (!out.empty()) {
        break;
      }
    }
    return out;
  }

  // 1. Housing in time.
  std::optional<Action> housing() {
    // Only while the workers do not wait for buildings.
    if (wanted_population() <= b_.planet.max_population || b_.free_population() > 1) {
      return std::nullopt;
    }
    const auto houses = [](const PlanItem& it) { return it.max_population > 0; };
    std::vector<Action> options = toward_target(candidates(Role::kHousing, houses));
    if (options.empty() && free_workers() < 1 && b_.room() <= 0) {
      // No room, no worker: nothing would change; the least needed
      // building makes way.
      options = make_way(Role::kHousing, houses);
    }
    std::optional<Action> best;
    double best_score = 0.0;
    for (const Action& a : options) {
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
    // Growth for workers still wanted, or the growth kept.
    const bool workers = b_.planet.population < wanted_population() && b_.room() > 0;
    const bool kept = b_.growth_rate() < kept_growth();
    if (!workers && !kept) {
      return std::nullopt;
    }
    const std::int32_t free = free_workers();
    // Workers: only when they come slower than the buildings are done.
    if (!kept && (free >= 2 || (free == 1 && interval(b_) < next_days()))) {
      return std::nullopt;
    }
    const std::int32_t wait = next_inhabitant(b_);
    // Close to the final layout a few days' wait rather than a square it
    // wants for more.
    if (!kept && wait <= kShortWait && must_follow_target() && missing_[PlanetManagement::kProsperity] == 0) {
      return std::nullopt;
    }
    const auto grows = [](const PlanItem& it) { return it.prosperity > 0; };
    std::vector<Action> options;
    if (free >= 1) {
      options = candidates(Role::kGrowth, grows);
    }
    if (options.empty()) {
      // No worker or no square: the least needed buildings may make way.
      options = make_way(Role::kGrowth, grows);
    }
    std::optional<Action> best;
    if (kept || wait == kNever) {
      // Growth slower than kept or stopped: the most prosperity, even if
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
      for (const std::uint8_t colour : {kBlack, kWhite, kRed, kGreen, kBlue}) {
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

  // The outputs a day the final layout still wants more of (all alike).
  double wanted_value(const Sim& s) const {
    return (missing_[PlanetManagement::kIndustry] > 0 ? s.exact_industry() : 0.0) +
           (missing_[PlanetManagement::kResearch] > 0 ? u16(s.planet.research) : 0.0) +
           (missing_[PlanetManagement::kProsperity] > 0 ? prosperity_sum(s) : 0.0);
  }
  // The goal's building: the best output gain per day of building (once
  // the final layout must be followed, of the outputs it wants).
  std::optional<Action> best_goal_building(std::vector<Action> options) {
    const bool target = must_follow_target();
    const auto worth = [&](const Sim& s) { return target ? wanted_value(s) : value(s); };
    std::optional<Action> best;
    double best_score = 0.0;
    for (const Action& a : options) {
      const double gain = b_.with(a, worth) - worth(b_);
      if (const double score = gain / days(a.item); score > best_score + 1e-9) {
        best = a;
        best_score = score;
      }
    }
    return best;
  }

  // The day (from today) the `labs`-th of the goal's buildings (`lab_cost`)
  // is done with the planet's industry at `industry`, after a factory
  // `factory_days` long (0: none): each starts once a worker is free for it
  // (the free ones, then births as far as there is room), housing
  // (`house`, its worker not counted) first where the room is full.
  std::int32_t labs_done(std::int32_t factory_days, std::int32_t industry, std::int32_t lab_cost, std::int32_t labs,
                         const std::optional<Action>& house) const {
    const std::int32_t free = b_.free_population();
    std::int32_t room = b_.room();
    std::int32_t day = 0;
    std::int32_t started = 0;
    const auto start = [&](std::int32_t length) {
      ++started;
      while (free + std::min(births(b_, day), room) < started && day < kNever) {
        ++day;
      }
      day = std::min(kNever, day + length);
    };
    if (factory_days > 0) {
      start(factory_days);
    }
    for (std::int32_t i = 0; i < labs; ++i) {
      if (house && free + room <= started) {
        const PlanItem& h = b_.item(house->item);
        day = std::min(kNever, day + build_days(h.cost, industry));
        room += h.max_population;
      }
      start(build_days(lab_cost, industry));
    }
    return day;
  }

  // 4 (early research focus). A factory first only when the goal's
  // buildings for the workers to come (and their housing) are done sooner
  // with it.
  std::optional<Action> sooner_labs(const Action& next) {
    const std::int32_t squares = std::max(1, empty_ - 1);
    const std::int32_t free = b_.free_population();
    // The goal's buildings for the workers there are and those born while
    // they are built.
    std::int32_t labs = std::clamp(free, 1, squares);
    for (std::int32_t more = 0; more < squares; ++more) {
      const std::int32_t workers = std::clamp(free + births(b_, labs * days(next.item)), 1, squares);
      if (workers <= labs) {
        break;
      }
      labs = workers;
    }
    const std::int32_t lab_cost = b_.item(next.item).cost;
    const std::vector<Action> houses = candidates(Role::kHousing, [](const PlanItem& it) { return it.max_population > 0; });
    const std::optional<Action> house = houses.empty() ? std::nullopt : std::optional(houses.front());
    std::optional<Action> best;
    std::int32_t best_day = labs_done(0, industry(), lab_cost, labs, house);
    for (const Action& a : toward_target(candidates(Role::kIndustry, [](const PlanItem& it) { return it.industry > 0; }))) {
      const std::int32_t after = b_.with(a, [](const Sim& s) { return static_cast<std::int32_t>(u16(s.planet.industry)); });
      if (const std::int32_t day = labs_done(days(a.item), after, lab_cost, labs, house); day < best_day) {
        best = a;
        best_day = day;
      }
    }
    return best;
  }

  // 4. Industry while it speeds up the buildings still to come.
  std::optional<Action> productivity() {
    if (reachable_ == 0) {
      return std::nullopt;
    }
    const std::optional<Action> next = best_goal_building(candidates(Role::kGoal, [](const PlanItem&) { return true; }));
    if (!next) {
      return std::nullopt;
    }
    if (goal_ == PlanetGoal::kEarlyResearch) {
      return sooner_labs(*next);
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
    for (const Action& a : toward_target(candidates(Role::kIndustry, [](const PlanItem& it) { return it.industry > 0; }))) {
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

  // 5. The goal's buildings (toward the final layout once it must be
  // followed).
  std::optional<Action> goal() {
    const std::vector<Action> options = toward_target(candidates(Role::kGoal, [](const PlanItem&) { return true; }));
    if (std::optional<Action> best = best_goal_building(options)) {
      return best;
    }
    // The housing the final layout still wants (it adds no output).
    if (must_follow_target() && missing_housing_ > 0) {
      for (const Action& a : toward_target(candidates(Role::kHousing, [](const PlanItem& it) { return it.max_population > 0; }))) {
        return a;
      }
    }
    return std::nullopt;
  }

  // The prosperity a day the planet needs: its growth while it wants more
  // workers, with a Shipyard an inhabitant within a ship's building time.
  std::int32_t needed_growth() const {
    return b_.planet.population < wanted_population() ? std::numeric_limits<std::int32_t>::max() : kept_growth();
  }
  // The prosperity a day kept: a Shipyard's (while no more workers than
  // those for ships are free).
  std::int32_t kept_growth() const { return b_.free_population() <= ship_workers() ? ship_growth() : 0; }
  // The points of `output` its buildings may not go below when making way:
  // all while kept, the final build-out's minimum (`FinalOutput`).
  std::int32_t kept_points(PlanetManagement::Output output, std::int32_t now) const {
    const FinalOutput& f = m_.final_outputs[output];
    return std::min<std::int32_t>(now, f.mode == FinalMode::kKeep ? now : f.mode == FinalMode::kAtLeast ? f.value : 0);
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

  // The buildings that may make way, the one whose loss costs the goal the
  // least first: not the Transport Tubes, not housing in use, not the
  // prosperity needed (`needed_growth`) nor the industry and research kept.
  std::vector<std::uint16_t> victims() {
    struct Victim {
      std::uint16_t square;
      double loss;
    };
    std::vector<Victim> all;
    const std::int32_t need = needed_growth();
    const std::int32_t rate = b_.growth_rate();
    const std::int32_t min_industry = kept_points(PlanetManagement::kIndustry, industry());
    const std::int32_t min_research = kept_points(PlanetManagement::kResearch, u16(b_.planet.research));
    const double now = value(b_);
    for (std::uint16_t i = 0; i < b_.planet.surface_squares; ++i) {
      const std::uint8_t it = b_.squares[i].item;
      if (!b_.built(i) || std::ranges::find(kBuildings, it) == kBuildings.end() || !b_.removable(i)) {
        continue;
      }
      const auto [loss, ok] = b_.with(Action{kNoItem, i, i}, [&](const Sim& s) {
        const bool keeps = (b_.item(it).prosperity <= 0 || s.growth_rate() >= std::min(need, rate)) &&
                           (b_.item(it).industry <= 0 || u16(s.planet.industry) >= min_industry) &&
                           (b_.item(it).research <= 0 || u16(s.planet.research) >= min_research);
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
  PlanetManagement m_;
  Weights weights_;
  // The share of the usable surface built.
  double fill_ = 0.0;
  bool shipyard_ = false;
  // The owner builds on black squares too (Orfa's ability).
  bool black_usable_ = false;
  // The empty usable squares, and those reachable now.
  std::int32_t empty_ = 0;
  std::int32_t reachable_ = 0;
  // The squares the final layout still wants per output, and of housing.
  std::array<std::int32_t, PlanetManagement::kOutputs> missing_{};
  // The empty squares from which the final layout is followed.
  std::int32_t lead_ = kTargetLeadMin;
  std::int32_t missing_housing_ = 0;
  std::array<std::vector<std::int32_t>, 5> distance_{};
};

}  // namespace

void pe_manage_planet_rules(Session& session, Planet& planet, PlanetGoal goal, const PlanetManagement& management) {
  const bool complete = antag_race_research_complete(session, session.game->cosmos.races.at(planet.owner));
  Rules rules(session, planet, goal, management, complete);
  const Action a = rules.decide();
  // The player hears once when the planet is built up (nothing left to
  // build or replace), again after it built anew.
  const auto index = planet_index(session.game->cosmos, planet);
  std::uint8_t& reported = session.game->planet_management.at(static_cast<std::size_t>(index)).reported;
  if (a.item == kNoItem) {
    if (rules.built_up() && reported == 0 && planet.owner == session.player_race && !session.autoplay) {
      reported = 1;
      session.post_event(EventCode::kPlanetBuiltUp, index);
    }
    return;
  }
  reported = 0;
  if (a.victim != kNoSquare) {
    const std::uint8_t there = planet_squares(session.game->cosmos, planet)[a.victim].item;
    if (!planet_set_square_item(session, planet, a.victim, there, SquareMode::kRemove)) {
      return;
    }
  }
  planet_set_square_item(session, planet, a.square, a.item, SquareMode::kProject);
}

}  // namespace oa::game
