// The planet management's rules (`game/planet_plan_rules.cpp`) on a new
// colony of each size, under each strategy: the scenario tests of
// `plan_rig.h`.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <format>
#include <map>
#include <vector>

#include "game/antag_planet.h"
#include "game/cosmos.h"
#include "game/planet.h"
#include "game/planet_final.h"
#include "game/player_actions.h"
#include "game/race_ai.h"
#include "game/ship.h"
#include "game_oracle.h"
#include "plan_rig.h"

namespace oa::game {
namespace {

using plan_test::BuildStep;
using plan_test::DayRecord;
using plan_test::PlanRig;

constexpr int kDays = 600;
constexpr std::array<int, 2> kSeeds{7, 11};
// The final build-out presets the scenario tests run with.
constexpr std::array<FinalPreset, 3> kPresets{FinalPreset::kResearch, FinalPreset::kShipyard, FinalPreset::kDock};
// The day each size is full by: the latest of the rules of 2026-11-21
// (seeds 7, 11, all strategies) and a tenth more, at most `kDays`; small
// planets a few days more for the last 6 buildings toward the final layout.
constexpr std::array<int, 5> kFullBy{120, 165, 335, 370, kDays};

bool housing_or_growth(const GameTables& t, std::uint8_t item) {
  const PlanItem& it = t.planitems.items.at(item);
  return it.max_population > 0 || it.prosperity > 0;
}

// The faults of a run, one line each (empty: none).
std::vector<std::string> faults(const GameTables& t, PlanRig& rig) {
  std::vector<std::string> out;
  const std::optional<int> full = rig.full_day();
  if (const int by = kFullBy.at(rig.planet().size); !full || *full > by) {
    out.push_back(std::format("full on day {}, not by day {}", full.value_or(-1), by));
  }
  int idle = 0;
  int stopped = 0;
  for (const DayRecord& r : rig.days) {
    if (full && r.day >= *full) {
      break;
    }
    // No project although a worker and a square are free.
    idle = !r.project && r.free_workers > 0 && r.open > 0 ? idle + 1 : 0;
    if (idle == 2) {
      out.push_back(std::format("day {}: idle with {} free workers, {} open squares", r.day, r.free_workers, r.open));
    }
    stopped = r.prosperity == 0 && r.population > 2 ? stopped + 1 : 0;
    if (stopped == 2) {
      out.push_back(std::format("day {}: growth stopped before full", r.day));
    }
  }
  std::map<std::uint16_t, std::vector<std::uint8_t>> history;
  for (const BuildStep& s : rig.steps) {
    if (s.removed != kNoItem && (!full || s.day < *full) && !housing_or_growth(t, s.item)) {
      out.push_back(std::format("day {}: {} replaced by {} before full", s.day, t.planitems.items.at(s.removed).name,
                                t.planitems.items.at(s.item).name));
    }
    std::vector<std::uint8_t>& h = history[s.square];
    if (h.size() >= 2 && h[h.size() - 2] == s.item && s.removed != kNoItem) {
      out.push_back(std::format("day {}: ping-pong on square {}: {} again", s.day, s.square,
                                t.planitems.items.at(s.item).name));
    }
    h.push_back(s.item);
  }
  return out;
}

TEST_CASE("a managed colony builds without idling, early demolition or ping-pong", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  for (const int seed : kSeeds) {
    for (int size = 0; size < 5; ++size) {
      for (const FinalPreset preset : kPresets) {
        PlanRig rig(*t, seed, size);
        if (!rig.ready()) {
          continue;
        }
        rig.management().final_outputs = planet_final_preset(preset, rig.planet());
        rig.run(kDays);
        const std::optional<int> full = rig.full_day();
        std::printf("C seed %d size %d preset %d squares %d full %d pop %d I %d R %d\n", seed, size,
                    static_cast<int>(preset), rig.planet().surface_squares, full.value_or(-1),
                    rig.days.back().population, rig.days.back().industry, rig.days.back().research);
        CAPTURE(seed, size, static_cast<int>(preset));
        const std::vector<std::string> found = faults(*t, rig);
        for (const std::string& f : found) {
          UNSCOPED_INFO(f);
        }
        CHECK(found.empty());
      }
    }
  }
}

// The outputs summed over the first `days` days of `rig`.
std::pair<long, long> sums(const PlanRig& rig, int days) {
  long industry = 0;
  long research = 0;
  for (const DayRecord& r : rig.days) {
    if (r.day <= days) {
      industry += r.industry;
      research += r.research;
    }
  }
  return {industry, research};
}

TEST_CASE("a final layout maximising research has research sooner, one maximising industry industry",
          "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig research(*t, 7, 3);
  PlanRig industry(*t, 7, 3);
  REQUIRE(research.ready());
  industry.management().final_outputs = planet_final_preset(FinalPreset::kDock, industry.planet());
  research.run(400);
  industry.run(400);
  for (std::size_t d = 24; d < 400; d += 25) {
    std::printf("F day %d  research I %d R %d  industry I %d R %d\n", research.days[d].day, research.days[d].industry,
                research.days[d].research, industry.days[d].industry, industry.days[d].research);
  }
  // The early phase: half the days the industry layout takes to be full.
  const int early = industry.full_day().value_or(400) / 2;
  const auto [ri, rr] = sums(research, early);
  const auto [ii, ir] = sums(industry, early);
  CAPTURE(early, ri, rr, ii, ir);
  CHECK(rr > ir);
  CHECK(ii > ri);
}

// The research buildings `rig`'s planet started, each with its square's
// colour.
std::vector<std::pair<std::uint8_t, std::uint8_t>> research_built(const GameTables& t, PlanRig& rig) {
  std::vector<std::pair<std::uint8_t, std::uint8_t>> out;
  const std::span<const Square> sq = planet_squares(rig.session().game->cosmos, rig.planet());
  for (const BuildStep& s : rig.steps) {
    if (t.planitems.items.at(s.item).research > 0) {
      out.emplace_back(s.item, static_cast<std::uint8_t>(sq[s.square].color & 7u));
    }
  }
  return out;
}

TEST_CASE("a final layout without research builds no research building on the way", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  for (const int seed : {7, 11}) {
    PlanRig rig(*t, seed, 3);
    REQUIRE(rig.ready());
    rig.management().final_outputs = planet_final_preset(FinalPreset::kShipyard, rig.planet());
    rig.management().final_outputs[PlanetManagement::kResearch] = FinalOutput{FinalMode::kAtLeast, 0, kFinalWeights};
    rig.run(600);
    CAPTURE(seed);
    CHECK(research_built(*t, rig).empty());
  }
}

TEST_CASE("research kept to its own squares builds research only on blue squares", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  for (const int seed : {7, 11}) {
    PlanRig rig(*t, seed, 3);
    REQUIRE(rig.ready());
    rig.management().final_outputs = planet_final_preset(FinalPreset::kShipyard, rig.planet());
    REQUIRE(rig.management().final_outputs[PlanetManagement::kResearch].mode == FinalMode::kKeep);
    rig.run(600);
    const auto built = research_built(*t, rig);
    CAPTURE(seed, built.size());
    CHECK(!built.empty());
    CHECK(std::ranges::all_of(built, [](const auto& b) { return b.second == 4; }));
  }
}

// The squares of each output's buildings and of housing on `rig`'s planet
// (`planet_final_output`, `planet_final_housing`).
std::pair<std::array<int, PlanetManagement::kOutputs>, int> built_layout(PlanRig& rig) {
  std::array<int, PlanetManagement::kOutputs> out{};
  int housing = 0;
  const Planet& p = rig.planet();
  for (const Square& sq : planet_squares(rig.session().game->cosmos, p).first(p.surface_squares)) {
    if (sq.item == kNoItem) {
      continue;
    }
    if (const std::optional<PlanetManagement::Output> o = planet_final_output(sq.item)) {
      ++out[*o];
    }
    housing += planet_final_housing(sq.item) ? 1 : 0;
  }
  return {out, housing};
}

TEST_CASE("close to full a managed colony builds only what its final layout wants", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  for (const int size : {1, 4}) {
    for (const FinalPreset preset : {FinalPreset::kResearch, FinalPreset::kShipyard}) {
      PlanRig rig(*t, 7, size);
      REQUIRE(rig.ready());
      rig.management().final_outputs = planet_final_preset(preset, rig.planet());
      int unwanted = 0;
      while (rig.days.empty() || (rig.days.back().empty > 0 && rig.days.back().day < kDays)) {
        // What the final layout still wants before the day.
        const FinalLayout target =
            planet_final_layout(rig.session(), rig.planet(), planet_management(rig.session(), rig.planet()));
        const auto [built, housing] = built_layout(rig);
        std::array<int, PlanetManagement::kOutputs> missing{};
        for (std::size_t o = 0; o < missing.size(); ++o) {
          missing[o] = std::max(0, target.squares[o] - built[o]);
        }
        // The last 6 buildings.
        const bool follow = !rig.days.empty() && rig.days.back().empty <= 6;
        const std::size_t before = rig.steps.size();
        rig.day();
        for (std::size_t k = before; follow && k < rig.steps.size(); ++k) {
          const BuildStep& s = rig.steps[k];
          const std::optional<PlanetManagement::Output> o = planet_final_output(s.item);
          // The other buildings (defences) the layout keeps squares for.
          const bool wanted = o                               ? missing[*o] > 0
                              : planet_final_housing(s.item) ? target.housing > housing
                                                              : true;
          // Growth may still take a square when the next inhabitant is far.
          if (s.removed == kNoItem && !wanted && t->planitems.items.at(s.item).prosperity <= 0) {
            ++unwanted;
            UNSCOPED_INFO(std::format("day {}: {} not in the final layout", s.day, t->planitems.items.at(s.item).name));
          }
        }
      }
      CAPTURE(size, static_cast<int>(preset));
      CHECK(unwanted == 0);
    }
  }
}

// Whether `rig`'s planet has its final layout (housing within one).
bool at_final_layout(PlanRig& rig) {
  const FinalLayout target =
      planet_final_layout(rig.session(), rig.planet(), planet_management(rig.session(), rig.planet()));
  const auto [built, housing] = built_layout(rig);
  for (std::size_t o = 0; o < built.size(); ++o) {
    if (built[o] != target.squares[o]) {
      return false;
    }
  }
  return std::abs(housing - target.housing) <= 1;
}

// A full colony (seed 7, size 2, balanced, research preset, no research
// after the start), settled for 150 days.
PlanRig& full_colony(PlanRig& rig) {
  rig.stop_research();
  rig.run_until_full(kDays);
  rig.run(150);
  return rig;
}

TEST_CASE("a full colony rebuilds to a new final layout and stays", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  full_colony(rig);
  REQUIRE(rig.full_day().has_value());
  rig.management().final_outputs = planet_final_preset(FinalPreset::kShipyard, rig.planet());
  const int changed = rig.days.back().day;
  int reached = 0;
  while (reached == 0 && rig.days.back().day < changed + 300) {
    rig.day();
    reached = at_final_layout(rig) ? rig.days.back().day : 0;
  }
  CAPTURE(changed, reached);
  REQUIRE(reached > 0);
  const std::size_t steps = rig.steps.size();
  rig.run(100);
  CHECK(rig.steps.size() == steps);
  CHECK(at_final_layout(rig));
}

TEST_CASE("a Research Campus found during the final build-out replaces the labs once", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  constexpr std::uint8_t kLaboratory = 2;
  constexpr std::uint8_t kCampus = 8;
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  full_colony(rig);
  rig.learn(kCampus);
  const std::size_t from = rig.steps.size();
  rig.run(400);
  int labs = 0;
  for (const Square& sq : planet_squares(rig.session().game->cosmos, rig.planet()).first(rig.planet().surface_squares)) {
    labs += sq.item == kLaboratory ? 1 : 0;
  }
  CHECK(labs == 0);
  for (std::size_t k = from; k < rig.steps.size(); ++k) {
    CAPTURE(rig.steps[k].day);
    CHECK(rig.steps[k].item != kLaboratory);
  }
  CHECK(at_final_layout(rig));
}

TEST_CASE("a built-up colony maximising research takes over the scientists", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  full_colony(rig);
  rig.learn(kItemScientistTakeover);
  rig.run(200);
  CHECK(rig.planet().project_item == kItemScientistTakeover);
  // New settings: the takeover ends, the planet rebuilds.
  rig.management().final_outputs = planet_final_preset(FinalPreset::kShipyard, rig.planet());
  const std::size_t steps = rig.steps.size();
  rig.run(30);
  CHECK(rig.steps.size() > steps);
}

TEST_CASE("a colony of a species knowing all research takes over no scientists", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  full_colony(rig);
  Research& research = rig.session().game->research;
  for (std::size_t i = 0; i < research.count; ++i) {
    research.items[i].known_mask |= static_cast<std::uint8_t>(1u << rig.session().player_race);
  }
  bool takeover = false;
  for (int d = 0; d < 300; ++d) {
    rig.day();
    takeover = takeover || rig.planet().project_item == kItemScientistTakeover;
  }
  CHECK_FALSE(takeover);
}


// Test B's hand calculation, on the planet as it was when the planner chose
// (the new project taken off again): the game's outputs of the squares
// (`planet_recalc_output`) and the days of building worked out by hand, not
// with the planner's copy.
class Hand {
 public:
  enum Output { kIndustry, kResearch, kProsperity };

