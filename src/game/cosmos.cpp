#include "game/cosmos.h"

#include <cstring>

#include "game/ai.h"
#include "game/galaxy.h"
#include "game/planet.h"
#include "game/race.h"
#include "game/research.h"
#include "game/ship.h"

namespace oa::game {

namespace {

std::uint8_t bit(std::uint32_t race) { return static_cast<std::uint8_t>(1u << (race & 0x1f)); }

// Tells whether ship location kind `kind` is a planet (built, refit, orbit).
bool at_planet(ShipLocation kind) {
  return kind == ShipLocation::kBuilding || kind == ShipLocation::kRefit || kind == ShipLocation::kOrbit;
}

}  // namespace

void star_set_planet_owner(Session& session, Star& star, Planet& planet, std::uint8_t race) {
  Cosmos& cosmos = session.game->cosmos;
  const std::uint8_t old = planet.owner;
  if (old != kNoOwner) {
    if (race == kNoOwner) {
      planet.owner = kNoOwner;
    } else {
      planet.owner = race;
      star.owner_mask |= bit(race);
    }
    star.owner_mask = static_cast<std::uint8_t>(star.owner_mask & ~bit(old));
    for (std::int16_t i = 0; i < star.planet_count; ++i) {
      if (cosmos.planets[static_cast<std::size_t>(star.planets[static_cast<std::size_t>(i)])].owner == old) {
        star.owner_mask |= bit(old);
        break;
      }
    }
    const StarIndex home = cosmos.races[old].home;
    if (home == star_index(cosmos, star) && (star.owner_mask & bit(old)) == 0) {
      star.owner_mask &= 0x7f;
    }
  }
  if (planet.project_item != kNoItem) {
    if (planet.project_item == kItemShip) {
      ship_destroy(session,
                   cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, planet.project_square))]);
    }
    if (planet.project_square != kNoSquare && planet.project_item != kItemAutomation) {
      Square& sq = planet_squares(cosmos, planet)[planet.project_square];
      sq.item = kNoItem;
      sq.flags = 0;
    }
    planet.project_item = kNoItem;
    planet.progress = 0;
    planet.project_square = kNoSquare;
  }
}

void ship_enter_star_system(Session& session, Star& star, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  const StarIndex here = star_index(cosmos, star);
  if (ship.location_kind == ShipLocation::kLane) {
    const Lane& lane = cosmos.lanes[static_cast<std::size_t>(ship.location)];
    Vec3 p{};
    if (lane.star_a == here) {
      p = lane.end_a;
    } else if (lane.star_b == here) {
      p = lane.end_b;
    }
    p[1] = f32(static_cast<double>(p[1]) + -200.0);
    p[2] = f32(static_cast<double>(session.rng.rand() % 200) + p[2]);
    p[0] = f32(static_cast<double>(session.rng.rand() % 200) + p[0]);
    ship_set_position(session, ship, p);
    ship.location_kind = ShipLocation::kSystem;
    ship.location = here;
  } else if (ship.location_kind == ShipLocation::kSystem) {
    ship.location = here;
  }
  const std::uint8_t b = bit(static_cast<std::uint32_t>(ship.owner));
  star.ships_mask |= b;
  star.explored_mask |= b;
  for (std::int16_t i = 0; i < star.lane_count; ++i) {
    Lane& lane = cosmos.lanes[static_cast<std::size_t>(star.lanes[static_cast<std::size_t>(i)])];
    lane.known_mask |= b;
    cosmos.stars[static_cast<std::size_t>(lane.star_a)].lanes_known_mask |= b;
    cosmos.stars[static_cast<std::size_t>(lane.star_b)].lanes_known_mask |= b;
  }
}

void ship_leave_star_system(Session& session, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  if (at_planet(ship.location_kind)) {
    Planet& planet = cosmos.planets[static_cast<std::size_t>(ship.location)];
    if (!planet_undock_ship(session, planet, ship)) {
      ship.location_kind = ShipLocation::kSystem;
      ship.location = planet.star;
    }
  }
  Star& star = cosmos.stars[static_cast<std::size_t>(ship.location)];
  for (const ShipIndex i : cosmos_ships_at_star(cosmos, star)) {
    const Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
    if (other.owner == ship.owner && &other != &ship && other.location_kind != ShipLocation::kBuilding) {
      return;
    }
  }
  star.ships_mask = static_cast<std::uint8_t>(star.ships_mask & ~bit(static_cast<std::uint32_t>(ship.owner)));
}

