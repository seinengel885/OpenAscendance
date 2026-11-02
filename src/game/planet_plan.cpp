#include "game/planet_plan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "game/address.h"
#include "game/antag_planet.h"
#include "game/planet.h"
#include "game/planet_plan_sim.h"
#include "game/tables.h"

namespace oa::game {

namespace {

using namespace plan_detail;

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
// The victims of a replacement tried: the least useful buildings for which
// something else may go there.
constexpr std::size_t kVictims = 3;
// The average cost of a building, for the buildings industry can build.
constexpr double kAverageCost = 40.0;
// Fewer reachable empty squares than this make opening new ones worth it.
constexpr std::int32_t kFewSquares = 2;
// Defence goal: the industry from which defences come first, and the
// share of the surface that surface shields may take (one in four).
constexpr std::int32_t kDefenceIndustry = 5;
constexpr std::int32_t kSurfaceShieldShare = 4;


bool is_player_planet(const Session& session, const Planet& planet) {
  return planet.owner == session.player_race && !session.autoplay;
}


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





// What the quick estimate reads of a planet.
struct Look {
  double rate = 0.0;
  std::int32_t industry = 0;
  std::int32_t prosperity = 0;
  std::int32_t free = 0;
  std::int32_t room = 0;
  // The growth gathered toward the next inhabitant, the growth a day and
  // the inhabitants each one brings (two with a Cloning Plant).
  std::int32_t growth = 0;
  std::int32_t growth_rate = 0;
  std::int32_t per_growth = 1;
  // The empty squares (not black) a building may go on, and all of them.
  std::int32_t open = 0;
  std::int32_t empty = 0;
};

// The planner of one planet's economy.
class Planner {
 public:
  Planner(const Session& session, const Planet& planet, Weights weights)
      : neighbours_(surface_neighbours(session.tables->planitems, planet)),
        start_(session, planet, neighbours_),
        weights_(weights) {}

  const Sim& start() const { return start_; }

  // A candidate project with its quick estimate.
  struct Scored {
    Action action;
    double score = 0.0;
  };

