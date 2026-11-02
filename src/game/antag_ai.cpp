#include "game/antag_ai.h"

#include <array>
#include <vector>

#include "core/fixed_width.h"
#include "game/antag_planet.h"
#include "game/cosmos.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/planet_plan.h"
#include "game/race.h"
#include "game/race_ai.h"
#include "game/raceneg.h"
#include "game/ship.h"
#include "game/tables.h"

namespace oa::game {

namespace {

// Ship roles and parts of the designs (`gizmos.txt` ids).
constexpr std::int8_t kGizmoMassBarrage = 0;
constexpr std::int8_t kGizmoPlasmatron = 5;
constexpr std::int8_t kGizmoFergnatzLens = 7;
constexpr std::int8_t kGizmoHypersphereDriver = 8;
constexpr std::int8_t kGizmoIonWrap = 10;
constexpr std::int8_t kGizmoTonklinMotor = 16;
constexpr std::int8_t kGizmoProtonShaver = 27;
constexpr std::int8_t kGizmoMolecularTieDown = 33;
constexpr std::int8_t kGizmoBrunswik = 35;
constexpr std::int8_t kGizmoPhaseBomb = 70;

// The gizmo categories of `Ship_best_gizmo_for_race`.
constexpr std::uint8_t kCategoryWeapon = 0;
constexpr std::uint8_t kCategoryShield = 1;
constexpr std::uint8_t kCategoryDrive = 2;
constexpr std::uint8_t kCategoryGenerator = 4;

// Research items of the hull sizes: Interplanetary Exploration (medium),
// Advanced Exploration (large), Large Scale Construction (enormous).
constexpr std::array<std::uint16_t, 3> kHullResearch = {1, 19, 31};

// The specials of `Race_ai_fill_specials` (ANTAG 0x9694c, duplicates kept).
constexpr std::array<std::int8_t, 20> kSpecials = {0x45, 0x3f, 0x32, 0x2e, 0x2d, 0x26, 0x25, 0x22, 0x2f, 0x37,
                                                   0x34, 0x40, 0x36, 0x32, 0x26, 0x25, 0x42, 0x3d, 0x30, 0x31};

// Ability recharge periods by species (ANTAG 0x968e8, as PATCH 0x86768).
constexpr std::array<std::uint16_t, kSpeciesCount> kAbilityPeriod = {0,   0,  0,  0,  0, 60, 100, 62, 77, 90, 150,
                                                                     140, 63, 66, 72, 0, 92, 100, 0,  89, 68};

// The production roles of `Race_ai_start_ship_build`.
enum Role : std::uint8_t { kRoleColony = 0, kRoleWar = 1, kRoleInvasion = 2 };

std::uint8_t bit_of(std::uint32_t race) { return static_cast<std::uint8_t>(1u << (race & 0x1f)); }

bool knows(const Research& research, std::uint16_t item, RaceIndex race) {
  return ((research.items.at(item).known_mask >> (race & 0x1f)) & 1) != 0;
}

bool is_ai(const Session& session, const Race& race) { return race.index != session.player_race || session.autoplay; }

// Returns the counter of the ship's role at a star (`has_colonizer` ->
// colony, an Invasion Module -> invasion, else war).
std::array<std::int8_t, kMaxStars>& role_counter(Session::AntagAiWork& work, const Ship& ship) {
  if (ship.has_colonizer != 0) {
    return work.colony_ships;
  }
  return ship_has_gizmo(ship, kGizmoInvasionModule) ? work.invasion_ships : work.war_ships;
}

void count_up(std::int8_t& counter) { counter = wrap_i8(counter + 1); }

// `Ship_ai_strength` ANTAG 0x4a988: the parts with exactly one use left
// today, the power and the hit points.
std::int32_t ship_ai_strength(const Session& session, const Ship& ship) {
  std::int32_t sum = 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const GizmoSlot& slot = ship.slots[static_cast<std::size_t>(i)];
    if (slot.uses_left != 1 || slot.gizmo == -1) {
      continue;
    }
    const Gizmo& g = session.tables->gizmos.at(static_cast<std::uint8_t>(slot.gizmo));
    sum += g.industry;
    switch (g.category) {
      case 0:
        sum += g.level * 10 + g.range / 5;
        break;
      case 1:
        sum += g.level * 5;
        break;
      case 2:
        sum += g.level * 2;
        break;
      case 3:
        sum += g.level;
        break;
      case 5:
        sum += g.level * 5 + g.range / 5;
        break;
      default:
        break;
    }
  }
  return (sum + ship_battle_power(session, ship) * 10 + ship.hit_points * 4) / 10;
}

// `Star_ai_colonise_value` ANTAG 0x1da4c.
std::int32_t star_ai_colonise_value(const Session& session, const Star& star, RaceIndex race) {
  const Cosmos& cosmos = session.game->cosmos;
  const StarIndex here = star_index(cosmos, star);
  if ((star.lanes_known_mask & bit_of(race)) == 0) {
    return 700;
  }
  std::int32_t v = 0;
  for (std::int16_t k = 0; k < star.planet_count; ++k) {
    const Planet& planet = cosmos.planets[static_cast<std::size_t>(star.planets[static_cast<std::size_t>(k)])];
    if (planet.owner != race) {
      v += planet_ai_target_value(session, planet, race);
    }
  }
  for (std::int16_t i = 0; i < cosmos.race_count; ++i) {
    const auto ui = static_cast<std::size_t>(i);
    if (i != race && cosmos.races[ui].home == here && (star.owner_mask & 0x80) != 0 &&
        cosmos.races[race].relation[ui] == kWar) {
      v = v * 3 + 200;
    }
  }
  if (star.owner_mask == 0) {
    v *= 2;
  }
  if (v < 0) {
    v = 0;
  }
  if ((star.explored_mask & bit_of(race)) == 0) {
    v = v * 4 + 300;
  }
  return v;
}

// `Star_ai_defence_value` ANTAG 0x1db70, with the race's own ship list (so
// the threat of the races at war, which it sums, is always 0: original bug,
// fixed in peCustom, which sums the ships at the star of the races the race
// sees as a danger, `race_danger_mask`).
std::int32_t star_ai_defence_value(const Session& session, const Star& star, RaceIndex race,
                                   const std::vector<ShipIndex>& ships) {
  const Cosmos& cosmos = session.game->cosmos;
  const StarIndex here = star_index(cosmos, star);
  const std::uint8_t b = bit_of(race);
  if ((star.lanes_known_mask & b) == 0 || (star.owner_mask & b) == 0) {
    return 0;
  }
  std::int32_t acc = 0;
  float threat = 0.0f;
  if (star.ships_mask != 0) {
    const bool pe_custom = session.game->variant == Variant::kPeCustom;
    const std::uint8_t danger = pe_custom ? race_danger_mask(cosmos, cosmos.races[race]) : 0;
    const std::vector<ShipIndex> here_ships = pe_custom ? cosmos_ships_at_star(cosmos, star) : std::vector<ShipIndex>{};
    for (const ShipIndex i : pe_custom ? here_ships : ships) {
      const Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
      const bool at_star = (ship.location_kind == ShipLocation::kSystem && ship.location == here) ||
                           (ship.location_kind == ShipLocation::kOrbit &&
                            cosmos.planets[static_cast<std::size_t>(ship.location)].star == here);
      const bool hostile = pe_custom ? (danger & bit_of(static_cast<std::uint32_t>(ship.owner))) != 0
                                     : cosmos.races[race].relation[static_cast<std::size_t>(ship.owner)] == kWar;
      if (at_star && ship.owner != race && hostile) {
        threat = f32(static_cast<double>(ship_ai_strength(session, ship)) + threat);
      }
    }
    // The floats 4.0 (0x90b90) and 0.01 (0x90b94).
    threat = f32(static_cast<double>(threat) * 4.0f * static_cast<double>(0.01f));
  }
  if (threat > 0.0f) {
    for (std::int16_t k = 0; k < star.planet_count; ++k) {
      const Planet& planet = cosmos.planets[static_cast<std::size_t>(star.planets[static_cast<std::size_t>(k)])];
      if (planet.owner == race) {
        acc += planet_ai_own_value(session, planet) - 10 * planet_orbital_defence_rating(cosmos, planet) -
               5 * planet_surface_shield_strength(cosmos, planet);
      }
    }
    if (cosmos.races[race].home == here) {
      acc += 20000;
    }
    acc = chop(static_cast<double>(acc) * threat);
  }
  if (acc < 0) {
    acc = 0;
  }
  return acc * 5;
}

// `Cosmos_flood_frontier_lanes` ANTAG 0x1fdf0: counts at each star the
// lanes leading to unexplored or foreign stars, crossing the others. The
// lane marks are shared with the player's controlled-lane flood (kept).
void cosmos_flood_frontier_lanes(Session& session, const Star& star, RaceIndex race) {
  Cosmos& cosmos = session.game->cosmos;
  const Race& self = cosmos.races[race];
  std::uint8_t allies = 0;
  for (std::int16_t i = 0; i < cosmos.race_count; ++i) {
    if (self.relation[static_cast<std::size_t>(i)] == kAlliance) {
      allies |= bit_of(static_cast<std::uint32_t>(i));
    }
  }
  const StarIndex here = star_index(cosmos, star);
  std::uint8_t& out = session.antag_ai.frontier[static_cast<std::size_t>(here)];
  const std::uint8_t me = bit_of(race);
  for (std::int16_t k = 0; k < star.lane_count; ++k) {
    const LaneIndex l = star.lanes[static_cast<std::size_t>(k)];
    Lane& lane = cosmos.lanes[static_cast<std::size_t>(l)];
    if (lane.visited == race) {
      continue;
    }
    const Star& far = cosmos.stars[static_cast<std::size_t>(lane.star_a == here ? lane.star_b : lane.star_a)];
    if ((far.explored_mask & me) == 0) {
      out = wrap_u8(out + 1);
      continue;
    }
    if ((~allies & (far.owner_mask | far.ships_mask) & 0x7f & ~me) != 0) {
      out = wrap_u8(out + 1);
      continue;
    }
    if (lane_blocked(session, l, self)) {
      out = wrap_u8(out + 1);
    } else {
      lane.visited = race;
      cosmos_flood_frontier_lanes(session, far, race);
    }
  }
}

// `Cosmos_count_frontier_lanes` ANTAG 0x1fd90.
void cosmos_count_frontier_lanes(Session& session, RaceIndex race) {
  Cosmos& cosmos = session.game->cosmos;
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    session.antag_ai.frontier[static_cast<std::size_t>(s)] = 0;
  }
  cosmos_flood_frontier_lanes(session, cosmos.stars[static_cast<std::size_t>(cosmos.races[race].home)], race);
}