void star_send_player_ships_to_lane(Session& session, Star& star, Lane& lane) {
  Cosmos& cosmos = session.game->cosmos;
  Star& far =
      cosmos.stars[static_cast<std::size_t>(lane.star_a == star_index(cosmos, star) ? lane.star_b : lane.star_a)];
  for (const ShipIndex i : cosmos_ships_at_star(cosmos, star)) {
    Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    if (ship.owner == session.player_race) {
      ship_leave_star_system(session, ship);
      ship.location = lane_index(cosmos, lane);
      ship.location_kind = ShipLocation::kLane;
      ship_enter_star_system(session, far, ship);
    }
  }
}

std::vector<RaceIndex> star_list_present_races(const Cosmos& cosmos, const Star& star) {
  std::vector<RaceIndex> races;
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    if (bit(static_cast<std::uint32_t>(r)) & (star.ships_mask | star.owner_mask)) {
      races.push_back(static_cast<RaceIndex>(r));
    }
  }
  return races;
}

std::vector<ShipIndex> cosmos_ships_at_star(const Cosmos& cosmos, const Star& star) {
  const StarIndex here = star_index(cosmos, star);
  std::vector<ShipIndex> ships;
  std::int32_t used = 0;
  for (std::size_t i = 0; i < kMaxShips && used < cosmos.ship_count; ++i) {
    const Ship& ship = cosmos.ships[i];
    if (ship.owner == -1) {
      continue;
    }
    ++used;
    if ((at_planet(ship.location_kind) && cosmos.planets[static_cast<std::size_t>(ship.location)].star == here) ||
        (ship.location_kind == ShipLocation::kSystem && ship.location == here)) {
      ships.push_back(static_cast<ShipIndex>(i));
    }
  }
  return ships;
}

bool star_lane_end_guarded(const Session& session, const Star& star, RaceIndex race) {
  const Cosmos& cosmos = session.game->cosmos;
  const StarIndex here = star_index(cosmos, star);
  std::int16_t used = 0;
  for (std::size_t i = 0; i < kMaxShips && used < cosmos.ship_count; ++i) {
    const Ship& ship = cosmos.ships[i];
    if (ship.owner == -1) {
      continue;
    }
    ++used;
    const auto owner = static_cast<std::uint32_t>(ship.owner);
    if (ship.location_kind != ShipLocation::kLane ||
        !(cosmos.races[race].relation[owner] == kWar || owner == session.player_race || ship.owner == race)) {
      continue;
    }
    for (std::int16_t k = 0; k < star.lane_count; ++k) {
      const LaneIndex l = star.lanes[static_cast<std::size_t>(k)];
      if (ship.location != l) {
        continue;
      }
      const Lane& lane = cosmos.lanes[static_cast<std::size_t>(l)];
      // Still at this star's end: distance 0 travelling from star A
      // (direction +-0.0) or from star B (direction 1.0).
      std::uint32_t direction = 0;
      std::memcpy(&direction, &ship.position[1], sizeof direction);
      if ((lane.star_a == here && (direction & 0x7fffffff) == 0) || (lane.star_b == here && direction == 0x3f800000)) {
        return true;
      }
    }
  }
  return false;
}

Ship* cosmos_alloc_ship(Session& session, RaceIndex race) {
  Cosmos& cosmos = session.game->cosmos;
  for (Ship& ship : cosmos.ships) {
    if (ship.owner == -1) {
      ship_init(session, ship, 0);
      ship.owner = race;
      ++cosmos.ship_count;
      return &ship;
    }
  }
  return nullptr;
}

void cosmos_free_ship(Session& session, Ship& ship) {
  ship.owner = -1;
  ship.location_kind = ShipLocation::kNone;
  ship.name[0] = '\0';
  --session.game->cosmos.ship_count;
}