  Hand(PlanRig& rig, std::uint16_t project) : s_(rig.session()), p_(rig.planet()) {
    const std::span<const Square> real = planet_squares(s_.game->cosmos, p_);
    sq_.assign(real.begin(), real.end());
    if (project != kNoSquare) {
      sq_[project] = Square{sq_[project].color, kNoItem, 0};
    }
    p_.project_item = kNoItem;
    planet_recalc_output(s_, p_, sq_);
  }

  // An empty square of `colour` (else white, else any) where `item` may go
  // once `built` is built.
  std::uint16_t square(std::uint8_t item, std::uint8_t colour,
                       std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> built) const {
    const std::vector<Square> sq = with(built);
    for (const int c : {int{colour}, 1, -1}) {
      for (std::uint16_t i = 0; i < p_.surface_squares; ++i) {
        if (sq[i].item == kNoItem && (c < 0 || (sq[i].color & 7u) == c) && (sq[i].color & kSquareRuins) == 0 &&
            planet_can_place_item(s_, p_, sq, item, i)) {
          return i;
        }
      }
    }
    return kNoSquare;
  }

  // `o` (industry and prosperity in tenths) with `built` built.
  std::int32_t output(Output o, std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> built) const {
    Planet p = p_;
    const std::vector<Square> sq = with(built);
    planet_recalc_output(s_, p, sq);
    return o == kIndustry ? planet_industry_tenths(p) : o == kResearch ? static_cast<std::uint16_t>(p.research)
                                                                       : planet_prosperity_tenths(p);
  }

