#include "game/planet_plan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "game/address.h"
#include "game/antag_planet.h"
#include "game/planet.h"
#include "game/planet_plan_sim.h"
#include "game/tables.h"

namespace oa::game {

namespace {

using namespace plan_detail;

bool is_player_planet(const Session& session, const Planet& planet) {
  return planet.owner == session.player_race && !session.autoplay;
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

// Whether `planet` is its owner's first planet: the oldest of its home
// star.
bool first_planet(const Session& session, const Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  bool first = planet.owner < cosmos.race_count && planet.star == cosmos.races.at(planet.owner).home;
  for (PlanetIndex i = 0; first && i < cosmos.planet_count; ++i) {
    const Planet& other = cosmos.planets[static_cast<std::size_t>(i)];
    first = other.owner != planet.owner || other.star != planet.star || other.acquired_day > planet.acquired_day ||
            (other.acquired_day == planet.acquired_day && &other >= &planet);
  }
  return first;
}

struct AiPlan {
  PlanetGoal goal;
  PlanetManagement management;
};

// Hops from `star` to the nearest star that is not `race`'s alone (one with
// no colony, or another's).
std::int32_t frontier_hops(const Cosmos& cosmos, const Race& race, StarIndex star) {
  const auto bit = static_cast<std::uint8_t>(1u << (race.index & 0x1f));
  std::int32_t best = 0xff;
  for (StarIndex s = 0; s < cosmos.star_count; ++s) {
    if ((cosmos.stars[static_cast<std::size_t>(s)].owner_mask & 0x7f) != bit) {
      best = std::min<std::int32_t>(best, cosmos.hops[static_cast<std::size_t>(star)][static_cast<std::size_t>(s)]);
    }
  }
  return best;
}

// The AI's planets for Shipyards: one, and one more per four planets, of
// the planets of size small up the best by industry, nearer the frontier
// first (a species deep in a dead end builds them toward its way out).
std::vector<PlanetIndex> ai_shipyard_planets(const Session& session, const Race& race) {
  constexpr std::int32_t kHopWeight = 6;
  const Cosmos& cosmos = session.game->cosmos;
  std::vector<std::pair<std::int32_t, PlanetIndex>> ranked;
  std::int32_t planets = 0;
  for (PlanetIndex i = 0; i < cosmos.planet_count; ++i) {
    const Planet& p = cosmos.planets[static_cast<std::size_t>(i)];
    if (p.owner != race.index) {
      continue;
    }
    ++planets;
    if (p.size >= 1) {
      const std::int32_t score =
          u16(p.industry) + 2 * p.size - kHopWeight * frontier_hops(cosmos, race, p.star);
      ranked.emplace_back(-score, i);
    }
  }
  std::ranges::sort(ranked);
  const std::size_t n = std::min<std::size_t>(ranked.size(), static_cast<std::size_t>(1 + planets / 4));
  std::vector<PlanetIndex> out;
  for (std::size_t k = 0; k < n; ++k) {
    out.push_back(ranked[k].second);
  }
  return out;
}

// The AI's strategy and management of `planet` (`race` its owner): a
// Shipyard along on its Shipyard planets (first while it has none) (`ai_shipyard_planets`), which
// build for industry; its first planet balanced, for research when the
// Shipyards are elsewhere; the others by their squares' colours (red: industry, blue:
// research, else balanced); defences first under a lingering danger.
AiPlan ai_plan(const Session& session, const Race& race, const Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  const auto index = planet_index(cosmos, planet);
  AiPlan plan{PlanetGoal::kBalanced, planet_management_standard(session, planet)};
  const std::vector<PlanetIndex> yards = ai_shipyard_planets(session, race);
  const bool yard = std::ranges::find(yards, index) != yards.end();
  // The species' first Shipyard comes first: its ships colonize.
  const bool none = std::ranges::none_of(yards, [&](PlanetIndex i) {
    return planet_find_item(cosmos, cosmos.planets[static_cast<std::size_t>(i)], kItemShipyard) != kNoSquare;
  });
  plan.management.levels[PlanetManagement::kShipyard] =
      !yard ? BuildLevel::kNo : none ? BuildLevel::kFirst : BuildLevel::kAlong;
  if (pe_danger_at(session, race, planet)) {
    plan.management.levels[PlanetManagement::kPlanetaryDefence] = BuildLevel::kFirst;
    plan.management.levels[PlanetManagement::kOrbitalDefence] = BuildLevel::kFirst;
  }
  std::int32_t red = 0;
  std::int32_t blue = 0;
  for (const Square& sq : planet_squares(cosmos, planet).first(planet.surface_squares)) {
    red += sq.color == 2 ? 1 : 0;
    blue += sq.color == 4 ? 1 : 0;
  }
  const bool first = first_planet(session, planet);
  if (first) {
    // Balanced, or research when the Shipyards are elsewhere.
    plan.goal = yard || yards.empty() ? PlanetGoal::kBalanced : PlanetGoal::kResearch;
  } else if (yard) {
    plan.goal = PlanetGoal::kIndustry;
  } else if (2 * red > 3 * blue) {
    plan.goal = PlanetGoal::kIndustry;
  } else if (2 * blue > 3 * red) {
    plan.goal = PlanetGoal::kResearch;
  }
  return plan;
}

}  // namespace

std::optional<PlanetGoal> planet_next_strategy(std::optional<PlanetGoal> current, int delta) {
  const int n = static_cast<int>(kPlanetGoals.size()) + 1;
  const int at = current ? static_cast<int>(std::ranges::find(kPlanetGoals, *current) - kPlanetGoals.begin()) + 1 : 0;
  const int next = ((at + delta) % n + n) % n;
  return next == 0 ? std::nullopt : std::optional(kPlanetGoals[static_cast<std::size_t>(next - 1)]);
}

bool planet_has_ruins(const Session& session, const Planet& planet) {
  return ruins_square(session, planet) != kNoSquare;
}

std::string_view planet_goal_name(PlanetGoal goal) {
  switch (goal) {
    case PlanetGoal::kBalanced:
      return "Balanced";
    case PlanetGoal::kIndustry:
      return "Industry";
    case PlanetGoal::kResearch:
      return "Research";
  }
  return "Balanced";
}

PlanetManagement planet_management_standard(const Session& session, const Planet& planet) {
  constexpr std::array<std::int16_t, 5> kShields{2, 3, 4, 6, 8};
  constexpr std::array<std::int16_t, 5> kIndustry{3, 4, 5, 6, 8};
  constexpr std::array<std::int16_t, 5> kOrbitalWeapons{1, 1, 2, 3, 4};
  constexpr std::array<std::int16_t, 5> kOrbitalShields{1, 2, 2, 3, 4};
  const std::size_t size = std::min<std::size_t>(planet.size, 4);
  const bool first = first_planet(session, planet);
  PlanetManagement m;
  m.levels = {BuildLevel::kAlong, BuildLevel::kAlong, first ? BuildLevel::kFirst : BuildLevel::kNo, BuildLevel::kNo,
              ruins_square(session, planet) != kNoSquare ? BuildLevel::kAlong : BuildLevel::kNo};
  m.counts = {kShields[size], 0, 0, 0, kOrbitalWeapons[size], 0, kOrbitalShields[size]};
  m.min_industry = kIndustry[size];
  m.min_research = 0;
  m.growth_days = 0;
  m.research_boost = 0;
  return m;
}

std::int32_t planet_orbital_defence_room(const Session& session, const Planet& planet,
                                         const PlanetManagement& management) {
  const Cosmos& cosmos = session.game->cosmos;
  const bool yard = planet_find_item(cosmos, planet, kItemShipyard) != kNoSquare ||
                    management.levels[PlanetManagement::kShipyard] != BuildLevel::kNo;
  const bool dock = planet_find_item(cosmos, planet, plan_detail::kOrbitalDocks) != kNoSquare ||
                    management.levels[PlanetManagement::kOrbitalDock] != BuildLevel::kNo;
  const std::int32_t orbit = planet.square_count - planet.surface_squares;
  return std::max(0, orbit - (yard ? 1 : 0) - (dock ? 1 : 0) - (yard || dock ? 1 : 0));
}

PlanetManagement planet_management(const Session& session, const Planet& planet) {
  return planet_management_resolved(
      session.game->planet_management.at(static_cast<std::size_t>(planet_index(session.game->cosmos, planet))),
      planet_management_standard(session, planet));
}

PlanetManagement planet_management_resolved(PlanetManagement m, const PlanetManagement& standard) {
  for (std::size_t i = 0; i < m.levels.size(); ++i) {
    m.levels[i] = m.levels[i] == BuildLevel::kStandard ? standard.levels[i] : m.levels[i];
  }
  for (std::size_t i = 0; i < m.counts.size(); ++i) {
    m.counts[i] = m.counts[i] < 0 ? standard.counts[i] : m.counts[i];
  }
  m.min_industry = m.min_industry < 0 ? standard.min_industry : m.min_industry;
  m.min_research = m.min_research < 0 ? standard.min_research : m.min_research;
  m.growth_days = m.growth_days < 0 ? standard.growth_days : m.growth_days;
  m.research_boost = m.research_boost < 0 ? standard.research_boost : m.research_boost;
  return m;
}

PlanetAlarm planet_alarm(const Session& session, const Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  if (planet.owner >= cosmos.race_count) {
    return PlanetAlarm::kNone;
  }
  if (pe_danger_at(session, cosmos.races.at(planet.owner), planet)) {
    return PlanetAlarm::kDanger;
  }
  const std::span<const Square> squares = planet_squares(cosmos, planet);
  const bool empty = std::ranges::any_of(squares.first(planet.surface_squares), [](const Square& sq) {
    return sq.item == kNoItem && sq.color != 0 && (sq.color & kSquareRuins) == 0;
  });
  if (empty && planet.prosperity <= 0 && planet.population < planet.max_population &&
      planet.population <= planet.used_population) {
    return PlanetAlarm::kGrowthStopped;
  }
  if (planet.self_managed == 0 && planet.project_item == kNoItem && planet.population > planet.used_population) {
    return PlanetAlarm::kIdle;
  }
  return PlanetAlarm::kNone;
}

std::string_view planet_alarm_text(PlanetAlarm alarm) {
  switch (alarm) {
    case PlanetAlarm::kDanger:
      return "danger at the star";
    case PlanetAlarm::kGrowthStopped:
      return "growth stopped";
    case PlanetAlarm::kIdle:
      return "no project, free workers";
    case PlanetAlarm::kNone:
      break;
  }
  return "";
}

PlanetGoal planet_goal(const Session& session, const Planet& planet) {
  if (!is_player_planet(session, planet)) {
    return PlanetGoal::kBalanced;
  }
  return session.game->planet_goals.at(static_cast<std::size_t>(planet_index(session.game->cosmos, planet)));
}

void pe_manage_planet(Session& session, Race& race, Planet& planet) {
  if (planet.industry == 0) {
    return;
  }
  if (!is_player_planet(session, planet)) {
    // The AI: the Antagonizer's specials, the rest by the rules for the
    // strategy and management it picks.
    if (antag_race_ai_planet_specials(session, race, planet)) {
      return;
    }
    const AiPlan plan = ai_plan(session, race, planet);
    pe_manage_planet_rules(session, planet, plan.goal, plan.management);
    return;
  }
  // Under a lingering danger the orbital defences come first (unless the
  // planet's management never wants them).
  const PlanetManagement management = planet_management(session, planet);
  if (management.levels[PlanetManagement::kOrbitalDefence] != BuildLevel::kNo && pe_danger_at(session, race, planet) &&
      pe_build_orbital_defence(session, race, planet)) {
    return;
  }
  pe_manage_planet_rules(session, planet, planet_goal(session, planet), management);
}

bool pe_take_project_bell(Session& session, const Planet& planet) {
  std::uint8_t& bell =
      session.game->planet_management.at(static_cast<std::size_t>(planet_index(session.game->cosmos, planet))).bell;
  return std::exchange(bell, std::uint8_t{0}) != 0;
}

void pe_manage_planet_now(Session& session, Planet& planet) {
  if (session.game->variant == Variant::kPeCustom && is_player_planet(session, planet) && planet.self_managed != 0 &&
      planet.project_item == kNoItem) {
    pe_manage_planet(session, session.game->cosmos.races.at(planet.owner), planet);
  }
}

}  // namespace oa::game