bool lane_blocked(const Session& session, LaneIndex lane, const Race& race) {
  const Cosmos& cosmos = session.game->cosmos;
  const bool every_slot = session.game->variant == Variant::kPeCustom;
  std::int16_t used = 0;
  for (std::size_t i = 0; i < kMaxShips && (every_slot || used < cosmos.ship_count); ++i) {
    const Ship& ship = cosmos.ships[i];
    if (ship.owner == -1) {
      continue;
    }
    ++used;
    const auto owner = static_cast<std::size_t>(ship.owner);
    if (ship.location_kind == ShipLocation::kLane && ship.location == lane && ship.owner != race.index &&
        race.relation[owner] != kAlliance) {
      return every_slot || used < cosmos.ship_count;
    }
  }
  return !every_slot && used < cosmos.ship_count;
}

void cosmos_flood_controlled_lanes(Session& session, Star& star) {
  Cosmos& cosmos = session.game->cosmos;
  const Race& player = cosmos.races[session.player_race];
  std::uint8_t allies = 0;
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    if (player.relation[static_cast<std::size_t>(r)] == kAlliance) {
      allies |= bit(static_cast<std::uint32_t>(r));
    }
  }
  const StarIndex here = star_index(cosmos, star);
  for (std::int16_t k = 0; k < star.lane_count; ++k) {
    const LaneIndex l = star.lanes[static_cast<std::size_t>(k)];
    Lane& lane = cosmos.lanes[static_cast<std::size_t>(l)];
    if (lane.visited != 0) {
      continue;
    }
    Star& far = cosmos.stars[static_cast<std::size_t>(lane.star_a == here ? lane.star_b : lane.star_a)];
    const std::uint8_t me = session.player_mask;
    if ((far.explored_mask & me) == 0 || (~allies & (far.owner_mask | far.ships_mask) & 0x7f & ~me) != 0) {
      continue;
    }
    if (!lane_blocked(session, l, player)) {
      lane.visited = 1;
      ++cosmos.controlled_lanes;
      cosmos_flood_controlled_lanes(session, far);
    }
  }
}

bool cosmos_check_game_over(Session& session) {
  Cosmos& cosmos = session.game->cosmos;
  const Research& research = session.game->research;
  const std::uint8_t p = session.player_race;
  const Race& player = cosmos.races[p];
  const std::uint8_t me = bit(p);
  std::uint8_t ending = 0;
  if (player.extinct == -1) {
    ending = 1;
  } else {
    ending = 2;
    for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
      if (r != p && cosmos.races[static_cast<std::size_t>(r)].extinct == 0) {
        ending = 0;
        break;
      }
    }
  }
  if (ending == 0) {
    ending = 3;
    for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
      const Star& home = cosmos.stars[static_cast<std::size_t>(cosmos.races[static_cast<std::size_t>(r)].home)];
      if ((home.owner_mask & 0x7f) != me) {
        ending = 0;
        break;
      }
    }
  }
  if (ending == 0) {
    ending = 4;
    std::uint8_t alive = 0;
    for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
      const Race& race = cosmos.races[static_cast<std::size_t>(r)];
      if (race.extinct == 0) {
        alive |= bit(static_cast<std::uint32_t>(r));
      }
      if (r != p && race.extinct == 0 && player.relation[static_cast<std::size_t>(r)] != kAlliance) {
        ending = 0;
        break;
      }
    }
    if (ending == 4) {
      for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
        if ((cosmos.stars[static_cast<std::size_t>(s)].explored_mask & alive) == 0) {
          ending = 0;
          break;
        }
      }
    }
  }
  if (ending == 0 && cosmos.star_count * 2 / 3 + 1 <= cosmos.controlled_lanes) {
    ending = 5;
  }
  if (ending == 0) {
    return false;
  }
  std::int32_t score = race_count_discoveries(research, player);
  for (std::int16_t i = 0; i < cosmos.planet_count; ++i) {
    if (cosmos.planets[static_cast<std::size_t>(i)].owner == p) {
      score += 2;
    }
  }
  score += race_ship_count(cosmos, player);
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    if (cosmos.stars[static_cast<std::size_t>(s)].explored_mask & me) {
      ++score;
    }
  }
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    if (r != p && cosmos.races[static_cast<std::size_t>(r)].extinct != -1) {
      if (player.relation[static_cast<std::size_t>(r)] == kAlliance) {
        score += 15;
      } else if (player.relation[static_cast<std::size_t>(r)] == kWar) {
        score -= 10;
      }
    }
  }
  if (ending == 4) {
    score += 50;
  }
  std::int32_t factor = 100 - cosmos.day * 75 / 4000;
  factor = factor < 25 ? 25 : factor > 100 ? 100 : factor;
  score = score * factor / 100;
  // The doubles 1.25 (0x80cf0) and 1.5 (0x80ce8).
  if (cosmos.atmosphere == Atmosphere::kNeutral) {
    score = chop(static_cast<double>(score) * 1.25);
  } else if (cosmos.atmosphere == Atmosphere::kHostile) {
    score = chop(static_cast<double>(score) * 1.5);
  }
  if (score < 0) {
    score = 0;
  }
  cosmos.ending = ending;
  cosmos.score = score * 100 / 600;
  // The window manager's game-event count set to 0 (the end-of-game screen
  // follows).
  session.events.clear();
  return true;
}

