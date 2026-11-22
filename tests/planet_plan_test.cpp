// The planet management's rules (`game/planet_plan_rules.cpp`) on a new
// colony of each size, under each strategy: the scenario tests of
// `plan_rig.h`.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cstdio>
#include <format>
#include <map>
#include <vector>

#include "game/planet_final.h"
#include "game_oracle.h"
#include "plan_rig.h"

namespace oa::game {
namespace {

using plan_test::BuildStep;
using plan_test::DayRecord;
using plan_test::PlanRig;

constexpr int kDays = 600;
constexpr std::array<int, 2> kSeeds{7, 11};
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
  // Early research focus fills slower (Patrick takes that).
  if (const int by = kFullBy.at(rig.planet().size);
      planet_goal(rig.session(), rig.planet()) != PlanetGoal::kEarlyResearch && (!full || *full > by)) {
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
      for (const PlanetGoal goal : kPlanetGoals) {
        PlanRig rig(*t, seed, size, goal);
        if (!rig.ready()) {
          continue;
        }
        rig.run(kDays);
        const std::optional<int> full = rig.full_day();
        std::printf("C seed %d size %d goal %d squares %d full %d pop %d I %d R %d\n", seed, size,
                    static_cast<int>(goal), rig.planet().surface_squares, full.value_or(-1),
                    rig.days.back().population, rig.days.back().industry, rig.days.back().research);
        CAPTURE(seed, size, static_cast<int>(goal));
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

TEST_CASE("early research focus has research sooner, early industry focus industry", "[pecustom][plan]") {
  OA_REQUIRE_TABLES(t);
  PlanRig industry(*t, 7, 3, PlanetGoal::kEarlyIndustry);
  PlanRig research(*t, 7, 3, PlanetGoal::kEarlyResearch);
  REQUIRE(industry.ready());
  industry.run(400);
  research.run(400);
  for (std::size_t d = 24; d < 400; d += 25) {
    std::printf("F day %d  early industry I %d R %d  early research I %d R %d\n", industry.days[d].day,
                industry.days[d].industry, industry.days[d].research, research.days[d].industry,
                research.days[d].research);
  }
  // The early phase: half the days early industry focus takes to be full.
  const int early = industry.full_day().value_or(400) / 2;
  const auto [ii, ir] = sums(industry, early);
  const auto [ri, rr] = sums(research, early);
  CAPTURE(early, ii, ir, ri, rr);
  CHECK(rr > ir);
  CHECK(ii > ri);
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
      PlanRig rig(*t, 7, size, PlanetGoal::kBalanced);
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
  PlanRig rig(*t, 7, 2, PlanetGoal::kBalanced);
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
  PlanRig rig(*t, 7, 2, PlanetGoal::kBalanced);
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
  PlanRig rig(*t, 7, 2, PlanetGoal::kBalanced);
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
  PlanRig rig(*t, 7, 2, PlanetGoal::kBalanced);
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

}  // namespace
}  // namespace oa::game