  // Whether `large` may go on a square of the colour of `o` (else white,
  // else any) and takes at most 50 days at the industry of now.
  bool large_soon(std::uint8_t large, Output o) const {
    return square(large, o == kIndustry ? 2 : 4, {}) != kNoSquare && days(large, output(kIndustry, {})) <= 50;
  }

  // Whether the boost on `square` brings more `o` than the best building of
  // `o` there.
  bool boost_worth(std::uint16_t square, std::uint8_t boost, Output o) const {
    const std::int32_t now = output(o, {});
    std::int32_t best = 0;
    // The output buildings (`planitem.txt` 0 to 10).
    for (std::uint8_t it = 0; it <= 10; ++it) {
      const PlanItem& item = s_.tables->planitems.items.at(it);
      const std::int32_t v = o == kIndustry ? item.industry : o == kResearch ? item.research : item.prosperity;
      if (v > 0 && it != kItemColonyBase && planet_can_place_item(s_, p_, sq_, it, square)) {
        best = std::max(best, output(o, {{square, it}}) - now);
      }
    }
    return output(o, {{square, boost}}) - now > best;
  }

 private:
  // The squares with `built` built.
  std::vector<Square> with(std::initializer_list<std::pair<std::uint16_t, std::uint8_t>> built) const {
    std::vector<Square> sq = sq_;
    for (const auto& [square, item] : built) {
      sq[square].item = item;
      sq[square].flags = kSquareBuilt;
    }
    return sq;
  }
  std::int32_t days(std::uint8_t item, std::int32_t industry) const {
    return (10 * s_.tables->planitems.items.at(item).cost + industry - 1) / industry;
  }