  // The best project for the planet now (no item: wait).
  Action best() const {
    Sim sim = start_;
    if (const std::optional<Action> a = workforce(sim, kHorizon)) {
      return *a;
    }
    std::vector<Scored> candidates = scored(sim, kHorizon);
    if (candidates.empty()) {
      return {};
    }
    std::ranges::stable_sort(candidates, [](const Scored& a, const Scored& b) { return a.score > b.score; });
    // Replacements (the planet is full, see `scored`): the best one by the
    // quick estimate, if it gains more than the outputs lost while
    // rebuilding. (Playing them forward would credit each with the later
    // replacements, while waiting gets none.)
    if (std::ranges::any_of(candidates, [](const Scored& c) { return c.action.victim != kNoSquare; })) {
      return candidates.front().score > 0.0 ? candidates.front().action : Action{};
    }
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

  // The workforce rule, before any reckoning: while empty squares wait for
  // more workers than the planet has or will have, the planet's last free
  // worker builds housing in time (when at most one inhabitant's room is
  // left), and growth is restarted where one building can; without a worker
  // to build them, the least useful building (not one of the same kind)
  // makes way.
  // Returns the best such project by the quick estimate, if one is due.
  std::optional<Action> workforce(Sim& sim, std::int32_t remaining) const {
    const Look now = look(sim, weights_);
    if (now.free > 1 || now.empty <= std::max(now.free, 0) + std::max(now.room, 0)) {
      return std::nullopt;
    }
    bool (*fits)(const PlanItem&) = nullptr;
    if (now.room <= 1) {
      fits = [](const PlanItem& it) { return it.max_population > 0; };
    } else if (now.growth_rate == 0) {
      fits = [](const PlanItem& it) { return it.prosperity > 0; };
    } else {
      return std::nullopt;
    }
    std::vector<Action> options = new_buildings(sim);
    std::erase_if(options, [&](const Action& a) { return !fits(sim.item(a.item)); });
    if (options.empty()) {
      // Never at the cost of a building of the same kind.
      options = replacements(sim);
      std::erase_if(options, [&](const Action& a) {
        return !fits(sim.item(a.item)) || fits(sim.item(sim.squares[a.victim].item));
      });
    }
    // Growth only where the building restarts it (with many inhabitants one
    // building may not).
    if (now.room > 1) {
      std::erase_if(options,
                    [&](const Action& a) { return sim.with(a, [](const Sim& x) { return x.growth_rate(); }) == 0; });
    }
    std::optional<Action> best;
    double best_score = 0.0;
    for (const Action& a : options) {
      const double v = estimate(sim, a, now, remaining);
      if (!best || v > best_score) {
        best = a;
        best_score = v;
      }
    }
    return best;
  }

  // The economy building whose loss costs the goal the least, `kNoSquare`
  // if none may go.
  std::uint16_t least_useful() const {
    Sim sim = start_;
    const std::vector<std::uint16_t> v = victims(sim);
    return v.empty() ? kNoSquare : v.front();
  }

  std::vector<Scored> scored(Sim& sim, std::int32_t remaining) const {
    std::vector<Scored> out;
    const Look now = look(sim, weights_);
    for (const Action& a : new_buildings(sim)) {
      out.push_back({a, estimate(sim, a, now, remaining)});
    }
    // Buildings are replaced only when no new one is worth it (no empty
    // square left for it, or no worker to build with) and the planet does
    // not just wait for its next inhabitant.
    const bool waiting = now.free < 1 && now.room >= 1 && now.growth_rate > 0;
    if (!waiting && std::ranges::none_of(out, [](const Scored& c) { return c.score > 0.0; })) {
      for (const Action& a : replacements(sim)) {
        out.push_back({a, estimate(sim, a, now, remaining)});
      }
    }
    return out;
  }

 private:
  // The candidates: per item and square colour the reachable empty square
  // that opens the most new squares, and the replacements of the least
  // useful buildings.
  // The replacements of the least useful buildings: the demolition frees a
  // worker and its square; the new building may go there or on an empty
  // square.
  std::vector<Action> replacements(Sim& sim) const {
    std::vector<Action> out;
    {
      std::size_t tried = 0;
      for (const std::uint16_t victim : victims(sim)) {
        if (tried == kVictims) {
          break;
        }
        const Square there = sim.squares[victim];
        sim.clear(victim);
        sim.recalc();
        std::vector<Action> builds = new_buildings(sim);
        // Not the same building again (elsewhere it would only move).
        std::erase_if(builds, [&](const Action& a) { return a.item == there.item; });
        for (Action& a : builds) {
          a.victim = victim;
          out.push_back(a);
        }
        tried += builds.empty() ? 0 : 1;
        sim.squares[victim] = there;
        sim.recalc();
      }
    }
    return out;
  }

  // The new buildings: per item and square colour the reachable empty
  // square that opens the most new squares.
  std::vector<Action> new_buildings(const Sim& sim) const {
    std::vector<Action> out;
    const Planet& p = sim.planet;
    if (sim.free_population() < 1) {
      return out;
    }
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
      // Transport Tubes only where they open new squares.
      const bool tubes = it == kTransportTubes;
      std::array<std::uint16_t, 5> best_square{};
      std::array<std::int32_t, 5> best_access{};
      best_square.fill(kNoSquare);
      best_access.fill(-1);
      for (std::size_t k = 0; k < reachable.size(); ++k) {
        const std::uint16_t i = reachable[k];
        const std::size_t colour = sim.squares[i].color & 7u;
        if (colour < best_square.size() && opened[k] > best_access[colour] && (!tubes || opened[k] > 0) &&
            sim.can_place(it, i)) {
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
          out.push_back({it, best_square[c]});
        }
      }
    }
    return out;
  }

