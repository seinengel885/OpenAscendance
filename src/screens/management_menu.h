// peCustom's management of planets (planet screen, planet list): the label
// "Management: <strategy or Off>", the window it opens (the strategy, which
// turns the self-management off or on, and the rest of the planets'
// `PlanetManagement`, `game/planet_plan.h`) for one planet or the planet
// list's marked ones, and their labels.
#pragma once

#include <span>
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

// Runs the management window of `planets` (one or more: a change sets its
// row on all of them) and applies it on OK; returns whether OK was clicked.
bool run_management_menu(WinMgr& wm, game::Session& session, std::span<game::Planet* const> planets);

// The labels of `planet`, comma separated ("" without).
std::string planet_labels_text(const game::Session& session, const game::Planet& planet);

// Draws the label symbol (a tag, 16 x 10 pixels) at (x, y) of the screen.
void draw_label_icon(Image8& screen, int x, int y, std::uint8_t color);

// Runs the label window of `planets` (check a label for all of them, type a
// new one) and applies it on OK; returns whether OK was clicked.
bool run_label_menu(WinMgr& wm, game::Session& session, std::span<game::Planet* const> planets);

}  // namespace oa
