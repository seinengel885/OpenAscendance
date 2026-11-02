#include "ui/event_wnd.h"

#include <format>
#include <string>

#include "core/c_format.h"
#include "core/error.h"
#include "game/planet_plan.h"
#include "game/session.h"
#include "ui/remake_help.h"
#include "ui/winmgr.h"

namespace oa {

namespace {

using game::EventCode;

// `gwshare.txt` tables of the pictures: the race slots' small portraits
// (0..6) and ship sprites (14..20), the planets, the planet items, the
// research items and the planet screen's pictures (`17np.shp`).
constexpr int kRacePortraits = 0;
constexpr int kRaceShips = 14;
constexpr int kPlanetShapes = 29;
constexpr int kPlanItemShapes = 30;
constexpr int kResearchShapes = 31;
constexpr int kPlanetScreenShapes = 44;
// `17np.shp` frame of the free-population event's second picture.
constexpr int kFreePopulationFrame = 2;
// `static.txt`: the plural ending ("" for 1, else "s") and the species
// names (`0xf4578`).
constexpr int kStringSingular = 0xd;
constexpr int kStringPlural = 0xe;
constexpr int kStringSpecies = 101;

std::string name_of(const auto& name) { return std::string(game::name_view(name)); }

// The planets frame of a planet: type * 5 + size.
int planet_frame(const game::Planet& p) { return p.type * 5 + p.size; }

}  // namespace

EventWnd::EventWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : HelpWnd(wm, record, shapes) {}

void EventWnd::set_event(const game::GameEvent& event) {
  event_ = event;
  if (event.code == EventCode::kPlanetBuiltUp) {
    open_remake_topic(wm_, *this, "oa_event_built_up");
  } else {
    open_topic(kHelpFile, std::format("event{:02d}", static_cast<int>(event.code)));
  }
  game::Session* session = wm_.services().game.core();
  if (session == nullptr) {
    throw std::logic_error("game event without a game core");
  }
  const game::Cosmos& cosmos = session->game->cosmos;
  const game::GameTables& tables = *session->tables;
  // The topic's first element is its text, the second its picture.
  HelpElement& text = content_.elements[0];
  HelpElement& picture = content_.elements[1];
  HelpElement& second = content_.elements[2];
  if (text.kind != HelpElement::Kind::kText) {
    throw DataError(std::format("help.txt: event{:02d} does not start with a text", static_cast<int>(event.code)));
  }
  const std::string format = text.text;
  const auto species = [&](int race) {
    return wm_.static_string(kStringSpecies + cosmos.races.at(static_cast<std::size_t>(race)).species);
  };
  const std::string player = species(session->player_race);
  const auto ship = [&](std::int32_t index) -> const game::Ship& {
    return cosmos.ships.at(static_cast<std::size_t>(index));
  };
  const auto planet = [&](std::int32_t index) -> const game::Planet& {
    return cosmos.planets.at(static_cast<std::size_t>(index));
  };
  const auto star = [&](std::int32_t index) -> const game::Star& {
    return cosmos.stars.at(static_cast<std::size_t>(index));
  };
  const auto research = [&](std::int32_t item) {
    return name_of(session->game->research.items.at(static_cast<std::size_t>(item)).name);
  };
  // The star of a ship in a system (asserted by the original).
  const auto ship_star = [&](const game::Ship& s) -> const game::Star& {
    if (s.location_kind != game::ShipLocation::kSystem) {
      throw std::logic_error("game event of a ship outside a system");
    }
    return star(s.location);
  };
  const auto set_picture = [](HelpElement& e, int table, int frame) {
    e.shape_index = table;
    e.frame = frame;
  };
  // The player's ship sprite of the ship's hull.
  const auto player_ship_picture = [&](const game::Ship& s) {
    set_picture(picture, kRaceShips + session->player_race, static_cast<std::int8_t>(s.hull_size));
  };
  const auto free_population = [](const game::Planet& p) { return p.population - p.used_population; };

  switch (event.code) {
    case EventCode::kResearchDone:
      text.text = c_format(format, player.c_str(), research(event.a).c_str());
      set_picture(picture, kResearchShapes, static_cast<std::int16_t>(event.a));
      break;
    case EventCode::kXenoDigDiscovery: {
      const game::Planet& p = planet(event.b);
      text.text = c_format(format, player.c_str(), name_of(p.name).c_str(), research(event.a).c_str());
      set_picture(picture, kResearchShapes, static_cast<std::int16_t>(event.a));
      set_picture(second, kPlanetShapes, planet_frame(p));
      break;
    }
    case EventCode::kBuildingComplete: {
      const game::Planet& p = planet(event.a);
      const auto item = static_cast<std::uint8_t>(event.b);
      const std::string name = name_of(p.name);
      text.text = c_format(format, tables.planitems.items.at(item).name.c_str(), name.c_str(), name.c_str(),
                           free_population(p));
      set_picture(picture, kPlanItemShapes, item);
      set_picture(second, kPlanetShapes, planet_frame(p));
      break;
    }
    case EventCode::kRaceWantsToTalk:
      text.text = c_format(format, species(static_cast<std::int16_t>(event.a)).c_str());
      set_picture(picture, kRacePortraits + static_cast<std::uint16_t>(event.a), 0);
      break;
    case EventCode::kRaceExtinct:
      text.text = c_format(format, species(event.a).c_str());
      set_picture(picture, kRacePortraits + static_cast<std::uint16_t>(event.a), 0);
      break;
    case EventCode::kShipArrived:
    case EventCode::kFirstContact:
    case EventCode::kHostileShips:
    case EventCode::kHostileColony:
    case EventCode::kOthersAtWar:
    case EventCode::kSystemDiscovered: {
      const game::Ship& s = ship(event.a);
      const game::Star& st = ship_star(s);
      const std::string ship_name = name_of(s.name);
      const std::string star_name = name_of(st.name);
      if (event.code == EventCode::kSystemDiscovered) {
        text.text = c_format(format, ship_name.c_str(), star_name.c_str(), static_cast<int>(st.planet_count),
                             wm_.static_string(st.planet_count == 1 ? kStringSingular : kStringPlural).c_str(),
                             static_cast<int>(st.lane_count),
                             wm_.static_string(st.lane_count == 1 ? kStringSingular : kStringPlural).c_str());
      } else {
        text.text = c_format(format, ship_name.c_str(), star_name.c_str(), species(event.b).c_str());
      }
      player_ship_picture(s);
      break;
    }
    case EventCode::kUnknownShipEntered:
    case EventCode::kHostileShipEntered: {
      const game::Ship& s = ship(event.a);
      const std::string star_name = name_of(ship_star(s).name);
      if (event.code == EventCode::kHostileShipEntered) {
        text.text = c_format(format, species(event.b).c_str(), star_name.c_str());
      } else {
        text.text = c_format(format, star_name.c_str());
      }
      // The arriving race's sprite, of the hull of the event's ship.
      set_picture(picture, kRaceShips + static_cast<std::int16_t>(event.b), static_cast<std::int8_t>(s.hull_size));
      break;
    }
    case EventCode::kShipScrapped:
      set_picture(picture, kRaceShips + session->player_race, static_cast<std::int16_t>(event.a));
      break;
    case EventCode::kShipLost:
      text.text = c_format(format, name_of(star(event.b).name).c_str());
      set_picture(picture, kRaceShips + session->player_race, static_cast<std::int16_t>(event.a));
      break;
    case EventCode::kColonyInvaded: {
      const game::Planet& p = planet(event.a);
      text.text = c_format(format, name_of(p.name).c_str(), species(p.owner).c_str());
      // The original stores the shape cache handle of `planets.shp` as the
      // picture's `gwshare.txt` index: the same number, the gwshare files
      // taking the cache slots 0..44 in order at start-up.
      set_picture(picture, kPlanetShapes, planet_frame(p));
      break;
    }
    case EventCode::kPlanetBuiltUp: {
      const game::Planet& p = planet(event.a);
      const std::string strategy = wm_.remake_text(game::planet_goal_name(game::planet_goal(*session, p)));
      text.text = c_format(format, name_of(p.name).c_str(), strategy.c_str());
      set_picture(picture, kPlanetShapes, planet_frame(p));
      set_picture(second, kPlanetScreenShapes, kFreePopulationFrame);
      break;
    }
    case EventCode::kFreePopulation: {
      const game::Planet& p = planet(event.a);
      text.text = c_format(format, name_of(p.name).c_str());
      set_picture(picture, kPlanetShapes, planet_frame(p));
      set_picture(second, kPlanetScreenShapes, kFreePopulationFrame);
      break;
    }
    case EventCode::kEnteredOrbit: {
      const game::Ship& s = ship(event.a);
      const game::Planet& p = planet(event.b);
      text.text = c_format(format, name_of(s.name).c_str(), name_of(p.name).c_str());
      player_ship_picture(s);
      set_picture(second, kPlanetShapes, planet_frame(p));
      break;
    }
    case EventCode::kShipBuilt:
    case EventCode::kShipRefitted: {
      const game::Ship& s = ship(event.a);
      const game::Planet& p = planet(event.b);
      const std::string name = name_of(p.name);
      text.text = c_format(format, name_of(s.name).c_str(), name.c_str(), name.c_str(), free_population(p));
      player_ship_picture(s);
      set_picture(second, kPlanetShapes, planet_frame(p));
      break;
    }
    case EventCode::kRemainingMoves:
      text.text = c_format(format, name_of(star(event.a).name).c_str());
      break;
    case EventCode::kBasicsKnown:
    case EventCode::kLabWithoutResearch:
      text.text = c_format(format, player.c_str());
      set_picture(picture, kRacePortraits + session->player_race, 0);
      break;
    case EventCode::kLaneTransmission: {
      // Posted by no code of the original; a ship in a lane, named after
      // the lane's end the player knows.
      const game::Ship& s = ship(event.a);
      if (s.location_kind != game::ShipLocation::kLane) {
        throw std::logic_error("lane transmission of a ship outside a lane");
      }
      const game::Lane& lane = cosmos.lanes.at(static_cast<std::size_t>(s.location));
      const game::Star& a = star(lane.star_a);
      const game::Star& end = (a.explored_mask & session->player_mask) != 0 ? a : star(lane.star_b);
      text.text = c_format(format, name_of(s.name).c_str(), name_of(end.name).c_str());
      player_ship_picture(s);
      break;
    }
    case EventCode::kAbilityCharged:
      break;
  }
}

}  // namespace oa