// Returns the star a ship counts at for the planner: its star in a system
// or in orbit, its destination star on a lane; `kNone` otherwise.
StarIndex ship_planning_star(const Cosmos& cosmos, const Ship& ship) {
  switch (ship.location_kind) {
    case ShipLocation::kSystem:
      return ship.location;
    case ShipLocation::kOrbit:
      return cosmos.planets[static_cast<std::size_t>(ship.location)].star;
    case ShipLocation::kLane:
      return ship.destination_kind == ShipOrder::kGoStar ? ship.destination : kNone;
    default:
      return kNone;
  }
}

// `Race_ai_evaluate_stars` ANTAG 0x3c2e8.
void race_ai_evaluate_stars(Session& session, Race& race, const std::vector<ShipIndex>& ships) {
  Cosmos& cosmos = session.game->cosmos;
  Session::AntagAiWork& work = session.antag_ai;
  const std::uint8_t war = race_at_war_mask(session, race);
  // peCustom: the frontier stars with ships of races it sees as a danger.
  const std::uint8_t threat = session.game->variant == Variant::kPeCustom ? race_danger_mask(cosmos, race) : war;
  cosmos_count_frontier_lanes(session, race.index);
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    const auto us = static_cast<std::size_t>(s);
    const Star& star = cosmos.stars[us];
    work.colonise[us] = star_ai_colonise_value(session, star, race.index);
    work.defence[us] = star_ai_defence_value(session, star, race.index, ships);
    if (work.frontier[us] > 0 && (threat & star.ships_mask) != 0) {
      work.defence[us] = work.defence[us] * 2 + 300 + work.frontier[us] * 150;
    }
    work.war_ships[us] = 0;
    work.invasion_ships[us] = 0;
    work.colony_ships[us] = 0;
  }
  for (const ShipIndex i : ships) {
    const Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    const StarIndex s = ship_planning_star(cosmos, ship);
    if (s > kNone && s < cosmos.star_count) {
      count_up(role_counter(work, ship)[static_cast<std::size_t>(s)]);
    }
  }
}