StarIndex cosmos_next_contested_star(Session& session) {
  Cosmos& cosmos = session.game->cosmos;
  const std::uint8_t p = session.player_race;
  const Race& player = cosmos.races[p];
  const std::uint8_t me = bit(p);
  const std::uint8_t war = race_at_war_mask(session, player);
  if (cosmos.battle_cursor == 0) {
    for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
      session.star_day_flags[static_cast<std::size_t>(s)] &= 0xfe;
    }
  }
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    std::uint8_t& flags = session.star_day_flags[static_cast<std::size_t>(s)];
    if (flags & 1) {
      continue;
    }
    flags |= 1;
    const Star& star = cosmos.stars[static_cast<std::size_t>(s)];
    if ((star.ships_mask & me) == 0 || (star.ships_mask & war) == 0) {
      continue;
    }
    for (const ShipIndex i : cosmos_ships_at_star(cosmos, star)) {
      const Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
      const auto owner = static_cast<std::uint32_t>(ship.owner);
      if ((owner == p || player.relation[owner] == kWar) && ship.power > 0) {
        ++cosmos.battle_cursor;
        return s;
      }
    }
  }
  return kNone;
}

void cosmos_end_day(Session& session) {
  Game& game = *session.game;
  Cosmos& cosmos = game.cosmos;
  if (session.autoplay && cosmos.day > 4999) {
    // The `flash.pop` demo restarts with a random galaxy (palettes are the
    // user interface's).
    const int a = session.rng.rand();
    const int b = session.rng.rand();
    const int c = session.rng.rand();
    cosmos_generate(session, static_cast<std::int16_t>(c % 76 + 25), static_cast<std::int16_t>(b % 5 + 3),
                    static_cast<std::uint8_t>(a % 21));
    cosmos_compute_hops(cosmos);
    cosmos_generate_planets(session);
    cosmos_assign_colors(session, static_cast<std::uint8_t>(session.rng.rand() % 6));
    cosmos.day = 0;
  }
  cosmos.battle_cursor = 0;
  session.star_day_flags.fill(0);
  cosmos_resolve_ai_battles(session);
  cosmos_resolve_remaining_battles(session);
  research_daily_totals(session);
  for (std::int16_t i = 0; i < cosmos.planet_count; ++i) {
    planet_daily_update(session, cosmos.planets[static_cast<std::size_t>(i)]);
  }
  cosmos_rank_races(session);
  if (game.variant == Variant::kPeCustom) {
    cosmos_note_ship_arrivals(session);
  }
  const StrategicAi& ai = strategic_ai(game.variant);
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    if (r != session.player_race) {
      ai.race_turn_update(session, cosmos.races[static_cast<std::size_t>(r)]);
    }
  }
  ai.race_turn_update(session, cosmos.races[session.player_race]);
  for (std::int16_t l = 0; l < cosmos.lane_count; ++l) {
    cosmos.lanes[static_cast<std::size_t>(l)].visited = 0;
  }
  Star& home = cosmos.stars[static_cast<std::size_t>(cosmos.races[session.player_race].home)];
  if ((home.owner_mask & session.player_mask) == 0) {
    cosmos.controlled_lanes = 0;
  } else {
    cosmos.controlled_lanes = 1;
    cosmos_flood_controlled_lanes(session, home);
  }
  ++cosmos.day;
  if (cosmos.ending == 0 && !session.autoplay) {
    cosmos_check_game_over(session);
  }
}

