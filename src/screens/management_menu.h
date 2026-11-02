// peCustom's management menu of a planet (planet screen, planet list): the
// label "Management: <goal or Off>" and the menu it opens, which turns the
// planet's self-management off or on with a goal (`game/planet_plan.h`).
#pragma once

#include <string>

#include "ui/winmgr.h"

namespace oa {

namespace game {
struct Planet;
struct Session;
}  // namespace game

// Whether the screens show the management label of `planet`: the player's
// planets in a peCustom game.
bool management_shown(const game::Session& session, const game::Planet& planet);

// "Management: Off" or "Management: <goal>".
std::string management_label(const WinMgr& wm, const game::Session& session, const game::Planet& planet);

// Runs the menu (Off and the goals available for the planet) and applies
// the choice; returns whether one was made.
bool run_management_menu(WinMgr& wm, game::Session& session, game::Planet& planet);

}  // namespace oa