// `Race_ai_choose_colony_site` ANTAG 0x403c0.
PlanetIndex antag_race_ai_choose_colony_site(const Session& session, const Race& race, const Star& star) {
  const Cosmos& cosmos = session.game->cosmos;
  for (std::int16_t i = 0; i < cosmos.race_count; ++i) {
    const auto ui = static_cast<std::size_t>(i);
    if (cosmos.races[ui].extinct != -1 && i != race.index && race.relation[ui] == kAlliance &&
        (star.owner_mask & bit_of(static_cast<std::uint32_t>(i))) != 0) {
      return kNone;
    }
  }
  std::uint16_t best = 0;
  PlanetIndex site = kNone;
  for (std::int16_t k = 0; k < star.planet_count; ++k) {
    const PlanetIndex p = star.planets[static_cast<std::size_t>(k)];
    const Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
    if (planet.owner != kNoOwner) {
      continue;
    }
    auto score = static_cast<std::uint16_t>(planet_habitability(cosmos, planet, race.ability == 2));
    if (planet.flags & kPlanetRuins) {
      score = static_cast<std::uint16_t>(score + 40);
    }
    if (best < score) {
      site = p;
      best = score;
    }
  }
  std::uint16_t minimum = 70;
  if (star.owner_mask == 0) {
    minimum = 35;
    const StarIndex here = star_index(cosmos, star);
    for (std::int16_t k = 0; k < star.lane_count; ++k) {
      const Lane& lane = cosmos.lanes[static_cast<std::size_t>(star.lanes[static_cast<std::size_t>(k)])];
      const Star& far = cosmos.stars[static_cast<std::size_t>(lane.star_a == here ? lane.star_b : lane.star_a)];
      if (far.owner_mask & bit_of(race.index)) {
        minimum = 17;
        break;
      }
    }
  } else {
    for (std::int16_t i = 0; i < cosmos.race_count; ++i) {
      const auto ui = static_cast<std::size_t>(i);
      if (race.relation[ui] == kWar && (star.owner_mask & bit_of(static_cast<std::uint32_t>(i))) != 0 &&
          (star.owner_mask & bit_of(race.index)) == 0) {
        minimum = 0;
        break;
      }
    }
  }
  return best <= minimum ? kNone : site;
}

// `Ship_obsolescence` ANTAG 0x4a1cc: the mean level deficit of the parts
// against the race's best. Original bug, fixed in peCustom: the category is
// read from the gizmo whose id is the slot number, not from the installed
// gizmo.
std::int32_t ship_obsolescence(const Session& session, const Ship& ship, RaceIndex race) {
  const auto& gizmos = session.tables->gizmos;
  const bool pe_custom = session.game->variant == Variant::kPeCustom;
  // The level of a best gizmo; none (-1) reads the bytes before the table,
  // taken as 0.
  const auto level_of = [&](std::int8_t id) { return id < 0 ? 0 : gizmos.at(static_cast<std::uint8_t>(id)).level; };
  const std::int8_t weapon = ship_best_gizmo_for_race(session, kCategoryWeapon, race);
  const std::int8_t generator = ship_best_gizmo_for_race(session, kCategoryGenerator, race);
  const std::int8_t drive = ship_best_gizmo_for_race(session, kCategoryDrive, race);
  const std::int8_t shield = ship_best_gizmo_for_race(session, kCategoryShield, race);
  const std::int8_t lane_drive =
      ship_race_has_gizmo(session, ship, kGizmoStarLaneHyperdrive) ? kGizmoStarLaneHyperdrive : kGizmoStarLaneDrive;
  std::int32_t deficit = 0;
  std::int32_t n = 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const std::int8_t g = ship.slots[static_cast<std::size_t>(i)].gizmo;
    if (g == -1) {
      continue;
    }
    std::int32_t best = -1;
    switch (gizmos.at(pe_custom ? static_cast<std::size_t>(static_cast<std::uint8_t>(g)) : static_cast<std::size_t>(i))
                .category) {
      case kCategoryWeapon:
        best = level_of(weapon);
        break;
      case kCategoryShield:
        best = level_of(shield);
        break;
      case kCategoryDrive:
        best = level_of(drive);
        break;
      case kCategoryGenerator:
        best = level_of(generator);
        break;
      default:
        break;
    }
    if (g == kGizmoStarLaneDrive && lane_drive == kGizmoStarLaneHyperdrive) {
      ++n;
      ++deficit;
    } else if (best != -1) {
      ++n;
      const std::int32_t mine = gizmos.at(static_cast<std::uint8_t>(g)).level;
      if (mine < best) {
        deficit += best - mine;
      }
    }
  }
  return n < 1 ? 0 : deficit * 10 / n;
}

// `Race_ai_ship_is_obsolete` ANTAG 0x3c12c.
bool race_ai_ship_is_obsolete(const Session& session, const Race& race, const Ship& ship) {
  const std::int8_t best = ship_best_gizmo_for_race(session, kCategoryWeapon, race.index);
  const std::int32_t best_damage =
      best < 0 || best > 75 ? 0 : session.tables->gizmos.at(static_cast<std::uint8_t>(best)).weapon_damage;
  if ((ship.slot_count - ship.gizmo_count) * 3 <= ship.slot_count &&
      (ship.has_colonizer != 0 || ship_has_gizmo(ship, kGizmoInvasionModule) || best_damage <= ship.weapon_damage)) {
    if (ship_experience(session, ship) < 1) {
      return false;
    }
    if (ship_obsolescence(session, ship, race.index) < 15) {
      return false;
    }
  }
  return true;
}

