#include "game/planet_plan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "game/address.h"
#include "game/antag_planet.h"
#include "game/planet.h"
#include "game/tables.h"

namespace oa::game {

namespace {

// Planet items of this module (`planitem.txt` order).
constexpr std::uint8_t kCloningPlant = 15;
constexpr std::uint8_t kSurfaceShield = 18;
constexpr std::uint8_t kSurfaceMegaShield = 19;
constexpr std::uint8_t kOutpost = 20;
constexpr std::uint8_t kTransportTubes = 21;

// The items the economy planner builds and replaces: the output and housing
// buildings (Factory, Agriplot, Laboratory, Habitat, Metroplex, Industrial
// Megafacility, Artificial Hydroponifer, Research Campus, Logic Factory,
// Engineering Retreat), the planet-wide boosts (Hyperpower, Fertilization
// and Cloning Plants, Internet), the Outpost and the means to reach squares
// (Transport Tubes, Terraforming).
constexpr std::array<std::uint8_t, 17> kEconomyItems{
    0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 12, 13, 14, 15, kOutpost, kTransportTubes, kItemTerraforming};

// The days the planner looks ahead, and the days after them over which the
// outputs reached still count (so that late investments are not worthless).
constexpr std::int32_t kHorizon = 150;
constexpr std::int32_t kTail = 150;
// The best candidates (by the quick estimate) that are played forward.
constexpr std::size_t kRollouts = 8;
// The candidate victims of a replacement: the least useful buildings.
constexpr std::size_t kVictims = 3;
// The average cost of a building, for the buildings industry can build.
constexpr double kAverageCost = 40.0;
// Fewer reachable empty squares than this make opening new ones worth it.
constexpr std::int32_t kFewSquares = 2;
// Defence goal: the industry from which defences come first, and the
// share of the surface that surface shields may take (one in four).
constexpr std::int32_t kDefenceIndustry = 5;
constexpr std::int32_t kSurfaceShieldShare = 4;

std::int32_t u16(std::int16_t v) { return static_cast<std::uint16_t>(v); }

// Whether the planet's owner has researched `item`.
bool race_knows_item(const Session& session, const Planet& planet, std::uint8_t item) {
  const Research& research = session.game->research;
  const std::uint8_t req = session.tables->planitems.items.at(item).research_requirement;
  return req == 0xff || (req < research.count && ((research.items[req].known_mask >> (planet.owner & 0x1f)) & 1) != 0);
}

bool is_player_planet(const Session& session, const Planet& planet) {
  return planet.owner == session.player_race && !session.autoplay;
}

// What one unit of each output is worth for a goal.
struct Weights {
  double industry = 1.0;
  double research = 1.0;
};

Weights goal_weights(PlanetGoal goal, bool research_complete) {
  Weights w;
  switch (goal) {
    case PlanetGoal::kBalanced:
    case PlanetGoal::kDefence:
      // Research grows linearly with its buildings, industry ever slower:
      // half a weight gives both about the same worth per building. (The
      // defences need research too.)
      w = {1.0, 0.5};
      break;
    case PlanetGoal::kResearch:
      w = {0.2, 1.0};
      break;
    default:
      // Industry, and the goals whose projects need industry.
      w = {1.0, 0.05};
      break;
  }
  if (research_complete) {
    w = {1.0, 0.0};
  }
  return w;
}

// A project: `item` on `square`, after demolishing the building there when
// `replace`. No item: wait.
struct Action {
  std::uint8_t item = kNoItem;
  std::uint16_t square = kNoSquare;
  bool replace = false;
};

// The surface squares' neighbours (up to 4, `kNoSquare` for none).
using Neighbours = std::vector<std::array<std::uint16_t, 4>>;

Neighbours surface_neighbours(const PlanItemTable& items, const Planet& planet) {
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
  }

  const Neighbours& neighbours() const { return *neighbours_; }
  const PlanItem& item(std::uint8_t i) const { return session_->tables->planitems.items.at(i); }