  static bool is_boost(std::uint8_t it) {
    return it == kItemHyperpowerPlant || it == kItemFertilizationPlant || it == kItemInternet || it == kCloningPlant;
  }

  // The empty squares (not black) next to `square` that are not reachable
  // yet.
  static std::int32_t access(const Sim& sim, std::uint16_t square) {
    std::int32_t n = 0;
    for (const std::uint16_t s : sim.neighbours()[square]) {
      if (s != kNoSquare && sim.empty(s) && sim.squares[s].color != 0 && !sim.touches_built(s)) {
        ++n;
      }
    }
    return n;
  }

  // The removable economy buildings (not the Transport Tubes, which link
  // squares), the one whose loss costs the goal the least first (after a
  // change of goal those whose outputs it does not want).
  std::vector<std::uint16_t> victims(Sim& sim) const {
    struct Victim {
      std::uint16_t square;
      double loss;
    };
    std::vector<Victim> all;
    // A building's worth a day: its outputs and the work of the inhabitants
    // its prosperity and housing bring (as the quick estimate counts them).
    const Look now = look(sim, weights_);
    const auto worth = [&](const Look& l) { return l.rate + workers(l, kHorizon) * worker_output() / 2.0; };
    for (std::uint16_t i = 0; i < sim.planet.surface_squares; ++i) {
      const std::uint8_t it = sim.squares[i].item;
      if (!sim.built(i) || it == kTransportTubes || std::ranges::find(kEconomyItems, it) == kEconomyItems.end() ||
          !sim.removable(i)) {
        continue;
      }
      const double loss =
          worth(now) - sim.with(Action{kNoItem, i, i}, [&](const Sim& s) { return worth(look(s, weights_)); });
      all.push_back({i, loss});
    }
    std::ranges::stable_sort(all, [](const Victim& a, const Victim& b) { return a.loss < b.loss; });
    std::vector<std::uint16_t> out;
    for (const Victim& v : all) {
      out.push_back(v.square);
    }
    return out;
  }

  // A worker's output (a basic building on its colour).
  double worker_output() const { return 1.5 * std::max(weights_.industry, weights_.research); }

  static Look look(const Sim& s, const Weights& w) {
    Look l{s.rate(w), u16(s.planet.industry), u16(s.planet.prosperity), s.free_population(), s.room()};
    l.growth = s.planet.growth;
    l.growth_rate = s.growth_rate();
    l.per_growth = s.built(s.find(kCloningPlant)) ? 2 : 1;
    for (std::uint16_t i = 0; i < s.planet.surface_squares; ++i) {
      if (s.empty(i) && s.squares[i].color != 0) {
        ++l.empty;
        l.open += s.touches_built(i) ? 1 : 0;
      }
    }
    return l;
  }