// `Race_ai_scrap_obsolete_ships` ANTAG 0x3c1c0.
void race_ai_scrap_obsolete_ships(Session& session, Race& race, const std::vector<ShipIndex>& ships) {
  Cosmos& cosmos = session.game->cosmos;
  if (race_ship_allowance(cosmos, race) >= 2 || ships.empty() || !is_ai(session, race)) {
    return;
  }
  // The hull level is computed and asserted non-zero, then unused.
  for (const ShipIndex i : ships) {
    Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    if (ship.location_kind == ShipLocation::kBuilding || ship.location_kind == ShipLocation::kRefit ||
        ship.destination_kind == ShipOrder::kColonize || ship.destination_kind == ShipOrder::kDefendPlanet ||
        ship.destination_kind == ShipOrder::kAttack) {
      continue;
    }
    if (race_ai_ship_is_obsolete(session, race, ship)) {
      ship_destroy(session, ship);
    }
  }
}

// The hull fillers (ANTAG 0x3f324..0x40084). `p` holds the design's parts.
struct Parts {
  std::int8_t generator;
  std::int8_t drive;
  std::int8_t weapon;
  std::int8_t shield;
  std::int8_t lane;
};

class Filler {
 public:
  Filler(Session& session, Ship& ship) : session_(session), ship_(ship) {}
  void install(std::int8_t gizmo, std::int32_t slot) { ship_install_gizmo(session_, ship_, gizmo, slot); }
  bool known(std::int8_t gizmo) const { return ship_race_has_gizmo(session_, ship_, gizmo); }
  // `alt` when known and different from `fallback`, else `fallback`.
  std::int8_t better(std::int8_t alt, std::int8_t fallback) const {
    return fallback != alt && known(alt) ? alt : fallback;
  }
  std::int32_t roll() { return session_.rng.rand() % 100; }
  Ship& ship() { return ship_; }

 private:
  Session& session_;
  Ship& ship_;
};

// `Race_ai_fill_specials` ANTAG 0x40084.
void race_ai_fill_specials(Session& session, Ship& ship, std::int32_t slot, std::int8_t fallback) {
  Filler f(session, ship);
  std::array<std::int8_t, kSpecials.size()> list{};
  std::int16_t count = 0;
  for (const std::int8_t g : kSpecials) {
    if (f.known(g)) {
      list[static_cast<std::size_t>(count++)] = g;
    }
  }
  while (slot < ship.slot_count && count != 0) {
    const std::int32_t k = session.rng.rand() % count;
    --count;
    f.install(list[static_cast<std::size_t>(k)], slot);
    list[static_cast<std::size_t>(k)] = list[static_cast<std::size_t>(count)];
    ++slot;
  }
  for (; slot < ship.slot_count; ++slot) {
    f.install(fallback, slot);
  }
}

// `Race_ai_fill_small_hull` ANTAG 0x3f324.
void race_ai_fill_small_hull(Filler& f, const Parts& p, std::uint8_t role) {
  f.install(p.generator, 0);
  f.install(p.drive, 1);
  f.install(p.lane, 2);
  const std::int8_t cargo = role == kRoleColony     ? kGizmoColonizer
                            : role == kRoleInvasion ? kGizmoInvasionModule
                                                    : p.weapon;
  f.install(cargo, 3);
  f.install(cargo, 4);
}

// The random fills of the colony and invasion designs: from `slot` to
// `end`, one draw each.
void fill_colony(Filler& f, const Parts& p, std::int32_t slot, std::int32_t end) {
  for (; slot < end; ++slot) {
    const std::int32_t r = f.roll();
    f.install(r > 80   ? p.generator
              : r > 60 ? p.shield
              : r > 40 ? kGizmoInvasionModule
              : r > 20 ? kGizmoColonizer
                       : p.lane,
              slot);
  }
}
void fill_invasion(Filler& f, const Parts& p, std::int32_t slot, std::int32_t end) {
  for (; slot < end; ++slot) {
    const std::int32_t r = f.roll();
    f.install(r > 75 ? p.generator : r > 50 ? p.shield : r > 25 ? kGizmoInvasionModule : p.lane, slot);
  }
}

// `Race_ai_fill_medium_hull` ANTAG 0x3f3f8.
void race_ai_fill_medium_hull(Session& session, const Race& race, Filler& f, const Parts& p, std::uint8_t role) {
  f.install(p.generator, 0);
  f.install(p.generator, 1);
  f.install(p.drive, 2);
  f.install(p.lane, 3);
  std::int32_t n = 4;
  if (f.ship().generator_power < 8) {
    f.install(p.generator, 4);
    n = 5;
  }
  if (role == kRoleColony) {
    f.install(kGizmoColonizer, n);
    f.install(kGizmoColonizer, n + 1);
    f.install(kGizmoColonizer, n + 2);
    f.install(kGizmoInvasionModule, n + 3);
    f.install(p.lane, n + 4);
    for (std::int32_t slot = n + 5; slot < 10; ++slot) {
      const std::int32_t r = f.roll();
      f.install(r > 75 ? p.generator : r > 50 ? p.shield : r > 25 ? kGizmoColonizer : p.lane, slot);
    }
  } else if (role == kRoleInvasion) {
    f.install(f.better(kGizmoPlasmatron, p.weapon), n);
    f.install(kGizmoInvasionModule, n + 1);
    f.install(kGizmoInvasionModule, n + 2);
    f.install(kGizmoInvasionModule, n + 3);
    f.install(p.shield, n + 4);
    fill_invasion(f, p, n + 5, 10);
  } else {
    f.install(f.better(kGizmoPlasmatron, p.weapon), n);
    f.install(p.weapon, n + 1);
    f.install(p.weapon, n + 2);
    f.install(p.shield, n + 3);
    std::int32_t next = n + 4;
    if (f.known(kGizmoRecaller)) {
      f.install(kGizmoRecaller, next++);
    }
    const Cosmos& cosmos = session.game->cosmos;
    if (p.lane == kGizmoStarLaneDrive || cosmos.day > 800 || cosmos.ranks[race.index].rank > 2) {
      f.install(p.lane, next++);
    }
    f.install(p.shield, next);
    f.install(kGizmoInvasionModule, next + 1);
    for (std::int32_t slot = next + 2; slot < 10; ++slot) {
      const std::int32_t r = f.roll();
      f.install(r > 66 ? p.generator : r > 33 ? kGizmoInvasionModule : p.shield, slot);
    }
  }
}

