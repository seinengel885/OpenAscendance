// peCustom's planet management (this fork's own, not in the original): the
// project of an idle planet the AI runs or the player lets manage itself,
// chosen by a few rules (`planet_plan_rules.cpp`) for the planet's strategy
// (`PlanetGoal`, `Game::planet_goals`) and settings (`PlanetManagement`). No
// building comes for free.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// The goals in the order the menus list them.
inline constexpr std::array<PlanetGoal, 3> kPlanetGoals{PlanetGoal::kBalanced, PlanetGoal::kIndustry,
                                                         PlanetGoal::kResearch};

// The strategy `delta` steps after `current` in the order off (none), then
// `kPlanetGoals`, wrapping.
std::optional<PlanetGoal> planet_next_strategy(std::optional<PlanetGoal> current, int delta);

// Whether `planet` has unexcavated ruins.
bool planet_has_ruins(const Session& session, const Planet& planet);

// The goal's English name (the remake text's key).
std::string_view planet_goal_name(PlanetGoal goal);

// The goal `planet`'s self-management follows: the player's choice, for the
// AI's planets balanced (`pe_manage_planet` picks the AI's own).
PlanetGoal planet_goal(const Session& session, const Planet& planet);

// The management of `planet` with its standards filled in
// (`PlanetManagement`, no -1 / `BuildLevel::kStandard` left).
PlanetManagement planet_management(const Session& session, const Planet& planet);
// `own` (a planet's settings) with `standard` filled in where it has none.
PlanetManagement planet_management_resolved(PlanetManagement own, const PlanetManagement& standard);
// The standards of `planet`: planetary and orbital defence along with the
// growth, a Shipyard first on the owner's first planet, the dig along where
// there are ruins; by size (tiny .. enormous) Surface Shields
// 2 / 3 / 4 / 6 / 8, orbital weapons 1 / 1 / 2 / 3 / 4 and shields
// 1 / 2 / 2 / 3 / 4, an industry of 3 / 4 / 5 / 6 / 8 kept.
PlanetManagement planet_management_standard(const Session& session, const Planet& planet);

// Why a planet of the player needs the player (the planet list's bell), in
// this order: a lingering danger at its star, its growth stopped while
// empty squares wait for workers, no project while it has free workers
// (not self-managed).
enum class PlanetAlarm : std::uint8_t { kNone, kDanger, kGrowthStopped, kIdle };
PlanetAlarm planet_alarm(const Session& session, const Planet& planet);
// The alarm's English text (the remake text's key), "" for none.
std::string_view planet_alarm_text(PlanetAlarm alarm);

// The orbital squares the orbital weapons, shields and cloakers of
// `planet` may take with `management` (resolved): all but one each for a
// Shipyard and an Orbital Dock (built or not set to never) and one for a
// ship with either.
std::int32_t planet_orbital_defence_room(const Session& session, const Planet& planet,
                                         const PlanetManagement& management);

// The rules (`planet_plan_rules.cpp`): start the next project of an idle
// planet managed for `goal` with `management` (its standards filled in).
void pe_manage_planet_rules(Session& session, Planet& planet, PlanetGoal goal, const PlanetManagement& management);

// peCustom: chooses the project of an idle planet that the AI runs (by the
// rules, for a strategy and management it picks itself) or that manages
// itself (`Planet::project_item` none).
void pe_manage_planet(Session& session, Race& race, Planet& planet);

}  // namespace oa::game