  Session& s_;
  Planet p_;
  std::vector<Square> sq_;
};

// Test B: on later planets (technology found on fixed days) the
// Megafacility and the Research Campus come in place of the Factory and the
// Laboratory whenever they take at most 50 days, the boosts only once they
// bring more than the best building of their output on their square.
TEST_CASE("large buildings and boosts come once they are worth it", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  constexpr std::uint8_t kFactory = 0;
  constexpr std::uint8_t kLaboratory = 2;
  constexpr std::uint8_t kMegafacility = 6;
  constexpr std::uint8_t kCampus = 8;
  struct Tier {
    std::uint8_t small, large;
    Hand::Output output;
  };
  constexpr std::array<Tier, 2> kTiers{{{kFactory, kMegafacility, Hand::kIndustry}, {kLaboratory, kCampus, Hand::kResearch}}};
  struct Boost {
    std::uint8_t item;
    Hand::Output output;
  };
  constexpr std::array<Boost, 3> kBoosts{{{kItemHyperpowerPlant, Hand::kIndustry},
                                         {kItemInternet, Hand::kResearch},
                                         {kItemFertilizationPlant, Hand::kProsperity}}};
  int larges = 0;
  int smalls = 0;
  for (const int seed : kSeeds) {
    for (const int size : {2, 3}) {
      for (const FinalPreset preset : kPresets) {
        PlanRig rig(*t, seed, size);
        REQUIRE(rig.ready());
        rig.management().final_outputs = planet_final_preset(preset, rig.planet());
        rig.stop_research();
        for (int d = 1; d <= kDays && (rig.days.empty() || rig.days.back().empty > 0); ++d) {
          if (d == 60) {
            rig.learn(kMegafacility);
            rig.learn(kCampus);
          }
          if (d == 150) {
            for (const Boost& b : kBoosts) {
              rig.learn(b.item);
            }
          }
          const std::size_t before = rig.steps.size();
          rig.day();
          for (std::size_t k = before; k < rig.steps.size(); ++k) {
            const BuildStep& s = rig.steps[k];
            if (s.removed != kNoItem) {
              continue;
            }
            const Hand hand(rig, s.square);
            for (const Tier& tier : kTiers) {
              if (d >= 60 && (s.item == tier.small || s.item == tier.large)) {
                CAPTURE(seed, size, static_cast<int>(preset), s.day, t->planitems.items.at(s.item).name);
                CHECK((s.item == tier.large) == hand.large_soon(tier.large, tier.output));
                ++(s.item == tier.large ? larges : smalls);
              }
            }
            for (const Boost& b : kBoosts) {
              if (s.item == b.item) {
                CAPTURE(seed, size, static_cast<int>(preset), s.day, t->planitems.items.at(b.item).name);
                CHECK(hand.boost_worth(s.square, b.item, b.output));
              }
            }
          }
        }
      }
    }
  }
  // Both came, the small ones while the large took too long.
  CHECK(larges > 0);
  CHECK(smalls > 0);
}

// The research toward `gizmos`: each requirement not known yet after its own
// prerequisites (depth first, the tables' order), each item once.
std::vector<std::uint8_t> research_path(const Session& session, std::initializer_list<std::int8_t> gizmos) {
  const Research& r = session.game->research;
  const RaceIndex race = session.player_race;
  std::vector<std::uint8_t> path;
  const auto visit = [&](const auto& self, std::uint8_t item) -> void {
    if (item >= r.count || ((r.items[item].known_mask >> race) & 1) != 0 || std::ranges::find(path, item) != path.end()) {
      return;
    }
    for (const std::uint8_t pre : r.items[item].prereqs) {
      if (pre == 0xff) {
        break;
      }
      self(self, pre);
    }
    path.push_back(item);
  };
  for (const std::int8_t g : gizmos) {
    visit(visit, session.tables->gizmos.at(static_cast<std::size_t>(g)).research_requirement);
  }
  return path;
}

// Orders a small colony ship with a Star Lane Drive at the planet's
// Shipyard, as the player does on the planet screen (`PlanWnd::ship_design`);
// returns the ship, none if the planet cannot build one.
Ship* order_colony_ship(Session& session, Planet& planet) {
  if (!planet_can_build_ship(session, planet)) {
    return nullptr;
  }
  const std::uint16_t square = planet_auto_place_item(session, planet, kItemShip, 0);
  if (square == kNoSquare) {
    return nullptr;
  }
  Ship* ship = race_ai_new_ship(session, session.game->cosmos.races.at(planet.owner), planet);
  REQUIRE(ship != nullptr);
  planet_set_square_ship(session, planet, square, ship_index(session.game->cosmos, *ship));
  ship_set_hull_size(session, *ship, 0);
  const std::int8_t generator = ship_best_gizmo_for_race(session, 4, planet.owner);
  const std::int8_t drive = ship_best_gizmo_for_race(session, 2, planet.owner);
  REQUIRE(generator >= 0);
  REQUIRE(drive >= 0);
  for (const std::int8_t g : {generator, drive, kGizmoStarLaneDrive, kGizmoColonizer, kGizmoColonizer}) {
    REQUIRE(ship_install_gizmo(session, *ship, g, ship->gizmo_count));
  }
  return ship;
}

// The day each seed's home planet launches its first interstellar colony
// ship by: the rules of 2026-11-22 and a twentieth more.
constexpr std::array<int, 3> kShipSeeds{7, 11, 23};
constexpr std::array<int, 3> kShipBy{310, 300, 309};

TEST_CASE("a managed home planet launches its first interstellar colony ship in time", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  constexpr int kLimit = 1000;
  for (std::size_t si = 0; si < kShipSeeds.size(); ++si) {
    const int seed = kShipSeeds[si];
    PlanRig rig(*t, seed, PlanRig::Home{});
    REQUIRE(rig.ready());
    Session& s = rig.session();
    Research& r = s.game->research;
    r.self_managed = 0;
    const std::vector<std::uint8_t> path = research_path(s, {kGizmoStarLaneDrive, kGizmoColonizer});
    Ship* ship = nullptr;
    int launched = 0;
    int known = 0;
    for (int day = 1; day <= kLimit && launched == 0; ++day) {
      const auto next = std::ranges::find_if(
          path, [&](std::uint8_t item) { return ((r.items[item].known_mask >> s.player_race) & 1) == 0; });
      if (next != path.end()) {
        if (r.project[s.player_race] != *next) {
          research_set_project(r, s.player_race, *next);
        }
      } else if (known == 0) {
        known = day;
      }
      if (known != 0 && ship == nullptr) {
        ship = order_colony_ship(s, rig.planet());
      }
      rig.day();
      if (ship != nullptr && ship->location_kind != ShipLocation::kBuilding) {
        launched = day;
      }
    }
    std::printf("A seed %d: research %zu items done day %d, ship launched day %d\n", seed, path.size(), known, launched);
    CAPTURE(seed, known);
    CHECK(launched > 0);
    CHECK(launched <= kShipBy[si]);
  }
}


// The buildings of `item` on the planet's surface.
int count_items(PlanRig& rig, std::uint8_t item) {
  int n = 0;
  for (const Square& sq : planet_squares(rig.session().game->cosmos, rig.planet()).first(rig.planet().surface_squares)) {
    n += sq.item == item ? 1 : 0;
  }
  return n;
}

constexpr std::uint8_t kMetroplex = 4;
constexpr std::uint8_t kOutpostItem = 20;
constexpr std::uint8_t kSurfaceShieldItem = 18;
constexpr std::uint8_t kSurfaceMegaShieldItem = 19;

TEST_CASE("the final layout houses in Metroplexes once known, for their output", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  PlanetManagement m = planet_management(rig.session(), rig.planet());
  m.final_outputs = planet_final_preset(FinalPreset::kDock, rig.planet());
  CHECK(planet_final_layout(rig.session(), rig.planet(), m).house != kMetroplex);
  rig.learn(kMetroplex);
  CHECK(planet_final_layout(rig.session(), rig.planet(), m).house == kMetroplex);
}

TEST_CASE("the final layout counts black squares once Terraforming is known", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 11, 3);
  REQUIRE(rig.ready());
  int black = 0;
  for (const Square& sq : planet_squares(rig.session().game->cosmos, rig.planet()).first(rig.planet().surface_squares)) {
    black += (sq.color & kSquareRuins) == 0 && (sq.color & 7u) == 0 && sq.item == kNoItem ? 1 : 0;
  }
  REQUIRE(black > 0);
  const PlanetManagement m = planet_management(rig.session(), rig.planet());
  const auto laid_out = [&] {
    const FinalLayout f = planet_final_layout(rig.session(), rig.planet(), m);
    return f.squares[0] + f.squares[1] + f.squares[2] + f.housing + f.open;
  };
  const int before = laid_out();
  rig.learn(kItemTerraforming);
  CHECK(laid_out() == before + black);
}

TEST_CASE("a minimum takes at most half the squares while research is maximised", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  PlanetManagement m = planet_management(rig.session(), rig.planet());
  m.final_outputs = planet_final_preset(FinalPreset::kResearch, rig.planet());
  m.final_outputs[PlanetManagement::kIndustry].value = 100;
  const FinalLayout f = planet_final_layout(rig.session(), rig.planet(), m);
  const int squares = f.squares[0] + f.squares[1] + f.squares[2] + f.open;
  CAPTURE(f.squares[0], f.squares[1], squares);
  CHECK(f.squares[PlanetManagement::kIndustry] == (squares + 1) / 2);
  CHECK(f.squares[PlanetManagement::kResearch] == squares - (squares + 1) / 2);
}

TEST_CASE("a full colony moves its inhabitants from Outposts into Metroplexes once known", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  rig.management().final_outputs = planet_final_preset(FinalPreset::kDock, rig.planet());
  full_colony(rig);
  const int outposts = count_items(rig, kOutpostItem);
  REQUIRE(outposts > 0);
  rig.learn(kMetroplex);
  rig.run(600);
  CAPTURE(outposts, count_items(rig, kOutpostItem), count_items(rig, kMetroplex));
  CHECK(count_items(rig, kOutpostItem) == 0);
  CHECK(count_items(rig, kMetroplex) > 0);
  CHECK(at_final_layout(rig));
}

TEST_CASE("a full colony without a free worker set to dig first digs soon once the dig is known", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 1);
  REQUIRE(rig.ready());
  // Ruins on the last empty usable square.
  const std::span<Square> sq = planet_squares(rig.session().game->cosmos, rig.planet());
  std::uint16_t ruins = kNoSquare;
  for (std::uint16_t i = 0; i < rig.planet().surface_squares; ++i) {
    ruins = sq[i].item == kNoItem && (sq[i].color & 7u) != 0 ? i : ruins;
  }
  REQUIRE(ruins != kNoSquare);
  sq[ruins].color |= kSquareRuins;
  rig.management().levels[PlanetManagement::kDig] = BuildLevel::kFirst;
  full_colony(rig);
  // No worker free and no growth (as a research colony grows full).
  REQUIRE(planet_prosperity_tenths(rig.planet()) == 0);
  rig.planet().population = rig.planet().used_population;
  rig.learn(kItemXenoDig);
  const int known = rig.days.back().day;
  rig.run(60);
  int dug = 0;
  for (const BuildStep& s : rig.steps) {
    dug = dug == 0 && s.item == kItemXenoDig ? s.day : dug;
  }
  CAPTURE(known, dug);
  CHECK(dug > known);
  CHECK(dug <= known + 30);
}

