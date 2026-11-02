#include "screens/management_menu.h"

#include <optional>
#include <vector>

#include "core/c_format.h"
#include "game/planet_plan.h"
#include "game/player_actions.h"
#include "game/session.h"
#include "game/variant.h"

namespace oa {

bool management_shown(const game::Session& session, const game::Planet& planet) {
  return session.game->variant == game::Variant::kPeCustom && planet.owner == session.player_race;
}

std::string management_label(const WinMgr& wm, const game::Session& session, const game::Planet& planet) {
  const std::string state = planet.self_managed == 0
                                ? wm.remake_text("Off")
                                : wm.remake_text(game::planet_goal_name(game::planet_goal(session, planet)));
  return c_format(wm.remake_text("Management: %s"), state.c_str());
}

bool run_management_menu(WinMgr& wm, game::Session& session, game::Planet& planet) {
  std::vector<std::optional<game::PlanetGoal>> options{std::nullopt};
  std::vector<std::string> names{wm.remake_text("Off")};
  int current = 0;
  for (const game::PlanetGoal goal : game::kPlanetGoals) {
    if (game::planet_goal_available(session, planet, goal)) {
      if (planet.self_managed != 0 && goal == game::planet_goal(session, planet)) {
        current = static_cast<int>(options.size());
      }
      options.emplace_back(goal);
      names.push_back(wm.remake_text(game::planet_goal_name(goal)));
    }
  }
  const std::string title =
      c_format(wm.remake_text("Management of %s"), std::string(game::name_view(planet.name)).c_str());
  const int pick = wm.choice_box(title, names, current);
  if (pick < 0) {
    return false;
  }
  game::planet_set_management(session, planet, options[static_cast<std::size_t>(pick)]);
  return true;
}

}  // namespace oa