  // The workers the planet can put to work within `left` days: its free
  // population and the inhabitants its growth brings (from the growth
  // gathered so far, as `planet_daily_update` counts), up to the buildings
  // its industry can build and its empty squares hold.
  double workers(const Look& l, double left) const {
    const double born = std::floor((l.growth + left * l.growth_rate) / start_.pop_growth()) * l.per_growth;
    const double grown = std::clamp(born, 0.0, static_cast<double>(std::max(l.room, 0)));
    const double buildable = left * l.industry / kAverageCost;
    return std::min({std::max(l.free, 0) + grown, buildable, static_cast<double>(l.empty)});
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
    if (a.victim != kNoSquare) {
      lost = now.rate - sim.with(Action{kNoItem, a.victim, a.victim}, [&](const Sim& s) { return s.rate(weights_); });
    }
    const Look after = sim.with(a, [&](const Sim& s) { return look(s, weights_); });
    const double worker = worker_output();
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
      if (first.victim != kNoSquare) {
        sim.clear(first.victim);
      }
      sim.start(first);
    }
    double total = 0.0;
    // Waiting means: until the population or the buildings change.
    bool decide = first.item != kNoItem;
    for (std::int32_t t = 0; t < kHorizon; ++t) {
      if (sim.planet.project_item == kNoItem && decide) {
        std::optional<Action> next = workforce(sim, kHorizon - t);
        if (!next) {
          const std::vector<Scored> s = scored(sim, kHorizon - t);
          const auto top = std::ranges::max_element(s, {}, &Scored::score);
          if (top != s.end() && top->score > 0.0) {
            next = top->action;
          }
        }
        if (next) {
          if (next->victim != kNoSquare) {
            sim.clear(next->victim);
          }
          sim.start(*next);
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
};

// Starts `a` on the planet; returns whether it did.
bool apply(Session& session, Planet& planet, const Action& a) {
  if (a.item == kNoItem) {
    return false;
  }
  if (a.victim != kNoSquare) {
    const std::uint8_t there = planet_squares(session.game->cosmos, planet)[a.victim].item;
    if (!planet_set_square_item(session, planet, a.victim, there, SquareMode::kRemove)) {
      return false;
    }
  }
  return planet_set_square_item(session, planet, a.square, a.item, SquareMode::kProject);
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

// Defence goal, once the planet builds defences in reasonable time: orbital
// weapons and shields in turn, then surface shields (against invasion) up
// to a share of the surface, then better orbital defences instead of
// weaker ones. Without a free worker for them the least useful building
// makes way.
bool defence_project(Session& session, Race& race, Planet& planet, Weights weights) {
  if (u16(planet.industry) < kDefenceIndustry) {
    return false;
  }
  const bool worker = static_cast<std::int32_t>(planet.population) - planet.used_population >= 1;
  const bool orbit_due = planet.free_orbital > 0 && (planet_best_orbital_shield_item(session, planet) != kNoItem ||
                                                     planet_best_orbital_weapon_item(session, planet) != kNoItem);
  std::uint8_t shield = kNoItem;
  for (const std::uint8_t it : {kSurfaceShield, kSurfaceMegaShield}) {
    shield = race_knows_item(session, planet, it) ? it : shield;
  }
  const bool shield_due =
      shield != kNoItem &&
      planet_surface_shield_strength(session.game->cosmos, planet) * kSurfaceShieldShare < planet.surface_squares;
  if (!orbit_due && !shield_due) {
    // The orbit full: its weaker defences replaced.
    return worker && pe_build_orbital_defence(session, race, planet);
  }
  // Without a free worker, the least useful building makes way (a surface
  // shield takes its square).
  std::uint16_t victim = kNoSquare;
  if (!worker) {
    victim = Planner(session, planet, weights).least_useful();
    const std::span<const Square> squares = planet_squares(session.game->cosmos, planet);
    if (victim == kNoSquare ||
        !planet_set_square_item(session, planet, victim, squares[victim].item, SquareMode::kRemove)) {
      return false;
    }
  }
  if (orbit_due && pe_build_orbital_defence(session, race, planet)) {
    return true;
  }
  if (!shield_due) {
    return false;
  }
  if (victim != kNoSquare && planet_can_place_item(session, planet, shield, victim)) {
    return planet_set_square_item(session, planet, victim, shield, SquareMode::kProject);
  }
  return planet_auto_place_item(session, planet, shield, 0) != kNoSquare;
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
    if (c.action.victim != kNoSquare) {
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
    case PlanetGoal::kBalancedV2:
      return "Balanced V2";
    case PlanetGoal::kIndustryV2:
      return "Industry V2";
    case PlanetGoal::kResearchV2:
      return "Research V2";
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
  if (planet_goal_v2(goal)) {
    pe_manage_planet_v2(session, planet, goal);
    return;
  }
  switch (goal) {
    case PlanetGoal::kDefence:
      if (defence_project(session, race, planet, weights)) {
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