void cosmos_next_turn(Session& session) {
  while (cosmos_next_contested_star(session) != kNone) {
  }
  cosmos_end_day(session);
}

void cosmos_rank_races(Session& session) {
  Cosmos& cosmos = session.game->cosmos;
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    RaceRank& rank = cosmos.ranks[static_cast<std::size_t>(r)];
    const Race& race = cosmos.races[static_cast<std::size_t>(r)];
    // Original bug: the fleet-strength sum is overwritten by the ship count;
    // fixed in peCustom.
    rank.fleet =
        static_cast<std::int16_t>(session.game->variant == Variant::kPeCustom ? race_fleet_strength(cosmos, race)
                                                                              : race_ship_count(cosmos, race));
    rank.colonies = static_cast<std::int16_t>(race_count_planets(cosmos, race));
  }
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    RaceRank& rank = cosmos.ranks[static_cast<std::size_t>(r)];
    const std::int32_t mine = static_cast<std::int16_t>(rank.fleet + rank.colonies);
    std::int16_t better = 0;
    for (std::int16_t o = 0; o < cosmos.race_count; ++o) {
      const RaceRank& other = cosmos.ranks[static_cast<std::size_t>(o)];
      if (o != r && mine < other.fleet + other.colonies) {
        ++better;
      }
    }
    rank.rank = better;
  }
}

void cosmos_resolve_ai_battles(Session& session) {
  Cosmos& cosmos = session.game->cosmos;
  const std::uint8_t me = bit(session.player_race);
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    Star& star = cosmos.stars[static_cast<std::size_t>(s)];
    const std::uint8_t present = star.ships_mask | star.owner_mask;
    star.battle_done = 0;
    if ((present & me) == 0) {
      if (present != 0) {
        star_resolve_combat(session, star, false);
      }
      star.battle_done = -1;
    }
  }
}

void cosmos_resolve_remaining_battles(Session& session) {
  Cosmos& cosmos = session.game->cosmos;
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    Star& star = cosmos.stars[static_cast<std::size_t>(s)];
    if (star.battle_done != -1) {
      if (star.ships_mask != 0 || star.owner_mask != 0) {
        star_resolve_combat(session, star, true);
      }
      star.battle_done = -1;
    }
  }
}

void star_resolve_combat(Session& session, Star& star, bool include_player) {
  Cosmos& cosmos = session.game->cosmos;
  if (session.autoplay) {
    include_player = true;
  }
  std::vector<RaceIndex> races;
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    if ((include_player || r != session.player_race) &&
        (bit(static_cast<std::uint32_t>(r)) & (star.owner_mask | star.ships_mask)) != 0) {
      races.push_back(static_cast<RaceIndex>(r));
    }
  }
  if (races.empty()) {
    return;
  }
  const StrategicAi& ai = strategic_ai(session.game->variant);
  bool active = true;
  while (active) {
    active = false;
    for (const RaceIndex r : races) {
      BattleAction action;
      active |= ai.battle_turn(session, cosmos.races[r], star, action);
      if (action.kind == BattleActor::kPlanet) {
        planet_fire_weapon(session, cosmos.planets[static_cast<std::size_t>(action.index)], true);
      } else if (action.kind == BattleActor::kShip) {
        ship_do_action(session, cosmos.ships[static_cast<std::size_t>(action.index)], true);
      }
    }
  }
}

void cosmos_note_ship_arrivals(Session& session) {
  const Cosmos& cosmos = session.game->cosmos;
  for (std::size_t i = 0; i < kMaxShips; ++i) {
    const Ship& ship = cosmos.ships[i];
    StarIndex star = kNone;
    if (ship.owner != -1 && ship.location_kind == ShipLocation::kSystem) {
      star = ship.location;
    } else if (ship.owner != -1 && ship.location_kind == ShipLocation::kOrbit) {
      star = cosmos.planets[static_cast<std::size_t>(ship.location)].star;
    }
    if (session.ship_arrival_star[i] != star) {
      session.ship_arrival_star[i] = star;
      session.ship_arrival_day[i] = cosmos.day;
    }
  }
}

}  // namespace oa::game
