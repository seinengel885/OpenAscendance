// peCustom's planet management (this fork's own, not in the original): the
// project of an idle planet the AI runs or the player lets manage itself,
// chosen for the planet's goal (`PlanetGoal`, `Game::planet_goals`).
//
// The economy is planned ahead: the planner copies the planet and plays its
// days forward with the game's own rules (`planet_recalc_output`,
// `planet_can_place_item`: colour bonus, adjacency, population, growth). Each
// candidate first project (an item on a square, possibly replacing a
// building) is followed by a quick greedy continuation for a horizon of
// days, and the one that yields the most of the goal's outputs over that
// horizon wins. So growth first or output first, which colour and which
// square, and converting buildings after a change of goal are all decided by
// the same reckoning, for the planet as it is.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// The goals in the order the menus list them.
inline constexpr std::array<PlanetGoal, kPlanetGoalCount> kPlanetGoals{PlanetGoal::kBalanced, PlanetGoal::kIndustry,
                                                                       PlanetGoal::kResearch, PlanetGoal::kDefence,
                                                                       PlanetGoal::kShipyard, PlanetGoal::kArchaeology};

// The goal's English name (the remake text's key).
std::string_view planet_goal_name(PlanetGoal goal);

// Whether `goal` can be chosen for `planet`: the Shipyard goal while the
// planet has no Shipyard and its owner can build one, Archaeology while it
// has unexcavated ruins.
bool planet_goal_available(const Session& session, const Planet& planet, PlanetGoal goal);

// The goal `planet`'s self-management follows: the player's choice (the
// balanced goal when that is no longer available), or the AI's own.
PlanetGoal planet_goal(const Session& session, const Planet& planet);

// peCustom: chooses the project of an idle planet that the AI runs or that
// manages itself (`Planet::project_item` none). The Shipyard and Archaeology
// goals hand the planet back to the player (self-management off) once their
// project has started, so the player hears of its completion.
void pe_manage_planet(Session& session, Race& race, Planet& planet);

}  // namespace oa::game
