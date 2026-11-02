// peCustom's planet management (this fork's own, not in the original): the
// project of an idle planet the AI runs or the player lets manage itself,
// chosen for the planet's goal (`PlanetGoal`, `Game::planet_goals`). No
// building comes for free.
//
// The economy is planned ahead on a copy of the planet played forward day by
// day with the game's own rules (`planet_recalc_output`,
// `planet_can_place_item`: colour bonus, adjacency, workers, growth toward
// the next inhabitant). In this order:
// - The workforce rule: while empty squares wait for more workers than the
//   planet has or will have, its last free worker builds housing in time,
//   and growth is restarted when one building can (else the planner
//   decides); without a worker the least useful building makes way.
// - While new buildings are worth it (empty squares first), each candidate
//   (an item on the square of a colour that suits it) is played forward
//   150 days, the next projects chosen by a quick estimate, and the one
//   yielding the most of the goal's outputs wins: growth first or output
//   first is decided for the planet as it is.
// - Only then (no empty square for a worthwhile building, or no worker and
//   none to come) buildings are replaced: the least useful ones (for the
//   goal, after a change of goal first those it does not want), each by the
//   best building by the quick estimate if it gains more than the outputs
//   lost while rebuilding (so buildings never replace each other back and
//   forth).
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// The goals in the order the menus list them.
inline constexpr std::array<PlanetGoal, kPlanetGoalCount> kPlanetGoals{
    PlanetGoal::kBalanced,   PlanetGoal::kIndustry,    PlanetGoal::kResearch,   PlanetGoal::kDefence,
    PlanetGoal::kShipyard,   PlanetGoal::kArchaeology, PlanetGoal::kBalancedV2, PlanetGoal::kIndustryV2,
    PlanetGoal::kResearchV2};

// The goal's English name (the remake text's key).
std::string_view planet_goal_name(PlanetGoal goal);

// Whether `goal` can be chosen for `planet`: the Shipyard goal while the
// planet has no Shipyard and its owner can build one, Archaeology while it
// has unexcavated ruins.
bool planet_goal_available(const Session& session, const Planet& planet, PlanetGoal goal);

// The goal `planet`'s self-management follows: the player's choice (the
// balanced goal when that is no longer available), or the AI's own.
PlanetGoal planet_goal(const Session& session, const Planet& planet);

// Whether `goal` is one of the rule-based planner's (`planet_plan_v2.cpp`).
bool planet_goal_v2(PlanetGoal goal);

// The rule-based planner (`planet_plan_v2.cpp`): starts the next project of
// an idle planet managed for the V2 goal `goal`.
void pe_manage_planet_v2(Session& session, Planet& planet, PlanetGoal goal);

// peCustom: chooses the project of an idle planet that the AI runs or that
// manages itself (`Planet::project_item` none). The Shipyard and Archaeology
// goals hand the planet back to the player (self-management off) once their
// project has started, so the player hears of its completion.
void pe_manage_planet(Session& session, Race& race, Planet& planet);

}  // namespace oa::game
