// The planet management's rules (`game/planet_plan_rules.cpp`) on a new
// colony of each size, under each strategy: the scenario tests of
// `plan_rig.h`.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <map>
#include <vector>

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
// (seeds 7, 11, all strategies) and a tenth more, at most `kDays`.
constexpr std::array<int, 5> kFullBy{120, 155, 335, 370, kDays};

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

TEST_CASE("a managed colony builds without idling, early demolition or ping-pong", "[pecustom][plan][!mayfail]") {
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

}  // namespace
}  // namespace oa::game