  std::int32_t free_population() const {
    return static_cast<std::int32_t>(planet.population) - static_cast<std::int32_t>(planet.used_population);
  }
  std::int32_t room() const {
    return static_cast<std::int32_t>(planet.max_population) - static_cast<std::int32_t>(planet.population);
  }
  double rate(const Weights& w) const { return w.industry * u16(planet.industry) + w.research * u16(planet.research); }

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

  void recalc() { planet_recalc_output(*session_, planet, squares); }

  // Starts `a` as the project.
  void start(const Action& a) {
    squares[a.square].item = a.item;
    squares[a.square].flags = 0;
    planet.project_item = a.item;
    planet.project_square = a.square;
    planet.progress = 0;
    recalc();
  }

  // Applies `a` finished (no item: only the demolition), calls `look` and
  // undoes it all.
  template <typename F>
  auto with(const Action& a, F look) {
    const Planet saved = planet;
    const Square square = squares[a.square];
    if (a.replace) {
      squares[a.square] = Square{square.color, kNoItem, 0};
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
    squares[a.square] = square;
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
      planet.growth = static_cast<std::uint16_t>(planet.growth + planet.prosperity);
      if (planet.owner != session_->player_race && u16(planet.prosperity) < 4) {
        planet.growth = static_cast<std::uint16_t>(planet.growth + 1);
      }
      if (session_->tables->planitems.pop_growth <= static_cast<std::int32_t>(planet.growth)) {
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
};

// Days to build `cost` at `industry`.
std::int32_t build_days(std::int32_t cost, std::int32_t industry) {
  return industry > 0 ? (cost + industry - 1) / industry : 100000;
}

// What the quick estimate reads of a planet.
struct Look {
  double rate = 0.0;
  std::int32_t industry = 0;
  std::int32_t prosperity = 0;
  std::int32_t free = 0;
  std::int32_t room = 0;
  // The empty squares (not black) a building may go on.
  std::int32_t open = 0;
};

// The planner of one planet's economy.
class Planner {
 public:
  // `replace` allows replacing buildings.
  Planner(const Session& session, const Planet& planet, Weights weights, bool replace = true)
      : neighbours_(surface_neighbours(session.tables->planitems, planet)),
        start_(session, planet, neighbours_),
        weights_(weights),
        replace_(replace) {}

  const Sim& start() const { return start_; }

  // A candidate project with its quick estimate.
  struct Scored {
    Action action;
    double score = 0.0;
  };

  // The best project for the planet now (no item: wait).
  Action best() const {
    Sim sim = start_;
    std::vector<Scored> candidates = scored(sim, kHorizon);
    if (candidates.empty()) {
      return {};
    }
    std::ranges::stable_sort(candidates, [](const Scored& a, const Scored& b) { return a.score > b.score; });
    if (candidates.size() > kRollouts) {
      candidates.resize(kRollouts);
    }
    // Waiting is a candidate when nothing new can be built, or nothing
    // looks worth it.
    if (start_.free_population() < 1 || candidates.front().score <= 0.0) {
      candidates.push_back({Action{}, 0.0});
    }
    if (candidates.size() == 1) {
      return candidates.front().action;
    }
    double best_value = -1.0;
    Action chosen;
    for (const Scored& c : candidates) {
      const double v = rollout(c.action);
      if (v > best_value + 1e-9) {
        best_value = v;
        chosen = c.action;
      }
    }
    return chosen;
  }

  std::vector<Scored> scored(Sim& sim, std::int32_t remaining) const {
    std::vector<Scored> out;
    Look now = look(sim, weights_);
    for (std::uint16_t i = 0; i < sim.planet.surface_squares; ++i) {
      if (sim.empty(i) && sim.squares[i].color != 0 && sim.touches_built(i)) {
        ++now.open;
      }
    }
    for (const Action& a : candidates(sim)) {
      out.push_back({a, estimate(sim, a, now, remaining)});
    }
    return out;
  }

 private:
  // The candidates: per item and square colour the reachable empty square
  // that opens the most new squares, and the replacements of the least
  // useful buildings.
  std::vector<Action> candidates(Sim& sim) const {
    std::vector<Action> out;
    const Planet& p = sim.planet;
    if (sim.free_population() >= 1) {
      // The empty squares a building may go on, with the access they open.
      std::vector<std::uint16_t> reachable;
      std::vector<std::int32_t> opened;
      for (std::uint16_t i = 0; i < p.surface_squares; ++i) {
        if (sim.empty(i) && sim.touches_built(i)) {
          reachable.push_back(i);
          opened.push_back(access(sim, i));
        }
      }
      for (const std::uint8_t it : kEconomyItems) {
        if (reachable.empty() || !sim.can_place(it, kNoSquare) || (is_boost(it) && sim.find(it) != kNoSquare)) {
          continue;
        }
        std::array<std::uint16_t, 5> best_square{};
        std::array<std::int32_t, 5> best_access{};
        best_square.fill(kNoSquare);
        best_access.fill(-1);
        for (std::size_t k = 0; k < reachable.size(); ++k) {
          const std::uint16_t i = reachable[k];
          const std::size_t colour = sim.squares[i].color & 7u;
          if (colour < best_square.size() && opened[k] > best_access[colour] && sim.can_place(it, i)) {
            best_access[colour] = opened[k];
            best_square[colour] = i;
          }
        }
        // The colours that give the item a bonus and white, black for the
        // items that may go there (not white then); the others only without
        // those.
        const PlanItem& item = sim.item(it);
        const bool any_square = (item.flags & kItemAnySquare) != 0;
        const std::array<bool, 5> fitting{any_square, !any_square, item.industry > 0, item.prosperity > 0,
                                          item.research > 0};
        bool any_fitting = false;
        for (std::size_t c = 0; c < best_square.size(); ++c) {
          any_fitting = any_fitting || (fitting[c] && best_square[c] != kNoSquare);
        }
        for (std::size_t c = 0; c < best_square.size(); ++c) {
          if (best_square[c] != kNoSquare && (fitting[c] || !any_fitting)) {
            out.push_back({it, best_square[c], false});
          }
        }
      }
    }
    if (replace_) {
      // Stuck: no square for a new building, or housing full.
      const bool stuck = sim.free_population() >= 1 ? out.empty() : sim.room() < 1;
      for (const std::uint16_t victim : victims(sim, stuck)) {
        const std::uint8_t there = sim.squares[victim].item;
        sim.squares[victim] = Square{sim.squares[victim].color, kNoItem, 0};
        sim.recalc();
        for (const std::uint8_t it : kEconomyItems) {
          if (it != there && sim.can_place(it, victim)) {
            out.push_back({it, victim, true});
          }
        }
        sim.squares[victim].item = there;
        sim.squares[victim].flags = kSquareBuilt;
        sim.recalc();
      }
    }
    return out;
  }

  static bool is_boost(std::uint8_t it) {
    return it == kItemHyperpowerPlant || it == kItemFertilizationPlant || it == kItemInternet || it == kCloningPlant;
  }

  // The empty squares next to `square` that are not reachable yet (those
  // that are not black count double).
  static std::int32_t access(const Sim& sim, std::uint16_t square) {
    std::int32_t n = 0;
    for (const std::uint16_t s : sim.neighbours()[square]) {
      if (s != kNoSquare && sim.empty(s) && !sim.touches_built(s)) {
        n += sim.squares[s].color == 0 ? 1 : 2;
      }
    }
    return n;
  }

  // The buildings worth replacing: when nothing new can be built (`stuck`)
  // the removable economy buildings whose loss costs the least, else those
  // whose outputs the goal does not want (after a change of goal).
  std::vector<std::uint16_t> victims(Sim& sim, bool stuck) const {
    struct Victim {
      std::uint16_t square;
      double loss;
    };
    std::vector<Victim> all;
    const double rate = sim.rate(weights_);
    const std::int32_t prosperity = u16(sim.planet.prosperity);
    const std::int32_t housing = sim.planet.max_population;
    for (std::uint16_t i = 0; i < sim.planet.surface_squares; ++i) {
      const std::uint8_t it = sim.squares[i].item;
      if (!sim.built(i) || std::ranges::find(kEconomyItems, it) == kEconomyItems.end() || !sim.removable(i)) {
        continue;
      }
      const PlanItem& item = sim.item(it);
      const bool unwanted =
          (item.industry > 0 || item.research > 0) && item.prosperity <= 0 && item.max_population <= 0 &&
          weights_.industry * item.industry + weights_.research * item.research < 0.1 * (item.industry + item.research);
      if (!stuck && !unwanted) {
        continue;
      }
      const double loss = sim.with(Action{kNoItem, i, true}, [&](const Sim& s) {
        return rate - s.rate(weights_) + 0.5 * (prosperity - u16(s.planet.prosperity)) +
               (housing - s.planet.max_population);
      });
      all.push_back({i, loss});
    }
    std::ranges::stable_sort(all, [](const Victim& a, const Victim& b) { return a.loss < b.loss; });
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < all.size() && i < kVictims; ++i) {
      out.push_back(all[i].square);
    }
    return out;
  }

  static Look look(const Sim& s, const Weights& w) {
    return {s.rate(w), u16(s.planet.industry), u16(s.planet.prosperity), s.free_population(), s.room(), 0};
  }

  // The workers the planet can put to work within `left` days: its free
  // population and growth, up to the buildings its industry can build.
  static double workers(const Look& l, double left) {
    const double grown = std::clamp(left * l.prosperity / 50.0, 0.0, static_cast<double>(std::max(l.room, 0)));
    const double buildable = left * l.industry / kAverageCost;
    return std::min(std::max(l.free, 0) + grown, buildable);
  }

  // The goal's outputs a project adds up to the end of the horizon, per day
  // of building: its own outputs and the work of the population it lets
  // the planet employ (growth, housing, building speed).
  double estimate(Sim& sim, const Action& a, const Look& now, std::int32_t remaining) const {
    const std::int32_t days = build_days(sim.item(a.item).cost, now.industry);
    const double left = remaining - days;
    if (left <= 0) {
      return 0.0;
    }
    double lost = 0.0;
    if (a.replace) {
      lost = now.rate - sim.with(Action{kNoItem, a.square, true}, [&](const Sim& s) { return s.rate(weights_); });
    }
    const Look after = sim.with(a, [&](const Sim& s) { return look(s, weights_); });
    // A worker's output (a basic building on its colour).
    const double worker = 1.5 * std::max(weights_.industry, weights_.research);
    double value = (after.rate - now.rate) * left - lost * days;
    value += (workers(after, left) - workers(now, left)) * worker * left / 2.0;
    // A coloured square used without its bonus loses a later building's.
    const std::uint8_t colour = sim.squares[a.square].color & 7u;
    const PlanItem& it = sim.item(a.item);
    if ((colour == 2 && it.industry <= 0) || (colour == 3 && it.prosperity <= 0) || (colour == 4 && it.research <= 0)) {
      const double bonus = colour == 2 ? weights_.industry : colour == 4 ? weights_.research : worker / 2.0;
      value -= 0.5 * bonus * left;
    }
    // Squares made reachable are worth something when few are left.
    if (now.open < kFewSquares) {
      value += (a.item == kItemTerraforming ? 2 : access(sim, a.square)) * worker * left / 10.0;
    }
    return value / (days + 1);
  }

  // The goal's outputs over the horizon after starting `first`, the next
  // projects chosen by the quick estimate.
  double rollout(const Action& first) const {
    Sim sim = start_;
    if (first.item != kNoItem) {
      if (first.replace) {
        sim.squares[first.square] = Square{sim.squares[first.square].color, kNoItem, 0};
      }
      sim.start(first);
    }
    double total = 0.0;
    bool decide = true;
    for (std::int32_t t = 0; t < kHorizon; ++t) {
      if (sim.planet.project_item == kNoItem && decide) {
        const std::vector<Scored> s = scored(sim, kHorizon - t);
        const auto top = std::ranges::max_element(s, {}, &Scored::score);
        if (top != s.end() && top->score > 0.0) {
          if (top->action.replace) {
            sim.squares[top->action.square] = Square{sim.squares[top->action.square].color, kNoItem, 0};
          }
          sim.start(top->action);
        }
        decide = false;
      }
      if (sim.day()) {
        decide = true;
      }
      total += sim.rate(weights_);
    }
    return total + sim.rate(weights_) * kTail;
  }

  Neighbours neighbours_;
  Sim start_;
  Weights weights_;
  bool replace_;
};

// Starts `a` on the planet; returns whether it did.
bool apply(Session& session, Planet& planet, const Action& a, SquareMode mode = SquareMode::kProject) {
  if (a.item == kNoItem) {
    return false;
  }
  if (a.replace) {
    const std::uint8_t there = planet_squares(session.game->cosmos, planet)[a.square].item;
    if (!planet_set_square_item(session, planet, a.square, there, SquareMode::kRemove)) {
      return false;
    }
  }
  return planet_set_square_item(session, planet, a.square, a.item, mode);
}

std::uint16_t ruins_square(const Session& session, const Planet& planet) {
  const std::span<const Square> squares = planet_squares(session.game->cosmos, planet);
  for (std::uint16_t i = 0; i < planet.surface_squares; ++i) {
    if ((squares[i].color & kSquareRuins) != 0 && squares[i].item == kNoItem) {
      return i;
    }
  }
  return kNoSquare;
}

// Hands a planet back to the player.
void hand_back(Session& session, Planet& planet) {
  planet.self_managed = 0;
  session.game->planet_goals.at(static_cast<std::size_t>(planet_index(session.game->cosmos, planet))) =
      PlanetGoal::kBalanced;
}

// Defence goal: orbital weapons and shields, then surface shields, once the
// planet builds them in reasonable time.
bool defence_project(Session& session, Race& race, Planet& planet) {
  if (u16(planet.industry) < kDefenceIndustry ||
      static_cast<std::int32_t>(planet.population) - planet.used_population < 1) {
    return false;
  }
  if (pe_build_orbital_defence(session, race, planet)) {
    return true;
  }
  if (planet_surface_shield_strength(session.game->cosmos, planet) * kSurfaceShieldShare >= planet.surface_squares) {
    return false;
  }
  for (const std::uint8_t shield : {kSurfaceMegaShield, kSurfaceShield}) {
    if (planet_can_place_item(session, planet, shield, kNoSquare)) {
      return planet_auto_place_item(session, planet, shield, 0) != kNoSquare;
    }
  }
  return false;
}

// Shipyard goal: more industry while it brings the Shipyard nearer, then the
// Shipyard (and the planet back to the player).
bool shipyard_project(Session& session, Planet& planet, const Planner& planner) {
  Sim sim = planner.start();
  if (!sim.can_place(kItemShipyard, kNoSquare) || planet.free_orbital == 0) {
    return false;
  }
  const std::int32_t cost = sim.item(kItemShipyard).cost;
  const std::int32_t industry = u16(planet.industry);
  std::int32_t best_days = build_days(cost, industry);
  Action faster;
  for (const Planner::Scored& c : planner.scored(sim, kHorizon)) {
    if (c.action.replace) {
      continue;
    }
    const std::int32_t after = sim.with(c.action, [](const Sim& s) { return u16(s.planet.industry); });
    const std::int32_t days = build_days(sim.item(c.action.item).cost, industry) + build_days(cost, after);
    if (days < best_days) {
      best_days = days;
      faster = c.action;
    }
  }
  if (faster.item != kNoItem) {
    return apply(session, planet, faster);
  }
  if (planet_auto_place_item(session, planet, kItemShipyard, 0) == kNoSquare) {
    return false;
  }
  hand_back(session, planet);
  return true;
}

// Archaeology goal: the shortest way of Transport Tubes to the ruins, then
// the dig (and the planet back to the player).
void archaeology_project(Session& session, Planet& planet, Weights weights) {
  const std::uint16_t ruins = ruins_square(session, planet);
  if (ruins == kNoSquare) {
    hand_back(session, planet);
    return;
  }
  if (planet_can_place_item(session, planet, kItemXenoDig, ruins)) {
    if (planet_set_square_item(session, planet, ruins, kItemXenoDig, SquareMode::kProject)) {
      hand_back(session, planet);
    }
    return;
  }
  const Neighbours neighbours = surface_neighbours(session.tables->planitems, planet);
  const Sim sim(session, planet, neighbours);
  std::uint16_t next = kNoSquare;
  if (!sim.touches_built(ruins)) {
    // Breadth first from the ruins over empty squares to the nearest one a
    // building may go on now.
    std::vector<std::uint16_t> queue{ruins};
    std::vector<bool> seen(planet.surface_squares, false);
    seen[ruins] = true;
    for (std::size_t q = 0; q < queue.size() && next == kNoSquare; ++q) {
      for (const std::uint16_t n : neighbours[queue[q]]) {
        if (n == kNoSquare || seen[n] || !sim.empty(n)) {
          continue;
        }
        seen[n] = true;
        if (sim.touches_built(n)) {
          next = n;
          break;
        }
        queue.push_back(n);
      }
    }
  }
  if (next == kNoSquare) {
    // Next to the ruins already (the dig waits for its research or a free
    // worker), or no way there: the economy meanwhile.
    apply(session, planet, Planner(session, planet, weights).best());
    return;
  }
  // The way: Transport Tubes, the cheapest building, which keeps no worker
  // once built; without a free worker to start them, the economy.
  if (!planet_can_place_item(session, planet, kTransportTubes, next) ||
      !planet_set_square_item(session, planet, next, kTransportTubes, SquareMode::kProject)) {
    apply(session, planet, Planner(session, planet, weights).best());
  }
}

}  // namespace

std::string_view planet_goal_name(PlanetGoal goal) {
  switch (goal) {
    case PlanetGoal::kBalanced:
      return "Balanced";
    case PlanetGoal::kIndustry:
      return "Industry";
    case PlanetGoal::kResearch:
      return "Research";
    case PlanetGoal::kDefence:
      return "Defence";
    case PlanetGoal::kShipyard:
      return "Shipyard";
    case PlanetGoal::kArchaeology:
      return "Archaeology";
  }
  return "Balanced";
}

bool planet_goal_available(const Session& session, const Planet& planet, PlanetGoal goal) {
  switch (goal) {
    case PlanetGoal::kShipyard:
      return planet_find_item(session.game->cosmos, planet, kItemShipyard) == kNoSquare &&
             race_knows_item(session, planet, kItemShipyard);
    case PlanetGoal::kArchaeology:
      return ruins_square(session, planet) != kNoSquare;
    default:
      return true;
  }
}

PlanetGoal planet_goal(const Session& session, const Planet& planet) {
  if (!is_player_planet(session, planet)) {
    return PlanetGoal::kBalanced;
  }
  const PlanetGoal goal =
      session.game->planet_goals.at(static_cast<std::size_t>(planet_index(session.game->cosmos, planet)));
  return planet_goal_available(session, planet, goal) ? goal : PlanetGoal::kBalanced;
}

void pe_manage_planet(Session& session, Race& race, Planet& planet) {
  const bool player = is_player_planet(session, planet);
  const PlanetGoal goal = planet_goal(session, planet);
  const Weights weights = goal_weights(goal, antag_race_research_complete(session, race));
  // The first buildings, finished at once and for free (as the
  // Antagonizer's management).
  if (planet_count_buildings(session.game->cosmos, planet) < 3 &&
      apply(session, planet, Planner(session, planet, weights, false).best(), SquareMode::kPlaceBuilt)) {
    return;
  }
  if (planet.industry == 0) {
    return;
  }
  if (player) {
    if (pe_danger_at(session, race, planet) && pe_build_orbital_defence(session, race, planet)) {
      return;
    }
  } else if (antag_race_ai_choose_orbital_project(session, race, planet) == 1 ||
             antag_race_ai_planet_specials(session, race, planet)) {
    // The AI's orbital projects and specials stay the Antagonizer's.
    return;
  }
  switch (goal) {
    case PlanetGoal::kDefence:
      if (defence_project(session, race, planet)) {
        return;
      }
      break;
    case PlanetGoal::kShipyard: {
      const Planner planner(session, planet, weights);
      if (!shipyard_project(session, planet, planner)) {
        apply(session, planet, planner.best());
      }
      return;
    }
    case PlanetGoal::kArchaeology:
      archaeology_project(session, planet, weights);
      return;
    default:
      break;
  }
  apply(session, planet, Planner(session, planet, weights).best());
}

}  // namespace oa::game
