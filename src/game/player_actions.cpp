#include "game/player_actions.h"

#include <algorithm>
#include <stdexcept>

#include "game/planet.h"
#include "game/race.h"
#include "game/ship.h"
#include "game/tables.h"
#include "game/variant.h"

namespace oa::game {

void planet_abandon_square(Session& session, Planet& planet, std::uint16_t square) {
  if (planet_squares(session.game->cosmos, planet)[square].item == kItemShip) {
    const ShipIndex ship = planet_square_ship(session, planet, square);
    ship_abandon(session, session.game->cosmos.ships.at(static_cast<std::size_t>(ship)));
  }
  planet_set_square_item(session, planet, square, kNoItem, SquareMode::kRemove);
}

Planet* ship_crew_home(Session& session, const Ship& ship) {
  if (session.game->variant != Variant::kPeCustom || ship.location_kind != ShipLocation::kOrbit) {
    return nullptr;
  }
  Planet& planet = session.game->cosmos.planets.at(static_cast<std::size_t>(ship.location));
  return planet.owner == ship.owner && planet.population < planet.max_population ? &planet : nullptr;
}

void ship_abandon(Session& session, Ship& ship) {
  if (Planet* planet = ship_crew_home(session, ship); planet != nullptr) {
    const std::int32_t crew = session.tables->planitems.items.at(kItemShip).used_population;
    planet->population =
        static_cast<std::uint16_t>(std::min<std::int32_t>(planet->population + crew, planet->max_population));
    planet_recalc_output(session, *planet);
  }
  ship_destroy(session, ship);
}

void planet_toggle_self_managed(Planet& planet) { planet.self_managed = ~planet.self_managed; }

ColonizeResult planet_colonize(Session& session, Planet& planet, std::uint16_t ship_square, std::uint16_t square,
                               std::string_view name) {
  Cosmos& cosmos = session.game->cosmos;
  Ship& ship = cosmos.ships.at(static_cast<std::size_t>(planet_square_ship(session, planet, ship_square)));
  ship_remove_gizmo(session, ship, ship_find_gizmo(ship, kGizmoColonizer));
  if (planet.owner != kNoOwner) {
    throw std::logic_error("planet_colonize: the planet is owned");
  }
  // The input box edits the name in place: at most 20 characters.
  const std::size_t n = std::min<std::size_t>(name.size(), 20);
  std::copy_n(name.begin(), n, planet.name.begin());
  planet.name[n] = '\0';
  const auto owner = static_cast<std::uint8_t>(ship.owner);
  planet.owner = owner;
  Star& star = cosmos.stars.at(static_cast<std::size_t>(planet.star));
  star.owner_mask = static_cast<std::uint8_t>(star.owner_mask | (1u << (owner & 0x1f)));
  planet.population = 2;
  planet.acquired_day = cosmos.day;
  planet_set_square_item(session, planet, square, kItemColonyBase, SquareMode::kPlaceBuilt);
  // The race with the most colonies among the player and the races it has
  // met (the first one on a tie).
  ColonizeResult result;
  const Race& player = cosmos.races.at(session.player_race);
  std::int32_t best = 0;
  std::int32_t best_race = -1;
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    const Race& race = cosmos.races.at(static_cast<std::size_t>(r));
    if (race.extinct != 0) {
      continue;
    }
    const std::int32_t count = race_count_planets(cosmos, race);
    if (r == session.player_race) {
      result.player_colonies = count;
    }
    if (best < count && (r == session.player_race || player.relation.at(static_cast<std::size_t>(r)) != kNotMet)) {
      best = count;
      best_race = r;
    }
  }
  if (best_race < 0) {
    throw std::logic_error("planet_colonize: no race has a colony");
  }
  result.best_race = static_cast<RaceIndex>(best_race);
  result.best_colonies = best;
  return result;
}

void research_set_project(Research& research, RaceIndex race, std::uint8_t item) { research.project.at(race) = item; }

void research_toggle_self_managed(Research& research) { research.self_managed = ~research.self_managed; }

void research_clear_unseen(Research& research, RaceIndex race) {
  for (std::uint16_t i = 0; i < research.count; ++i) {
    ResearchItem& item = research.items.at(i);
    item.unseen_mask = static_cast<std::uint8_t>(item.unseen_mask & ~(1u << (race & 0x1f)));
  }
}

}  // namespace oa::game
