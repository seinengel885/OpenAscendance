// A peCustom game with one new colony of the player under self-management
// (`game/planet_plan.h`), run day by day, recording what the planet builds:
// the rig of the planet management's scenario tests.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "game/cosmos.h"
#include "game/new_game.h"
#include "game/planet.h"
#include "game/planet_plan.h"
#include "game/session.h"
#include "game/tables.h"

namespace oa::game::plan_test {

// A square's building changed on a day: `item` built there (the project
// placed on it), `removed` the building that was there (`kNoItem` on an
// empty square).
struct BuildStep {
  int day = 0;
  std::uint16_t square = 0;
  std::uint8_t item = kNoItem;
  std::uint8_t removed = kNoItem;
};

// One day of the planet.
struct DayRecord {
  int day = 0;
  std::int32_t industry = 0;
  std::int32_t research = 0;
  std::int32_t prosperity = 0;  // in tenths
  std::int32_t population = 0;
  std::int32_t free_workers = 0;
  bool project = false;  // a project at the day's end
  std::int32_t open = 0;  // empty usable squares a building may go on now
  std::int32_t empty = 0;  // empty usable squares
};

class PlanRig {
 public:
  // A new game (`seed`), the player's colony on an unowned planet of
  // `size` with most of its surface not black (Colony Base, two inhabitants) with the planet's
  // standard management; the player's other planets are not managed.
  PlanRig(const GameTables& tables, int seed, int size) : session_(tables) {
    start(seed);
    Cosmos& c = session_.game->cosmos;
    for (PlanetIndex i = 0; i < c.planet_count; ++i) {
      Planet& p = c.planets[static_cast<std::size_t>(i)];
      if (p.owner == kNoOwner && p.size == size && habitable(p)) {
        planet_ = &p;
        index_ = i;
        break;
      }
    }
    if (planet_ == nullptr) {
      return;
    }
    Planet& p = *planet_;
    p.owner = static_cast<std::uint8_t>(session_.player_race);
    c.stars[static_cast<std::size_t>(p.star)].owner_mask |= static_cast<std::uint8_t>(1u << session_.player_race);
    p.population = 2;
    planet_auto_build_item(session_, p, kItemColonyBase);
    planet_recalc_output(session_, p);
    manage(PlanetManagement{});
  }
  // A new game (`seed`), the player's home planet managed with the
  // standard management of a first planet (Shipyard first).
  struct Home {};
  PlanRig(const GameTables& tables, int seed, Home) : session_(tables) {
    start(seed);
    Cosmos& c = session_.game->cosmos;
    for (PlanetIndex i = 0; i < c.planet_count && planet_ == nullptr; ++i) {
      if (c.planets[static_cast<std::size_t>(i)].owner == session_.player_race) {
        planet_ = &c.planets[static_cast<std::size_t>(i)];
        index_ = i;
      }
    }
    if (planet_ != nullptr) {
      manage(planet_management_standard(session_, *planet_));
    }
  }

  // Whether most of `p`'s surface is not black.
  bool habitable(const Planet& p) const {
    const std::span<const Square> sq = planet_squares(session_.game->cosmos, p).first(p.surface_squares);
    return 2 * std::ranges::count_if(sq, [](const Square& s) { return (s.color & 7u) != 0; }) > p.surface_squares;
  }

  bool ready() const { return planet_ != nullptr; }
  Session& session() { return session_; }
  Planet& planet() { return *planet_; }
  PlanetIndex index() const { return index_; }
  PlanetManagement& management() { return session_.game->planet_management[static_cast<std::size_t>(index_)]; }