// The war designs' tail of the large and enormous hulls: the specials known,
// then `Race_ai_fill_specials` with the generator.
void fill_war_tail(Session& session, Filler& f, const Parts& p, std::int32_t next, bool enormous) {
  if (f.known(kGizmoAccutron)) {
    f.install(kGizmoAccutron, next++);
  }
  if (f.known(kGizmoRecaller)) {
    f.install(kGizmoRecaller, next++);
  }
  if (f.known(kGizmoReplenisher)) {
    f.install(kGizmoReplenisher, next++);
  }
  f.install(kGizmoPhaseBomb, next++);
  if (enormous) {
    f.install(kGizmoPhaseBomb, next++);
    f.install(p.lane, next++);
  }
  if (f.known(kGizmoBrunswik)) {
    f.install(kGizmoBrunswik, next++);
  } else if (f.known(kGizmoMolecularTieDown)) {
    f.install(kGizmoMolecularTieDown, next++);
  }
  race_ai_fill_specials(session, f.ship(), next, p.generator);
}

// `Race_ai_fill_large_hull` ANTAG 0x3f784.
void race_ai_fill_large_hull(Session& session, Filler& f, const Parts& p, std::uint8_t role) {
  f.install(p.generator, 0);
  f.install(p.generator, 1);
  f.install(p.generator, 2);
  f.install(p.drive, 3);
  f.install(p.lane, 4);
  f.install(p.lane, 5);
  std::int32_t n = 6;
  if (f.ship().generator_power < 16) {
    f.install(p.generator, 6);
    n = 7;
  }
  if (role == kRoleColony) {
    for (std::int32_t k = 0; k < 4; ++k) {
      f.install(kGizmoColonizer, n + k);
    }
    f.install(kGizmoInvasionModule, n + 4);
    f.install(p.shield, n + 5);
    f.install(p.lane, n + 6);
    fill_colony(f, p, n + 7, 15);
  } else if (role == kRoleInvasion) {
    f.install(f.better(kGizmoPlasmatron, p.weapon), n);
    for (std::int32_t k = 1; k <= 4; ++k) {
      f.install(kGizmoInvasionModule, n + k);
    }
    f.install(p.shield, n + 5);
    f.install(p.lane, n + 6);
    fill_invasion(f, p, n + 7, 15);
  } else {
    f.install(p.weapon, n);
    f.install(f.better(kGizmoFergnatzLens, p.weapon), n + 1);
    f.install(f.better(kGizmoPlasmatron, p.weapon), n + 2);
    f.install(p.shield, n + 3);
    std::int32_t next = n + 4;
    if (p.lane == kGizmoStarLaneDrive) {
      f.install(kGizmoStarLaneDrive, next++);
    }
    f.install(kGizmoInvasionModule, next++);
    fill_war_tail(session, f, p, next, false);
  }
}

// `Race_ai_fill_enormous_hull` ANTAG 0x3fbac.
void race_ai_fill_enormous_hull(Session& session, Filler& f, const Parts& p, std::uint8_t role) {
  for (std::int32_t k = 0; k < 4; ++k) {
    f.install(p.generator, k);
  }
  f.install(p.drive, 4);
  f.install(p.drive, 5);
  f.install(p.lane, 6);
  f.install(p.lane, 7);
  f.install(p.shield, 8);
  std::int32_t n = 9;
  if (f.ship().generator_power < 24) {
    f.install(p.generator, 9);
    n = 10;
  }
  if (p.lane == kGizmoStarLaneDrive) {
    f.install(kGizmoStarLaneDrive, n++);
  }
  if (role == kRoleColony) {
    for (std::int32_t k = 0; k < 5; ++k) {
      f.install(kGizmoColonizer, n + k);
    }
    f.install(kGizmoInvasionModule, n + 5);
    f.install(p.shield, n + 6);
    fill_colony(f, p, n + 7, 25);
  } else if (role == kRoleInvasion) {
    f.install(f.better(kGizmoPlasmatron, p.weapon), n);
    for (std::int32_t k = 1; k <= 7; ++k) {
      f.install(kGizmoInvasionModule, n + k);
    }
    f.install(p.shield, n + 8);
    f.install(p.lane, n + 9);
    fill_invasion(f, p, n + 10, 25);
  } else {
    f.install(p.weapon, n);
    f.install(f.better(kGizmoFergnatzLens, p.weapon), n + 1);
    f.install(f.better(kGizmoPlasmatron, p.weapon), n + 2);
    f.install(f.better(kGizmoHypersphereDriver, p.weapon), n + 3);
    f.install(kGizmoInvasionModule, n + 4);
    f.install(kGizmoInvasionModule, n + 5);
    fill_war_tail(session, f, p, n + 6, true);
  }
}

