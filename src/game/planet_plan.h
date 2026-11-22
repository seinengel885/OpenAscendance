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
inline constexpr std::array<PlanetGoal, 3> kPlanetGoals{PlanetGoal::kBalanced, PlanetGoal::kEarlyIndustry,
                                                         PlanetGoal::kEarlyResearch};

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
// The standards of `planet`: no buildings (the player sets per planet what
// it wants), but a Shipyard first on the owner's first planet; the final
// build-out of the research preset.
PlanetManagement planet_management_standard(const Session& session, const Planet& planet);

// The final build-out's presets (`PlanetManagement::final_outputs`).
enum class FinalPreset : std::uint8_t {
  kResearch,  // industry at least 3 / 4 / 5 / 6 / 8 by size, research maximised, no prosperity
  kShipyard,  // industry maximised, research kept (blue squares), prosperity kept
  kDock,      // industry maximised, research kept (blue squares), no prosperity
};
std::array<FinalOutput, PlanetManagement::kOutputs> planet_final_preset(FinalPreset preset, const Planet& planet);

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

// Whether `item` is a project without end the rules choose (Endless Party,
// Scientist Takeover): a planet running one is managed every day.
bool pe_endless_project(std::uint8_t item);

// The rules (`planet_plan_rules.cpp`): start the next project of an idle
// planet managed with `management` (its standards filled in), or end its
// project without end for another or none.
void pe_manage_planet_rules(Session& session, Planet& planet, const PlanetManagement& management);

// peCustom: chooses the project of an idle planet (or one running a
// project without end, `pe_endless_project`) that the AI runs (by the
// rules, for a strategy and management it picks itself) or that manages
// itself; a planet busy otherwise is left alone.
void pe_manage_planet(Session& session, Race& race, Planet& planet);

// Whether the bell of `planet`'s project was on (`PlanetManagement::bell`,
// turned off): called as its project completes.
bool pe_take_project_bell(Session& session, const Planet& planet);

// peCustom: a planet of the player whose management was just turned on or
// changed starts its project now (`pe_manage_planet`), not only with the
// next day: when idle or its project has not begun (no progress, not a
// ship), that one cancelled.
void pe_manage_planet_now(Session& session, Planet& planet);

}  // namespace oa::game