TEST_CASE("a built-up colony maximising industry without a Shipyard takes over the scientists", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  rig.management().final_outputs = planet_final_preset(FinalPreset::kShipyard, rig.planet());
  full_colony(rig);
  REQUIRE(planet_find_item(rig.session().game->cosmos, rig.planet(), kItemShipyard) == kNoSquare);
  rig.learn(kItemScientistTakeover);
  rig.run(200);
  CHECK(rig.planet().project_item == kItemScientistTakeover);
}

TEST_CASE("the balanced preset maximises industry and research alike, no prosperity minimum", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  constexpr FinalOutput kMax{FinalMode::kMaximise, 0, kFinalWeights};
  CHECK(planet_final_preset(FinalPreset::kBalanced, rig.planet()) ==
        std::array{kMax, kMax, FinalOutput{FinalMode::kAtLeast, 0, kFinalWeights}});
}

TEST_CASE("an AI colony builds no more Surface Shields than its management wants", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  // `ai_plan`'s Surface Shields by planet size.
  constexpr std::array<int, 5> kShields{2, 3, 4, 6, 8};
  for (const int seed : {7, 23}) {
    PlanRig rig(*t, seed, 2);
    REQUIRE(rig.ready());
    rig.run(900);
    const Cosmos& c = rig.session().game->cosmos;
    for (PlanetIndex i = 0; i < c.planet_count; ++i) {
      const Planet& p = c.planets[static_cast<std::size_t>(i)];
      if (p.owner >= c.race_count || p.owner == rig.session().player_race) {
        continue;
      }
      int shields = 0;
      for (const Square& sq : planet_squares(c, p).first(p.surface_squares)) {
        shields += sq.item == kSurfaceShieldItem || sq.item == kSurfaceMegaShieldItem ? 1 : 0;
      }
      CAPTURE(seed, i, p.size);
      CHECK(shields <= kShields.at(std::min<std::size_t>(p.size, 4)));
    }
  }
}