// `Race_ai_design_ship` ANTAG 0x3f060: the standard hull choice and parts,
// the filler of the hull with the planner's role.
void antag_race_ai_design_ship(Session& session, Race& race, Ship& ship, Planet& planet, std::uint8_t role) {
  const Cosmos& cosmos = session.game->cosmos;
  const Research& research = session.game->research;
  std::int16_t hull = 0;
  for (const std::uint16_t item : kHullResearch) {
    if (!knows(research, item, race.index)) {
      break;
    }
    ++hull;
  }
  std::int16_t k = static_cast<std::int16_t>(cosmos.day / 250);
  k = k < 1 ? 1 : k > 8 ? 8 : k;
  for (; hull >= 0; --hull) {
    ship_set_hull_size(session, ship, static_cast<std::uint8_t>(hull));
    const std::int32_t cost = ship_industry_cost(session, ship);
    if (planet.industry == 0) {
      planet.industry = 1;  // written to the planet (original side effect)
    }
    if ((k * ship.slot_count + cost) / static_cast<std::uint16_t>(planet.industry) + 1 < race.ship_patience) {
      break;
    }
  }
  if (hull < 0) {
    hull = 0;
  }
  ship_set_hull_size(session, ship, static_cast<std::uint8_t>(hull));
  Parts p{};
  p.weapon = ship_best_gizmo_for_race(session, kCategoryWeapon, race.index);
  p.generator = ship_best_gizmo_for_race(session, kCategoryGenerator, race.index);
  p.drive = ship_best_gizmo_for_race(session, kCategoryDrive, race.index);
  p.shield = ship_best_gizmo_for_race(session, kCategoryShield, race.index);
  p.lane =
      ship_race_has_gizmo(session, ship, kGizmoStarLaneHyperdrive) ? kGizmoStarLaneHyperdrive : kGizmoStarLaneDrive;
  if (p.generator == -1) {
    p.generator = kGizmoProtonShaver;
  }
  if (p.drive == -1) {
    p.drive = kGizmoTonklinMotor;
  }
  if (p.weapon == -1) {
    p.weapon = kGizmoMassBarrage;
  }
  if (p.shield == -1) {
    p.shield = kGizmoIonWrap;
  }
  Filler f(session, ship);
  switch (hull) {
    case 0:
      race_ai_fill_small_hull(f, p, role);
      break;
    case 1:
      race_ai_fill_medium_hull(session, race, f, p, role);
      break;
    case 2:
      race_ai_fill_large_hull(session, f, p, role);
      break;
    default:
      race_ai_fill_enormous_hull(session, f, p, role);
      break;
  }
}

// `Race_ai_start_ship_build` ANTAG 0x3cb60: a Shipyard where there is none
// (dead in practice), else a ship of `role`; returns 0, 1, or 2.
std::int32_t race_ai_start_ship_build(Session& session, Race& race, Planet& planet, std::uint8_t role) {
  const Cosmos& cosmos = session.game->cosmos;
  if (planet.industry == 0) {
    return 0;
  }
  if (planet_find_item(cosmos, planet, kItemShipyard) == kNoSquare) {
    const std::uint8_t req = session.tables->planitems.items[kItemShipyard].research_requirement;
    if ((cosmos.day > 170 || cosmos.atmosphere == Atmosphere::kHostile) &&
        knows(session.game->research, req, race.index)) {
      return planet_auto_place_item(session, planet, kItemShipyard, 0) == 0 ? 2 : 1;
    }
    return 0;
  }
  if (!planet_can_build_ship(session, planet) || (planet.owner == session.player_race && !session.autoplay)) {
    return 0;
  }
  const std::uint16_t square = planet_auto_place_item(session, planet, kItemShip, 0);
  if (square == kNoSquare) {
    return 0;
  }
  Ship* ship = race_ai_new_ship(session, race, planet);
  antag_race_ai_design_ship(session, race, *ship, planet, role);
  planet_set_square_ship(session, planet, square, ship_index(session.game->cosmos, *ship));
  return 1;
}