  // One day of the game, recorded.
  void day() {
    Cosmos& c = session_.game->cosmos;
    const std::span<const Square> now = planet_squares(c, *planet_);
    const std::vector<Square> before(now.begin(), now.begin() + planet_->surface_squares);
    cosmos_next_turn(session_);
    session_.events.take();
    ++day_;
    const std::span<const Square> after = planet_squares(c, *planet_);
    for (std::uint16_t i = 0; i < planet_->surface_squares; ++i) {
      if (after[i].item != before[i].item && after[i].item != kNoItem) {
        steps.push_back({day_, i, after[i].item, (before[i].flags & kSquareBuilt) != 0 ? before[i].item : kNoItem});
      }
    }
    DayRecord r;
    r.day = day_;
    r.industry = static_cast<std::uint16_t>(planet_->industry);
    r.research = static_cast<std::uint16_t>(planet_->research);
    r.prosperity = planet_prosperity_tenths(*planet_);
    r.population = planet_->population;
    r.free_workers = planet_->population - planet_->used_population;
    r.project = planet_->project_item != kNoItem;
    for (std::uint16_t i = 0; i < planet_->surface_squares; ++i) {
      if (usable_[i] && after[i].item == kNoItem) {
        ++r.empty;
        r.open += planet_can_place_item(session_, *planet_, 0, i) ? 1 : 0;
      }
    }
    days.push_back(r);
  }
  void run(int n) {
    for (int i = 0; i < n; ++i) {
      day();
    }
  }

  // The player researches nothing more.
  void stop_research() {
    session_.game->research.self_managed = 0;
    session_.game->research.project[session_.player_race] = -1;
  }
  // The player knows `item` (its research requirement).
  void learn(std::uint8_t item) {
    Research& r = session_.game->research;
    const std::uint8_t req = session_.tables->planitems.items.at(item).research_requirement;
    if (req < r.count) {
      r.items[req].known_mask |= static_cast<std::uint8_t>(1u << session_.player_race);
    }
  }
  // Runs until the planet is full (at most `limit` days in all).
  void run_until_full(int limit) {
    while ((days.empty() || days.back().empty > 0) && day_ < limit) {
      day();
    }
  }

  // The usable surface squares (not ruins; black only for Orfa).
  int usable_squares() const { return static_cast<int>(std::ranges::count(usable_, true)); }

  // The first day without an empty usable square, none before.
  std::optional<int> full_day() const {
    for (const DayRecord& r : days) {
      if (r.empty == 0) {
        return r.day;
      }
    }
    return std::nullopt;
  }

  std::vector<BuildStep> steps;
  std::vector<DayRecord> days;

 private:
  // The new game, none of the player's planets managed.
  void start(int seed) {
    session_.game->variant = Variant::kPeCustom;
    const CosmosTxt txt{.stars = 50, .races = 7, .seed = seed, .species = {}};
    program_start(session_, 0, &txt, false);
    NewGameSetup setup;
    setup.activate(session_);
    setup.begin(session_);
    session_.window_state.assign(kWindowStateSize, 0);
    Cosmos& c = session_.game->cosmos;
    session_.game->research.self_managed = -1;
    for (PlanetIndex i = 0; i < c.planet_count; ++i) {
      Planet& p = c.planets[static_cast<std::size_t>(i)];
      if (p.owner == session_.player_race) {
        p.self_managed = 0;
      }
    }
  }
  // Puts the planet under `management`, notes its usable squares.
  void manage(const PlanetManagement& management) {
    Planet& p = *planet_;
    p.self_managed = -1;
    session_.game->planet_management[static_cast<std::size_t>(index_)] = management;
    usable_.assign(p.surface_squares, false);
    const bool black = session_.game->cosmos.races.at(p.owner).ability == 2;
    const std::span<const Square> sq = planet_squares(session_.game->cosmos, p);
    for (std::uint16_t i = 0; i < p.surface_squares; ++i) {
      usable_[i] = (sq[i].color & kSquareRuins) == 0 && ((sq[i].color & 7u) != 0 || black);
    }
  }

  Session session_;
  Planet* planet_ = nullptr;
  PlanetIndex index_ = 0;
  int day_ = 0;
  std::vector<bool> usable_;
};

}  // namespace oa::game::plan_test