TEST_CASE("under a lingering danger an AI colony builds orbital defence beyond its counts, as the player's", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  constexpr std::uint8_t kOrbitalShieldItem = 26;
  PlanRig rig(*t, 7, 2);
  REQUIRE(rig.ready());
  rig.run(300);
  Session& session = rig.session();
  Cosmos& c = session.game->cosmos;
  // An AI colony with orbit to spare, its orbital defence of `ai_plan`'s
  // counts (weapons 1 / 1 / 2 / 3 / 4, shields 1 / 2 / 2 / 3 / 4) built.
  Planet* planet = nullptr;
  for (PlanetIndex i = 0; i < c.planet_count && planet == nullptr; ++i) {
    Planet& p = c.planets[static_cast<std::size_t>(i)];
    if (p.owner < c.race_count && p.owner != session.player_race && p.industry > 0 &&
        p.square_count - p.surface_squares >= 8) {
      planet = &p;
    }
  }
  REQUIRE(planet != nullptr);
  Race& race = c.races.at(planet->owner);
  for (const std::uint8_t item : {kItemMissileBase, kOrbitalShieldItem}) {
    const std::uint8_t req = t->planitems.items.at(item).research_requirement;
    session.game->research.items.at(req).known_mask |= static_cast<std::uint8_t>(1u << race.index);
  }
  // Builds `item` on a free orbital square at once.
  const auto build = [&](std::uint8_t item) {
    for (Square& q : planet_squares(c, *planet).subspan(planet->surface_squares)) {
      if (q.item == kNoItem) {
        q.item = item;
        q.flags = kSquareBuilt;
        planet_recalc_output(session, *planet);
        return true;
      }
    }
    return false;
  };
  const std::size_t size = std::min<std::size_t>(planet->size, 4);
  for (int k = 0; k < std::array{1, 1, 2, 3, 4}[size]; ++k) {
    REQUIRE(build(kItemMissileBase));
  }
  for (int k = 0; k < std::array{1, 2, 2, 3, 4}[size]; ++k) {
    REQUIRE(build(kOrbitalShieldItem));
  }
  // A free worker for one: buildings without housing taken down.
  for (Square& q : planet_squares(c, *planet).first(planet->surface_squares)) {
    if (planet->population > planet->used_population) {
      break;
    }
    const PlanItem& it = t->planitems.items.at(q.item == kNoItem ? 0 : q.item);
    if (q.item != kNoItem && (q.flags & kSquareBuilt) != 0 && it.used_population > 0 && it.max_population == 0) {
      q.item = kNoItem;
      q.flags = 0;
      planet_recalc_output(session, *planet);
    }
  }
  REQUIRE(planet->population > planet->used_population);
  // A ship of the player (in a free slot), at war with it, lingering at
  // its star.
  ShipIndex ship = -1;
  for (std::size_t k = 0; k < c.ships.size() && ship < 0; ++k) {
    if (c.ships[k].owner < 0) {
      ship = static_cast<ShipIndex>(k);
    }
  }
  REQUIRE(ship >= 0);
  Ship& s = c.ships[static_cast<std::size_t>(ship)];
  s.owner = static_cast<std::int16_t>(session.player_race);
  ++c.ship_count;
  s.location_kind = ShipLocation::kSystem;
  s.location = planet->star;
  ship_enter_star_system(session, c.stars[static_cast<std::size_t>(planet->star)], s);
  session.ship_arrival_star[static_cast<std::size_t>(ship)] = planet->star;
  session.ship_arrival_day[static_cast<std::size_t>(ship)] = c.day - 30;
  race.relation[static_cast<std::size_t>(session.player_race)] = kWar;
  REQUIRE(pe_danger_at(session, race, *planet));
  if (planet->project_item != kNoItem) {
    planet->project_item = kNoItem;
    planet->project_square = kNoSquare;
  }
  pe_manage_planet(session, race, *planet);
  CHECK((planet->project_item == kItemMissileBase || planet->project_item == kOrbitalShieldItem));
}