// The production planner of `Race_ai_turn` ANTAG 0x3b5b8 (step 7): the role
// quotas, the demand per own star, the shipyards and their orders.
void race_ai_plan_production(Session& session, Race& race, const std::vector<ShipIndex>& ships) {
  Cosmos& cosmos = session.game->cosmos;
  Session::AntagAiWork& w = session.antag_ai;
  std::int16_t n = wrap_i16(race_ship_allowance(cosmos, race));
  if (n > 10) {
    n = 10;
  }
  if (n <= 0) {
    return;
  }
  const std::uint8_t war = race_at_war_mask(session, race);
  float invasion = 0.0f;
  float warships = 0.0f;
  float colony = 0.0f;
  const auto add = [](float& total, std::int32_t v) { total = f32(static_cast<double>(v) + total); };
  // Arithmetic shift with the CPU's 5-bit count.
  const auto sar = [](std::int32_t v, std::int32_t count) { return v >> (count & 31); };
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    const auto us = static_cast<std::size_t>(s);
    const Star& star = cosmos.stars[us];
    const std::int32_t ic = w.invasion_ships[us];
    const std::int32_t wc = w.war_ships[us];
    if (war & star.owner_mask) {
      add(invasion, ic > 1 ? sar(w.colonise[us], ic - 1) : w.colonise[us]);
      const std::int32_t v = w.defence[us] + w.colonise[us];
      add(warships, wc > 0 ? sar(v, wc) : v);
    } else if (war & star.ships_mask) {
      add(warships, wc > 0 ? sar(w.defence[us], wc) : w.defence[us]);
    } else {
      if (w.colony_ships[us] <= 0) {
        add(colony, w.colonise[us]);
      }
      add(warships, wc > 0 ? sar(w.defence[us], wc) : w.defence[us]);
    }
  }
  // The float 0.05 (0x920f0) and the double 2/3 (0x920f4).
  const double kTwoThirds = 2.0 / 3.0;
  colony = f32(static_cast<double>(colony) * 0.05f);
  const double invasion_ext = static_cast<double>(invasion) * kTwoThirds;
  invasion = f32(invasion_ext);
  const float total = f32(invasion_ext + warships + colony);
  if (total > 0.0f) {
    const double count = n;
    invasion = f32(invasion * count);
    warships = f32(warships * count);
    colony = f32(count * colony);
    const double inverse = 1.0 / total;
    invasion = f32(invasion * inverse);
    warships = f32(warships * inverse);
    colony = f32(inverse * colony);
  } else {
    colony = f32(n / 3);
    warships = colony;
    invasion = f32((static_cast<double>(n) - colony) - colony);
  }
  // Ships already in production count against the quotas.
  const auto take = [](float& quota) { quota = f32(static_cast<double>(quota) - 1.0); };
  for (const ShipIndex i : ships) {
    const Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    if (ship.location_kind != ShipLocation::kBuilding) {
      continue;
    }
    if (ship.has_colonizer != 0 && colony > 0.0f) {
      take(colony);
    } else if (ship_has_gizmo(ship, kGizmoInvasionModule) && invasion > 0.0f) {
      take(invasion);
    } else if (warships > 0.0f) {
      take(warships);
    }
  }
  w.colony_demand.fill(0);
  w.war_demand.fill(0);
  w.invasion_demand.fill(0);
  for (std::int16_t i = 0; i < n; ++i) {
    w.yard_planet[static_cast<std::size_t>(i)] = -1;
  }
  const std::uint8_t me = bit_of(race.index);
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    const auto us = static_cast<std::size_t>(s);
    const Star& own = cosmos.stars[us];
    if ((own.owner_mask & me) == 0) {
      continue;
    }
    for (std::int16_t t = 0; t < cosmos.star_count; ++t) {
      const auto ut = static_cast<std::size_t>(t);
      if ((cosmos.stars[ut].lanes_known_mask & me) == 0) {
        continue;
      }
      // `Star_hop_count` 0x1da04: no decay below 25 hops (kept).
      const std::int32_t hops = cosmos.hops[ut][us];
      const float weight = static_cast<float>(1 / (hops / 25 + 1));
      // Original bug, fixed in peCustom: the branch tests the own star's
      // masks, not t's.
      const Star& tested = session.game->variant == Variant::kPeCustom ? cosmos.stars[ut] : own;
      if (war & tested.owner_mask) {
        const std::int32_t v = chop(static_cast<double>(w.colonise[ut]) * weight);
        if (w.invasion_ships[ut] < 1) {
          w.invasion_demand[us] += v;
        } else if (w.invasion_ships[ut] < 2) {
          w.invasion_demand[us] += v / 2;
        }
        if (w.war_ships[ut] < 1) {
          w.war_demand[us] += v;
        }
      } else if (war & tested.ships_mask) {
        const std::int32_t v =
            chop(static_cast<double>(wrap_i32(static_cast<std::int64_t>(w.defence[ut]) + w.colonise[ut])) * weight);
        if (w.war_ships[ut] < 1) {
          w.war_demand[us] += v;
        }
      } else {
        if (w.colony_ships[ut] < 1) {
          w.colony_demand[us] = chop(static_cast<double>(w.colonise[ut]) * weight * static_cast<double>(0.05f) +
                                     static_cast<double>(w.colony_demand[us]));
        }
        const std::int32_t v = chop(static_cast<double>(w.defence[ut]) * weight);
        if (w.war_ships[ut] < 1) {
          w.war_demand[us] += v;
        }
      }
    }
  }
  // The shipyards: the best scores, empty slots first.
  for (std::int16_t pi = 0; pi < cosmos.planet_count; ++pi) {
    const Planet& planet = cosmos.planets[static_cast<std::size_t>(pi)];
    if (planet.project_item != kNoItem || planet_find_item(cosmos, planet, kItemShipyard) == kNoSquare ||
        planet.owner != race.index) {
      continue;
    }
    const auto s = static_cast<std::size_t>(planet.star);
    const std::int32_t score = (w.colony_demand[s] + w.war_demand[s] + w.invasion_demand[s]) / 10 *
                               static_cast<std::uint16_t>(planet.industry);
    std::size_t slot = 0;
    std::int32_t lowest = w.yard_score[0];
    for (std::int16_t i = 0; i < n; ++i) {
      const auto ui = static_cast<std::size_t>(i);
      if (w.yard_planet[ui] == -1) {
        slot = ui;
        break;
      }
      if (w.yard_score[ui] < lowest) {
        lowest = w.yard_score[ui];
        slot = ui;
      }
    }
    if (w.yard_score[slot] < score || w.yard_planet[slot] == -1) {
      w.yard_planet[slot] = pi;
      w.yard_score[slot] = score;
    }
  }
  for (std::int16_t i = 0; i < n; ++i) {
    const std::int32_t pi = w.yard_planet[static_cast<std::size_t>(i)];
    if (pi < 0 || pi >= cosmos.planet_count) {
      continue;
    }
    Planet& planet = cosmos.planets[static_cast<std::size_t>(pi)];
    const auto s = static_cast<std::size_t>(planet.star);
    if (colony > 0.0f && w.war_demand[s] < w.colony_demand[s] && w.invasion_demand[s] < w.colony_demand[s]) {
      race_ai_start_ship_build(session, race, planet, kRoleColony);
      take(colony);
    } else if (warships > 0.0f && w.war_demand[s] > w.invasion_demand[s]) {
      race_ai_start_ship_build(session, race, planet, kRoleWar);
      take(warships);
    } else if (invasion > 0.0f) {
      race_ai_start_ship_build(session, race, planet, kRoleInvasion);
      take(invasion);
    } else if (colony > 0.0f) {
      race_ai_start_ship_build(session, race, planet, kRoleColony);
      take(colony);
    } else if (warships > 0.0f) {
      race_ai_start_ship_build(session, race, planet, kRoleWar);
      take(warships);
    }
  }
}

// `Race_ai_turn` ANTAG 0x3b5b8.
void antag_race_ai_turn(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  const std::vector<ShipIndex> ships = race_list_ships(cosmos, race);
  const bool ai = is_ai(session, race);
  if (ai) {
    race_update_attitude_drift(session, race);
    race_bio_rhythm(session, race);
    race_diplomacy_turn(session, race);
    race_ai_evaluate_stars(session, race, ships);
  }
  if (!antag_race_research_complete(session, race) &&
      (race.index != session.player_race || session.autoplay || session.game->research.self_managed == -1) &&
      session.game->research.project[race.index] == -1) {
    race_ai_choose_research(session, race);
  }
  if (ai) {
    race_use_special_ability(session, race, true);
    for (const ShipIndex i : ships) {
      antag_race_ai_ship_orders(session, race, cosmos.ships[static_cast<std::size_t>(i)]);
    }
    race_ai_plan_production(session, race, ships);
  }
  for (std::int16_t i = 0; i < cosmos.planet_count; ++i) {
    Planet& planet = cosmos.planets[static_cast<std::size_t>(i)];
    if (planet.owner == race.index &&
        (race.index != session.player_race || planet.self_managed != 0 || session.autoplay) &&
        planet.project_item == kNoItem) {
      if (session.game->variant == Variant::kPeCustom) {
        pe_manage_planet(session, race, planet);
      } else {
        antag_race_ai_manage_planet(session, race, planet);
      }
    }
  }
}

}  // namespace

