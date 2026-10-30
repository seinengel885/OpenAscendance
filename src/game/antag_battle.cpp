#include "game/antag_battle.h"

#include <array>
#include <vector>

#include "core/fixed_width.h"
#include "game/antag_ai.h"
#include "game/battle_helpers.h"
#include "game/cosmos.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/ship.h"
#include "game/tables.h"

namespace oa::game {

using namespace battle_detail;

namespace {

// ANTAG 0x96960 / 0x9696c: the standard evasive list without the Recaller
// (36), the target kinds not shifted (original bug, fixed in peCustom by
// `kPeCustomEvasiveTargets`: the Brunswik Dissipator gets the Recaller's
// "untargeted" kind, Molecular Tie Down the Dissipator's, the final 0 the
// Tie Down's).
constexpr std::array<std::int8_t, 10> kEvasiveGizmos = {69, 63, 50, 46, 45, 38, 37, 35, 33, 0};
constexpr std::array<std::uint8_t, 10> kEvasiveTargets = {kTargetNone, kTargetNone, kTargetShip, kTargetNone,
                                                          kTargetShip, kTargetNone, kTargetShip, kTargetNone,
                                                          kTargetShip, kTargetShip};
constexpr std::array<std::uint8_t, 10> kPeCustomEvasiveTargets = {kTargetNone, kTargetNone, kTargetShip, kTargetNone,
                                                                  kTargetShip, kTargetNone, kTargetShip, kTargetShip,
                                                                  kTargetShip, kTargetShip};

constexpr std::int8_t kGizmoPhaseBomb = 0x46;
constexpr std::int8_t kGizmoCarbonizer = 0x2f;
// A surface Tractor Beam only fires at ships at least this far (float
// 0x92334).
constexpr float kTractorMinimum = 1440.0f;
// The share of a weapon's range the AI stands off at (doubles 0x9275b,
// 0x91e43; 0x92763 for the Carbonizer) and the margin outside the enemy's
// reach (double 0x9232c).
constexpr double kReachShare = 0.95;
constexpr double kCarbonizerShare = 0.825;
constexpr double kEnemyReachMargin = 1.2;

// A ship that fights: not a colony ship, or one carrying an Invasion Module.
bool eligible(const Ship& ship) { return ship.has_colonizer == 0 || carries(ship, kGizmoInvasionModule); }

// Each built weapon of each own planet (surface ones too: the Tractor Beam,
// only when the planet counts a weapon) aims at the nearest hostile ship
// and the first one able to fire is chosen; a Tractor Beam only at 1440 or
// more (the planet loops of 0x41268).
bool planets_fire(Session& session, const BattleLists& lists, const std::vector<ShipIndex>& present,
                  std::uint8_t hostile_mask, BattleAction& action) {
  Cosmos& cosmos = session.game->cosmos;
  const PlanItemTable& items = session.tables->planitems;
  // The original's target variable is not reset between squares (a target
  // is always found: some hostile ship is in space).
  ShipIndex target = kNone;
  for (const PlanetIndex p : lists.planets) {
    Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
    for (std::uint16_t sq = 0; sq < planet.square_count; ++sq) {
      const std::int32_t square = planet.first_square + sq;
      const Square& s = cosmos.squares[static_cast<std::size_t>(square)];
      if (s.item == kNoItem || (s.flags & kSquareBuilt) == 0 || (items.items.at(s.item).flags & kItemWeapon) == 0 ||
          (s.item == kItemTractorBeam && planet.weapon_count == 0)) {
        continue;
      }
      float best = kFarAway;
      for (const ShipIndex i : present) {
        const Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
        if (!hostile_owner(hostile_mask, ship)) {
          continue;
        }
        const float d = target_distance(planet.position, ship.position, ship);
        if (d < best) {
          best = d;
          target = i;
        }
      }
      if ((s.item != kItemTractorBeam || kTractorMinimum <= best) && target != kNone) {
        planet.fire_square = square_address(session, square);
        planet.fire_target = ship_address(session, target);
        planet_fire_weapon(session, planet, false);
        if (planet.fire_result & 1) {
          action = {BattleActor::kPlanet, p};
          return true;
        }
      }
    }
  }
  return false;
}

// A move order to `base + v`, `v = from - base + (rand() % m + a, ...)` (x,
// y, z drawn in that order) set to the length `reach` when it is not
// negative.
void stand_off(Session& session, Ship& mover, const Vec3& base, std::int32_t m, std::int32_t a, float reach) {
  Vec3 v = sub(mover.position, base);
  Vec3 r{};
  for (float& c : r) {
    c = static_cast<float>(session.rng.rand() % m + a);
  }
  for (std::size_t i = 0; i < 3; ++i) {
    v[i] = f32(static_cast<double>(v[i]) + r[i]);
  }
  if (0.0f <= reach) {
    set_length(v, reach);
  }
  set_action(mover, 0, 0, kTargetPoint, 0);
  for (std::size_t i = 0; i < 3; ++i) {
    mover.target_point[i] = f32(static_cast<double>(v[i]) + base[i]);
  }
}

// The hostile planet attack (own ships, no hostile ship in space): every
// hostile planet of the star in orbit order is considered.
std::int32_t attack_planets(Session& session, Race& race, Star& star, const BattleLists& lists, BattleAction& action) {
  Cosmos& cosmos = session.game->cosmos;
  const StarIndex here = star_index(cosmos, star);
  for (std::int16_t k = 0; k < star.planet_count; ++k) {
    const PlanetIndex p = star.planets[static_cast<std::size_t>(k)];
    Planet& planet = cosmos.planets[static_cast<std::size_t>(p)];
    const std::uint8_t owner = planet.owner;
    if (owner == kNoOwner || owner == race.index ||
        !(race.relation[owner] == kWar ||
          (owner == session.player_race &&
           (session.star_day_flags[static_cast<std::size_t>(here)] & (1u << (race.index & 0x1f))) != 0))) {
      continue;
    }
    // Invade with an own ship docked in its orbit.
    for (std::uint16_t sq = planet.surface_squares; sq < planet.square_count; ++sq) {
      if (cosmos.squares[static_cast<std::size_t>(planet.first_square + sq)].item != kItemShip) {
        continue;
      }
      Ship& docked = cosmos.ships[static_cast<std::size_t>(planet_square_ship(session, planet, sq))];
      if (docked.owner == race.index) {
        std::int32_t used = 0;
        planet_invade(session, planet, sq, used);
        planet_undock_ship(session, planet, docked);
        return -1;
      }
    }
    const RawAddress planet_raw = planet_address(session, p);
    const bool guarded = planet_has_orbital_guard(session, planet);
    if (!guarded) {
      // The first ship in space with an Invasion Module goes into its orbit.
      for (const ShipIndex i : lists.ships) {
        Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
        if (ship.location_kind == ShipLocation::kSystem && carries(ship, kGizmoInvasionModule)) {
          action = ship_action(cosmos, ship);
          set_action(ship, 0, 0, kTargetPlanet, planet_raw);
          return -1;
        }
      }
      // The first fighting ship in space with a Phase Bomb, not travelling
      // to this very star, bombs it (or nobody does).
      for (const ShipIndex i : lists.ships) {
        Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
        if (!eligible(ship) || (ship.destination_kind == ShipOrder::kGoStar && ship.destination == here) ||
            ship.location_kind != ShipLocation::kSystem || !carries(ship, kGizmoPhaseBomb)) {
          continue;
        }
        if (ship_ai_select_weapon(session, ship, kSelectPlanet, planet_raw) != -1) {
          action = ship_action(cosmos, ship);
          return -1;
        }
        break;
      }
    }
    const bool orbit_used = planet.free_orbital < planet.square_count - planet.surface_squares;
    if (!guarded && !orbit_used) {
      continue;
    }
    // Attacked: the first armed (or Phase Bomb) fighting ship in space able
    // to bombard it fires, else the last one tried stands off.
    Ship* mover = nullptr;
    for (const ShipIndex i : lists.ships) {
      Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
      if (!eligible(ship) || ship.location_kind != ShipLocation::kSystem ||
          (!ship_has_ready_weapon(session, ship) && !carries(ship, kGizmoPhaseBomb))) {
        continue;
      }
      mover = &ship;
      if (ship_ai_select_weapon(session, ship, kSelectPlanet, planet_raw) != -1) {
        action = ship_action(cosmos, ship);
        return -1;
      }
    }
    if (mover == nullptr) {
      return 0;
    }
    float reach = static_cast<float>(ship_ai_engage_distance(session, *mover, 2));
    if (0.0f > reach) {
      reach = f32(static_cast<double>(planet_ai_orbital_weapon_reach(session, planet)) * kEnemyReachMargin);
    }
    action = ship_action(cosmos, *mover);
    stand_off(session, *mover, planet.position, 100, 20, reach);
    return -1;
  }
  return 0;
}

}  // namespace

bool antag_race_battle_turn(Session& session, Race& race, Star& star, BattleAction& action) {
  static constexpr BattleTurnRules kAntagonizer = {
      .tactical = antag_race_ai_tactical,
      .ship_orders = antag_race_ai_ship_orders,
      .recall_at_any_threat = true,
      .tactical_zero_continues = true,
      .only_invasion_ships_wait = true,
  };
  return race_battle_turn_with(session, race, star, action, kAntagonizer);
}

// ANTAG 0x41268. Its first calls (`Race_count_ships_at_star` for both
// relations, `Star_ai_colonise_value`, `Star_ai_defence_value`) only feed the
// PATCH fight-or-flee test, replaced by a constant "fight": not ported.
std::int32_t antag_race_ai_tactical(Session& session, Race& race, Star& star, const BattleLists& lists,
                                    BattleAction& action) {
  Cosmos& cosmos = session.game->cosmos;
  std::uint8_t hostile_mask = 0;
  for (const std::uint8_t entry : lists.races) {
    if (entry & kBattleRaceHostile) {
      hostile_mask = static_cast<std::uint8_t>(hostile_mask | (1u << (entry & 7)));
    }
  }
  const std::vector<ShipIndex> present = cosmos_ships_at_star(cosmos, star);
  bool hostile_in_space = false;
  for (const ShipIndex i : present) {
    const Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    if (ship.location_kind == ShipLocation::kSystem && hostile_owner(hostile_mask, ship)) {
      hostile_in_space = true;
      break;
    }
  }
  if (!hostile_in_space && race.index == session.player_race && !session.autoplay) {
    return 2;
  }
  if (lists.ships.empty()) {
    return hostile_in_space && planets_fire(session, lists, present, hostile_mask, action) ? -1 : 0;
  }
  if (!hostile_in_space) {
    return attack_planets(session, race, star, lists, action);
  }

  // Shields up on the own ships in space a hostile ship could hit, down on
  // the others.
  Ship* threatened = nullptr;
  Ship* threat = nullptr;
  for (const ShipIndex i : lists.ships) {
    Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    if (ship.location_kind != ShipLocation::kSystem) {
      continue;
    }
    for (const ShipIndex j : present) {
      Ship& enemy = cosmos.ships[static_cast<std::size_t>(j)];
      if (hostile_owner(hostile_mask, enemy) && can_hit(session, enemy, ship)) {
        ship_set_all_shields(session, ship, true);
        threatened = &ship;
        threat = &enemy;
      }
    }
    if (&ship != threatened) {
      ship_set_all_shields(session, ship, false);
    }
  }
  Ship* unarmed = nullptr;
  Ship* out_of_range = nullptr;
  if (ships_fire(session, lists, present, hostile_mask, action, unarmed, out_of_range, eligible)) {
    return -1;
  }
  if (use_support_gizmos(session, out_of_range, unarmed, action)) {
    return -1;
  }
  if (threatened != nullptr && antag_ship_ai_use_evasive_gizmo(session, *threatened, *threat)) {
    action = ship_action(cosmos, *threatened);
    return -1;
  }
  // The strongest fighting ship (hit points x damage x power) closes in;
  // without a positive value the threatened ship is kept.
  Ship* mover = threatened;
  std::int32_t best = 0;
  for (const ShipIndex i : lists.ships) {
    Ship& ship = cosmos.ships[static_cast<std::size_t>(i)];
    if (!eligible(ship) || ship.location_kind != ShipLocation::kSystem) {
      continue;
    }
    const std::int32_t v = wrap_i32(static_cast<std::int64_t>(ship.hit_points) * ship.weapon_damage * ship.power);
    if (v > best) {
      best = v;
      mover = &ship;
    }
  }
  if (mover == nullptr) {
    return 0;
  }
  if (!lists.planets.empty() && planets_fire(session, lists, present, hostile_mask, action)) {
    return -1;
  }
  float nearest = kFarAway;
  Ship* goal = nullptr;
  for (const ShipIndex j : present) {
    Ship& enemy = cosmos.ships[static_cast<std::size_t>(j)];
    if (!hostile_owner(hostile_mask, enemy)) {
      continue;
    }
    const float d = target_distance(enemy.position, mover->position, enemy);
    if (d < nearest && kApproachMinimum < d) {
      nearest = d;
      goal = &enemy;
    }
  }
  if (goal == nullptr) {
    return 0;
  }
  float reach = static_cast<float>(ship_ai_engage_distance(session, *mover, 1));
  if (0.0f > reach) {
    reach = f32(static_cast<double>(ship_ai_engage_distance(session, *goal, 1)) * kEnemyReachMargin);
  }
  action = ship_action(cosmos, *mover);
  stand_off(session, *mover, goal->position, 200, 100, reach);
  return -1;
}

std::int32_t ship_ai_engage_distance(const Session& session, const Ship& ship, std::int32_t mode) {
  float best = -1.0f;
  for (std::int32_t s = 0; s < ship.slot_count; ++s) {
    const GizmoSlot& slot = ship.slots[static_cast<std::size_t>(s)];
    const std::int8_t g = slot.gizmo;
    if (g < 0 || static_cast<std::size_t>(g) >= kGizmoCount) {
      continue;
    }
    const Gizmo& gizmo = session.tables->gizmos[static_cast<std::size_t>(g)];
    if (gizmo.uses != 0 && slot.uses_left == 0) {
      continue;
    }
    // The weapons but the Nanomanipulator (9: original off-by-one), and by
    // mode the gizmos aimed at ships or at planets.
    double share = 0.0;
    if (g <= 8) {
      share = kReachShare;
    } else if (g == kGizmoCarbonizer) {
      share = kCarbonizerShare;
    } else if (mode == 1) {
      switch (g) {
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x25:
        case 0x26:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x36:
        case 0x3d:
        case 0x40:
        case 0x42:
          share = kReachShare;
          break;
        default:
          break;
      }
    } else if (mode == 2 && g == kGizmoPhaseBomb) {
      share = kReachShare;
    }
    if (share == 0.0) {
      continue;
    }
    const float d = f32(static_cast<double>(gizmo.range << 5) * share);
    if (best < d) {
      best = d;
    }
  }
  return chop(best);
}

std::int32_t planet_ai_orbital_weapon_reach(const Session& session, const Planet& planet) {
  const Cosmos& cosmos = session.game->cosmos;
  float best = -1.0f;
  for (std::uint16_t sq = planet.surface_squares; sq < planet.square_count; ++sq) {
    const Square& s = cosmos.squares[static_cast<std::size_t>(planet.first_square + sq)];
    if (s.item == kNoItem || (s.flags & kSquareBuilt) == 0 || s.item < kItemMissileBase || s.item > kItemMegaWhopper ||
        (s.flags >> 8) == 0) {
      continue;
    }
    PlanetWeaponStats stats;
    planitem_weapon_stats(s.item, stats);
    const float d = f32(static_cast<double>(stats.range << 5) * kReachShare);
    if (best < d) {
      best = d;
    }
  }
  return chop(best);
}

bool antag_ship_ai_use_evasive_gizmo(Session& session, Ship& ship, Ship& threat) {
  return use_evasive_gizmo(session, ship, threat, kEvasiveGizmos,
                           session.game->variant == Variant::kPeCustom ? kPeCustomEvasiveTargets : kEvasiveTargets);
}

}  // namespace oa::game