TEST_CASE("a built-up colony swaps a factory on blue and a laboratory on red back", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  constexpr std::uint8_t kFactory = 0;
  constexpr std::uint8_t kLaboratory = 2;
  PlanRig rig(*t, 7, 3);
  REQUIRE(rig.ready());
  rig.management().final_outputs = planet_final_preset(FinalPreset::kBalanced, rig.planet());
  full_colony(rig);
  Session& session = rig.session();
  const std::span<Square> sq = planet_squares(session.game->cosmos, rig.planet());
  std::uint16_t red = kNoSquare;
  std::uint16_t blue = kNoSquare;
  for (std::uint16_t i = 0; i < rig.planet().surface_squares; ++i) {
    red = red == kNoSquare && (sq[i].color & 7u) == 2 && sq[i].item == kFactory ? i : red;
    blue = blue == kNoSquare && (sq[i].color & 7u) == 4 && sq[i].item == kLaboratory ? i : blue;
  }
  REQUIRE(red != kNoSquare);
  REQUIRE(blue != kNoSquare);
  std::swap(sq[red].item, sq[blue].item);
  planet_recalc_output(session, rig.planet());
  rig.run(150);
  CAPTURE(red, blue, sq[red].item, sq[blue].item);
  CHECK(t->planitems.items.at(sq[red].item).industry > 0);
  CHECK(t->planitems.items.at(sq[blue].item).research > 0);
}

TEST_CASE("a colony short of room automates cheap buildings in place of housing", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 3);
  REQUIRE(rig.ready());
  rig.management().final_outputs = planet_final_preset(FinalPreset::kBalanced, rig.planet());
  rig.stop_research();
  rig.learn(kItemAutomation);
  rig.run(400);
  int automated = 0;
  for (const Square& sq : planet_squares(rig.session().game->cosmos, rig.planet())) {
    automated += (sq.flags & kSquareAutomated) != 0 ? 1 : 0;
  }
  CHECK(automated > 0);
}

}  // namespace
}  // namespace oa::game