void antag_race_ai_ship_orders(Session& session, Race& race, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  Session::AntagAiWork& w = session.antag_ai;
  if (ship.location_kind == ShipLocation::kBuilding || ship.location_kind == ShipLocation::kRefit ||
      ship.location_kind == ShipLocation::kLane) {
    return;
  }
  if (ship.owner == session.player_race && !session.autoplay) {
    ship.destination_kind = ShipOrder::kNothing;
    ship.destination = kNone;
    ship.stale_destination = 0;
    return;
  }
  // Re-planning: a ship in a system heading for a star leaves the count of
  // its role at its current star and gets new orders.
  if (ship.location_kind == ShipLocation::kSystem && ship.destination_kind == ShipOrder::kGoStar) {
    std::int8_t& counter = role_counter(w, ship)[static_cast<std::size_t>(ship.location)];
    if (counter > 0) {
      counter = wrap_i8(counter - 1);
    }
    ship.destination_kind = ShipOrder::kNothing;
    ship.destination = kNone;
    ship.stale_destination = 0;
  }
  if (ship.destination_kind != ShipOrder::kNothing) {
    return;
  }
  if (ship.location_kind == ShipLocation::kOrbit) {
    planet_undock_ship(session, cosmos.planets[static_cast<std::size_t>(ship.location)], ship);
  }
  const StarIndex here = ship.location;
  const Star& current = cosmos.stars[static_cast<std::size_t>(here)];
  if (ship.has_colonizer == -1) {
    const PlanetIndex site = antag_race_ai_choose_colony_site(session, race, current);
    if (site != kNone) {
      ship.destination_kind = ShipOrder::kColonize;
      ship.destination = site;
      return;
    }
  }
  if (ship.lane_speed <= 0 && ship.hyper_speed <= 0) {
    return;
  }
  const bool invader = ship_has_gizmo(ship, kGizmoInvasionModule);
  const std::uint8_t war = race_at_war_mask(session, race);
  float best = 0.0f;
  StarIndex chosen = kNone;
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    const auto us = static_cast<std::size_t>(s);
    if (w.colonise[us] <= 0 && w.defence[us] <= 0) {
      continue;
    }
    const Star& star = cosmos.stars[us];
    // Heads the ship there (none for the current star; kept: the last star
    // scored stays the destination when no other wins).
    const std::uint8_t hops = ship_set_destination_star(session, ship, star);
    const std::uint8_t own = star.owner_mask;
    const std::uint8_t shp = star.ships_mask;
    // The value, stored as a float, then divided by 2^count (stored again).
    float v = 0.0f;
    std::int32_t shift = 0;
    if ((war & (own | shp)) == 0) {
      if (ship.has_colonizer != 0) {
        v = w.colony_ships[us] > 0 ? 0.0f : static_cast<float>(w.colonise[us]);
      } else if (!invader) {
        v = static_cast<float>(w.defence[us]);
        shift = w.war_ships[us];
      }
    } else if (ship.has_colonizer == 0) {
      if (!invader) {
        v = static_cast<float>((war & own) == 0 ? w.defence[us] : w.colonise[us] + w.defence[us]);
        shift = w.war_ships[us];
      } else if ((war & own) != 0) {
        v = static_cast<float>(w.colonise[us]);
        shift = w.invasion_ships[us] - 1;
      }
    } else if ((war & own) != 0 && (shp & war) == 0 && w.colony_ships[us] < 1) {
      v = static_cast<float>(w.colonise[us]);
    }
    if (shift > 0) {
      v = f32(static_cast<double>(v) / static_cast<double>(1u << (shift & 0x1f)));
    }
    const double score = static_cast<double>(v) / static_cast<double>(hops / 25 + 1);
    if (best < score) {
      chosen = s;
      best = f32(score);
    }
  }
  if (chosen != kNone && chosen != here &&
      ship_set_destination_star(session, ship, cosmos.stars[static_cast<std::size_t>(chosen)]) != 0) {
    count_up(role_counter(w, ship)[static_cast<std::size_t>(chosen)]);
  }
}

void antag_race_turn_update(Session& session, Race& race) {
  Cosmos& cosmos = session.game->cosmos;
  if (race.extinct == -1) {
    return;
  }
  const std::int32_t allowance = race_ship_allowance(cosmos, race);
  if (allowance < 0) {
    const std::vector<ShipIndex> ships = race_list_ships(cosmos, race);
    const std::int32_t last = static_cast<std::int32_t>(ships.size()) - 1;
    for (std::int16_t i = 0; i < -allowance; ++i) {
      Ship& ship = cosmos.ships.at(static_cast<std::size_t>(ships.at(static_cast<std::size_t>(last - i))));
      if (race.index == session.player_race) {
        session.post_event(EventCode::kShipScrapped, ship.hull_size);
      }
      ship_destroy(session, ship);
    }
  }
  race_ai_scrap_obsolete_ships(session, race, race_list_ships(cosmos, race));
  const std::vector<ShipIndex> ships = race_list_ships(cosmos, race);
  if (ships.empty() && race_count_planets(cosmos, race) == 0) {
    race.extinct = -1;
    session.post_event(EventCode::kRaceExtinct, race.index);
    return;
  }
  for (const ShipIndex i : ships) {
    ship_new_day(session, cosmos.ships[static_cast<std::size_t>(i)]);
  }
  session.research_complete_valid = false;
  antag_race_ai_turn(session, race);
  antag_race_ai_demolish_research_buildings(session, race);
  if ((cosmos.stars.at(static_cast<std::size_t>(race.home)).owner_mask & 0x80) == 0) {
    return;
  }
  race.ability_charge = wrap_i32(static_cast<std::int64_t>(race.ability_charge) + 1);
  if (race.index == session.player_race && race.ability_charge != 0 &&
      kAbilityPeriod.at(race.ability) == static_cast<std::uint32_t>(race.ability_charge)) {
    session.post_event(EventCode::kAbilityCharged, 0);
  }
}

}  // namespace oa::game
