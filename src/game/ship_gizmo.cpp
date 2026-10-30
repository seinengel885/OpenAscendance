// The gizmo effects (`shipgiz.cpp`, `docs/08-combat.md` "Using a gizmo").
#include <cmath>
#include <utility>
#include <vector>

#include "game/cosmos.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/race.h"
#include "game/ship.h"
#include "game/tables.h"

namespace oa::game {

namespace {

constexpr std::uint8_t kCategoryWeapon = 0;
constexpr std::uint8_t kCategoryDrive = 2;
constexpr std::uint8_t kCategorySpecial = 5;

// Target flag bits (`GZ_*` of gizmos.txt).
constexpr std::uint16_t kFlagTargetShip = 1;
constexpr std::uint16_t kFlagTargetLane = 2;
constexpr std::uint16_t kFlagTargetPlanet = 0xc;  // orbit | surface

// Gizmo ids with an effect (`gizmos.txt` order).
enum GizmoId : std::uint8_t {
  kLaneBlocker = 32,
  kMolecularTieDown = 33,
  kIntellectScrambler = 34,
  kBrunswikDissipator = 35,
  kRecaller = 36,
  kDisarmer = 37,
  kSmartBomb = 38,
  kGravityDistorter = 39,
  kFleetDisperser = 40,
  kPositronBouncer = 45,
  kGravimetricCatapult = 46,
  kCarbonizer = 47,
  kContainmentDevice = 48,
  kShieldBlaster = 49,
  kBackfirer = 50,
  kLaneDestabilizer = 51,
  kTractorBeam = 52,
  kCannibalizer = 53,
  kMovingPartExploiter = 54,
  kHyperswapper = 55,
  kGravimetricCondensor = 56,
  kAccutron = 57,
  kRemoteRepair = 58,
  kSacrificialOrb = 59,
  kLaneMagnetron = 60,
  kDisintegrator = 61,
  kLaneEndoscope = 62,
  kToroidalBlaster = 63,
  kGizmogrifier = 64,
  kReplenisher = 65,
  kSpecialtyBlaster = 66,
  kPlasmaCoupler = 68,
  kInvulnerablizer = 69,
  kPhaseBomb = 70,
  kSelfDestructotron = 72,
  kMassCondensor = 74,
  kHyperfuel = 75,
};

const Gizmo& gizmo_of(const Session& session, std::int8_t id) {
  return session.tables->gizmos.at(static_cast<std::uint8_t>(id));
}

// The acting gizmo slot of the action block, its slot number.
std::int32_t action_slot(const Session& session, const Ship& ship) {
  return gizmo_slot_at(session, ship_index(session.game->cosmos, ship), ship.action_slot).value();
}

Ship& ship_at_raw(Session& session, RawAddress raw) {
  return session.game->cosmos.ships.at(static_cast<std::size_t>(ship_at(session, raw).value()));
}

Star& star_of(Cosmos& cosmos, StarIndex star) { return cosmos.stars.at(static_cast<std::size_t>(star)); }

// The star of `ship` for the cosmos module's star argument: a ship in space
// (any other location reads its location as the star in the original).
Star& ship_location_star(Cosmos& cosmos, const Ship& ship) { return star_of(cosmos, ship.location); }

// `a + b`, each component stored as a float.
Vec3 add(const Vec3& a, const Vec3& b) {
  return {f32(static_cast<double>(a[0]) + b[0]), f32(static_cast<double>(a[1]) + b[1]),
          f32(static_cast<double>(a[2]) + b[2])};
}

// The Myrmidonic Carbonizer's minimum range: PATCH tests `dist < dist *
// 0.75` (never true); the Antagonizer `dist < range * 0.75` (`range` with
// the Accutron bonus).
bool carbonizer_too_close(const Session& session, float distance, float range) {
  const double limit = antagonizer_rules(session.game->variant) ? static_cast<double>(range) * 0.75
                                                                : static_cast<double>(distance) * 0.75;
  return static_cast<double>(distance) < limit;
}

// Removes the gizmo of the acting slot (a gizmo consumed by its use).
void consume_action_gizmo(Session& session, Ship& ship) {
  ship_remove_gizmo(session, ship, action_slot(session, ship));
}

}  // namespace

// 0x46bb0. An Invulnerablizer absorbs nothing and takes no hull damage.
std::uint8_t ship_take_damage(Session& session, Ship& ship, bool apply, std::int32_t damage, RaceIndex attacker) {
  Cosmos& cosmos = session.game->cosmos;
  std::uint8_t result = 0;
  std::int32_t hull = 0;
  std::int32_t absorbed = (ship.battle_flags & kShipInvulnerable) ? 0 : damage;
  if (ship.shield < absorbed) {
    result = kActionPenetrated;
    hull = damage - ship.shield;
    absorbed = ship.shield;
  }
  if (ship.hit_points <= hull) {
    result |= kActionDestroyed;
    hull = ship.hit_points;
  }
  if (apply) {
    StarIndex star = kNone;
    if (ship.location_kind == ShipLocation::kSystem) {
      star = ship.location;
    } else if (ship.location_kind == ShipLocation::kOrbit || ship.location_kind == ShipLocation::kBuilding ||
               ship.location_kind == ShipLocation::kRefit) {
      star = cosmos.planets.at(static_cast<std::size_t>(ship.location)).star;
    }
    race_offended_by(session, cosmos.races.at(static_cast<std::size_t>(ship.owner)), attacker, star_of(cosmos, star));
    ship.shield -= absorbed;
    ship.hit_points -= hull;
    if (ship.hit_points < 1) {
      const std::uint8_t hull_size = ship.hull_size;
      const std::int16_t owner = ship.owner;
      ship_destroy(session, ship);
      if (owner == session.player_race && session.screen != 16) {
        session.post_event(EventCode::kShipLost, hull_size, star);
      }
    }
  }
  return result;
}

// 0x46d54: the target ship must be in space at the user's star; the damage
// is applied (or checked) before the special effects.
std::uint32_t ship_use_gizmo(Session& session, Ship& ship, bool apply) {
  Cosmos& cosmos = session.game->cosmos;
  const std::int8_t id = ship.slots.at(static_cast<std::size_t>(action_slot(session, ship))).gizmo;
  const Gizmo& g = gizmo_of(session, id);
  bool power_ok = true;
  std::int32_t cost = g.power;
  if (ship.power < g.power) {
    power_ok = false;
    cost = 0;
  }
  Ship& target = ship_at_raw(session, ship.target);
  bool in_range = true;
  if (ship.location_kind == ShipLocation::kSystem && target.location_kind == ShipLocation::kSystem &&
      ship.location == target.location) {
    if (g.range != 0) {
      const float distance = f32(length(sub(target.position, ship.position)));
      float range = static_cast<float>(g.range * 32);
      if (ship.battle_flags & kShipAccutron) {
        range = f32(static_cast<double>(range) * 2.0f);
      }
      if (range < distance) {
        in_range = false;
      }
      if (id == kCarbonizer && carbonizer_too_close(session, distance, range)) {
        in_range = false;
      }
    }
  } else {
    in_range = false;
  }
  if (!power_ok || !in_range || &target == &ship || (g.flags & kFlagTargetShip) == 0) {
    return 0;
  }
  std::uint32_t result = kActionPossible;
  ship.action_point = target.position;
  std::int32_t damage = g.category == kCategoryWeapon ? g.weapon_damage : 0;
  if (id == kCarbonizer) {
    damage = g.special1;
  }
  if (damage == 0) {
    result |= kActionNoDamage;
  } else {
    result |= ship_take_damage(session, target, apply, damage, static_cast<RaceIndex>(ship.owner));
  }
  if (!apply) {
    return result;
  }
  ship.power -= cost;
  const std::int32_t remaining = ship.power;
  switch (static_cast<std::uint8_t>(id)) {
    case kMolecularTieDown:
      target.moves = 0;
      target.battle_flags |= kShipImmobile;
      break;
    case kIntellectScrambler:
      target.built_day = cosmos.day;
      break;
    case kBrunswikDissipator:
      target.power = 0;
      break;
    case kDisarmer:
    case kSpecialtyBlaster: {
      const std::uint8_t category = id == kSpecialtyBlaster ? kCategorySpecial : kCategoryWeapon;
      std::vector<std::int32_t> slots;
      for (std::int32_t i = 0; i < target.slot_count; ++i) {
        const std::int8_t t = target.slots[static_cast<std::size_t>(i)].gizmo;
        if (t != -1 && gizmo_of(session, t).category == category) {
          slots.push_back(i);
        }
      }
      if (!slots.empty()) {
        ship_remove_gizmo(session, target,
                          slots[static_cast<std::size_t>(cosmos.day % static_cast<std::int32_t>(slots.size()))]);
      }
      break;
    }
    case kFleetDisperser: {
      // Every other ship of the star (docked ones included) closer than
      // `special1 * 32` to the target is pushed to exactly that distance.
      const std::int32_t reach = g.special1 * 32;
      for (const ShipIndex i : cosmos_ships_at_star(cosmos, ship_location_star(cosmos, ship))) {
        Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
        if (&other == &target) {
          continue;
        }
        Vec3 d = sub(other.position, target.position);
        if (length(d) < static_cast<double>(static_cast<float>(reach))) {
          set_length(d, static_cast<float>(reach));
          other.position = add(target.position, d);
        }
      }
      break;
    }
    case kPositronBouncer: {
      Vec3 d = sub(target.position, ship.position);
      set_length(d, static_cast<float>(g.special1 * 32));
      target.position = add(target.position, d);
      break;
    }
    case kContainmentDevice:
      for (std::int32_t i = 0; i < target.slot_count; ++i) {
        const std::int8_t t = target.slots[static_cast<std::size_t>(i)].gizmo;
        if (t == kGizmoColonizer || t == kGizmoInvasionModule) {
          ship_remove_gizmo(session, target, i);
        }
      }
      break;
    case kShieldBlaster:
      target.shield = 0;
      target.battle_flags |= kShipShieldsDown;
      break;
    case kBackfirer:
    case kMovingPartExploiter: {
      const std::uint8_t category = id == kBackfirer ? kCategoryWeapon : kCategorySpecial;
      std::int32_t count = 0;
      for (std::int32_t i = 0; i < target.slot_count; ++i) {
        const std::int8_t t = target.slots[static_cast<std::size_t>(i)].gizmo;
        if (t != -1 && gizmo_of(session, t).category == category) {
          ++count;
        }
      }
      target.hit_points -= count;
      if (target.hit_points < 1) {
        ship_destroy(session, target);
        result |= kActionDestroyed;
      }
      break;
    }
    case kTractorBeam: {
      // The pulled distance is compared unrounded with the 64 minimum.
      Vec3 d = sub(target.position, ship.position);
      const double pulled = length(d) - static_cast<double>(g.special1 * 32);
      const float to = pulled < 64.0 ? 64.0f : f32(pulled);
      set_length(d, to);
      target.position = add(ship.position, d);
      break;
    }
    case kHyperswapper:
      std::swap(ship.position, target.position);
      break;
    case kSacrificialOrb: {
      std::int32_t n = target.max_hit_points - target.hit_points;
      if (g.special1 < n) {
        n = g.special1;
      }
      target.hit_points += n;
      ship.hit_points -= n;
      if (ship.hit_points < 1) {
        ship_destroy(session, ship);
      }
      break;
    }
    case kDisintegrator:
      ship_destroy(session, target);
      consume_action_gizmo(session, ship);
      result |= kActionDestroyed;
      break;
    case kGizmogrifier: {
      if (target.gizmo_count == 0) {
        break;
      }
      const std::int32_t pick = cosmos.day % target.gizmo_count;
      std::int32_t filled = 0;
      for (std::int32_t i = 0; i < target.slot_count; ++i) {
        if (target.slots[static_cast<std::size_t>(i)].gizmo != -1) {
          if (filled == pick) {
            ship_remove_gizmo(session, target, i);
            break;
          }
          ++filled;
        }
      }
      break;
    }
    case kPlasmaCoupler: {
      std::int32_t n = target.generator_power - target.power;
      if (remaining < n) {
        n = remaining;
      }
      target.power += n;
      ship.power -= n;
      break;
    }
    case kMassCondensor: {
      // Ships farther than `special2 * 32` from the target are pulled by
      // `special1 * 32`, not closer than `special2 * 32`.
      const float threshold = static_cast<float>(g.special2 * 32);
      for (const ShipIndex i : cosmos_ships_at_star(cosmos, ship_location_star(cosmos, ship))) {
        Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
        if (&other == &target) {
          continue;
        }
        Vec3 d = sub(other.position, target.position);
        const float distance = f32(length(d));
        if (threshold < distance) {
          const double pulled = static_cast<double>(distance) - static_cast<double>(g.special1 * 32);
          const float to = pulled < static_cast<double>(threshold) ? threshold : f32(pulled);
          set_length(d, to);
          other.position = add(target.position, d);
        }
      }
      break;
    }
    default:
      break;
  }
  return result;
}

// 0x47c0c (named `Ship_can_use_gizmo_on` in data.csv: it also applies).
// No location test: the user need not be in space.
std::uint32_t ship_use_gizmo_on_planet(Session& session, Ship& ship, bool apply) {
  Cosmos& cosmos = session.game->cosmos;
  const std::int8_t id = ship.slots.at(static_cast<std::size_t>(action_slot(session, ship))).gizmo;
  const Gizmo& g = gizmo_of(session, id);
  bool power_ok = true;
  std::int32_t cost = g.power;
  if (ship.power < g.power) {
    power_ok = false;
    cost = 0;
  }
  Planet& planet = cosmos.planets.at(static_cast<std::size_t>(planet_at(session, ship.target).value()));
  ship.action_point = planet.position;
  bool in_range = true;
  if (g.range != 0) {
    const float distance = f32(length(sub(planet.position, ship.position)));
    float range = static_cast<float>(g.range * 32);
    if (ship.battle_flags & kShipAccutron) {
      range = f32(static_cast<double>(range) * 2.0f);
    }
    if (range < distance) {
      in_range = false;
    }
    if (id == kCarbonizer && carbonizer_too_close(session, distance, range)) {
      in_range = false;
    }
  }
  if (!power_ok || !in_range || (g.flags & kFlagTargetPlanet) == 0) {
    return 0;
  }
  if (!apply) {
    return kActionPossible;
  }
  ship.power -= cost;
  if (g.category == kCategoryWeapon) {
    planet_bombard(session, planet, g.weapon_damage, static_cast<RaceIndex>(ship.owner));
    return kActionPossible;
  }
  if (id == kPhaseBomb) {
    // Up to 5 built surface buildings without housing, in square order.
    // (The original's "square used" test reads the colour byte, never 0xff.)
    std::int32_t demolished = 0;
    for (std::uint16_t i = 0; i < planet.surface_squares && demolished < 5; ++i) {
      const Square& sq = planet_squares(cosmos, planet)[i];
      if ((sq.flags & kSquareBuilt) != 0 && session.tables->planitems.items.at(sq.item).max_population == 0 &&
          planet_set_square_item(session, planet, i, kNoItem, SquareMode::kRemove)) {
        ++demolished;
      }
    }
    consume_action_gizmo(session, ship);
  }
  return kActionPossible;
}

// 0x47ec8: the lane end on the user's side; no Accutron bonus.
std::uint32_t ship_use_gizmo_on_lane(Session& session, Ship& ship, bool apply) {
  Cosmos& cosmos = session.game->cosmos;
  const std::int8_t id = ship.slots.at(static_cast<std::size_t>(action_slot(session, ship))).gizmo;
  const Gizmo& g = gizmo_of(session, id);
  bool power_ok = true;
  std::int32_t cost = g.power;
  if (ship.power < g.power) {
    power_ok = false;
    cost = 0;
  }
  Lane& lane = cosmos.lanes.at(static_cast<std::size_t>(lane_at(session, ship.target).value()));
  const Vec3 end = lane.star_a == ship.location ? lane.end_a : lane.end_b;
  ship.action_point = end;
  const float distance = f32(length(sub(end, ship.position)));
  const bool in_range = g.range == 0 || !(static_cast<float>(g.range * 32) < distance);
  if (!power_ok || !in_range || (g.flags & kFlagTargetLane) == 0) {
    return 0;
  }
  if (!apply) {
    return kActionPossible;
  }
  ship.power -= cost;
  if (g.category == kCategoryWeapon && g.weapon_damage > 1) {
    lane.flags &= ~2u;
    return kActionPossible;
  }
  const StarIndex far = lane.star_a == ship.location ? lane.star_b : lane.star_a;
  switch (static_cast<std::uint8_t>(id)) {
    case kLaneBlocker:
      lane.flags |= 2u;
      return kActionPossible;
    case kLaneDestabilizer: {
      // Every ship on the lane reaches its end (used-slot scan).
      const float lane_length =
          f32(length(sub(star_of(cosmos, lane.star_a).position, star_of(cosmos, lane.star_b).position)));
      const LaneIndex self = lane_index(cosmos, lane);
      std::int32_t used = 0;
      for (std::size_t i = 0; i < kMaxShips && used < cosmos.ship_count; ++i) {
        Ship& other = cosmos.ships[i];
        if (other.owner != -1) {
          if (other.location_kind == ShipLocation::kLane && other.location == self) {
            other.position[0] = lane_length;
          }
          ++used;
        }
      }
      break;
    }
    case kLaneMagnetron:
      ship_leave_star_system(session, ship);
      ship.location_kind = ShipLocation::kLane;
      ship.location = lane_index(cosmos, lane);
      ship_enter_star_system(session, star_of(cosmos, far), ship);
      break;
    case kLaneEndoscope:
      star_of(cosmos, far).explored_mask |= static_cast<std::uint8_t>(1u << (ship.owner & 0x1f));
      return kActionPossible;
    default:
      return kActionPossible;
  }
  consume_action_gizmo(session, ship);
  return kActionPossible;
}

// 0x48208: only the power is checked.
std::uint32_t ship_use_gizmo_untargeted(Session& session, Ship& ship, bool apply) {
  Cosmos& cosmos = session.game->cosmos;
  const std::int8_t id = ship.slots.at(static_cast<std::size_t>(action_slot(session, ship))).gizmo;
  const Gizmo& g = gizmo_of(session, id);
  if (ship.power < g.power) {
    return 0;
  }
  if (!apply) {
    return kActionPossible;
  }
  ship.power -= g.power;
  const RaceIndex owner = static_cast<RaceIndex>(ship.owner);
  switch (static_cast<std::uint8_t>(id)) {
    case kRecaller: {
      ship_leave_star_system(session, ship);
      const StarIndex home = cosmos.races.at(owner).home;
      ship.location = home;
      ship_enter_star_system(session, star_of(cosmos, home), ship);
      break;
    }
    case kSmartBomb: {
      const Race& race = cosmos.races.at(owner);
      for (const ShipIndex i : cosmos_ships_at_star(cosmos, ship_location_star(cosmos, ship))) {
        Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
        if (other.owner != ship.owner && race.relation.at(static_cast<std::size_t>(other.owner)) == kWar) {
          ship_take_damage(session, other, true, g.special1, owner);
        }
      }
      consume_action_gizmo(session, ship);
      break;
    }
    case kGravityDistorter: {
      const float reach = static_cast<float>(g.special1 * 32);
      for (const ShipIndex i : cosmos_ships_at_star(cosmos, ship_location_star(cosmos, ship))) {
        Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
        if (&other == &ship) {
          continue;
        }
        Vec3 d = sub(other.position, ship.position);
        if (length(d) < static_cast<double>(reach)) {
          set_length(d, reach);
          other.position = add(ship.position, d);
        }
      }
      break;
    }
    case kGravimetricCatapult:
      for (float& c : ship.position) {
        c = -c;
      }
      break;
    case kCannibalizer:
      ship.hit_points -= g.special1;
      ship.power += g.special2;
      if (ship.hit_points < 1) {
        ship_destroy(session, ship);
      }
      break;
    case kGravimetricCondensor: {
      // Ships in space (the user included) farther than `special2 * 32`
      // from the star are pulled by `special1 * 32`, not closer than that.
      const float threshold = static_cast<float>(g.special2 * 32);
      for (const ShipIndex i : cosmos_ships_at_star(cosmos, ship_location_star(cosmos, ship))) {
        Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
        if (other.location_kind != ShipLocation::kSystem) {
          continue;
        }
        const float distance = f32(length(other.position));
        if (threshold < distance) {
          const double pulled = static_cast<double>(distance) - static_cast<double>(g.special1 * 32);
          set_length(other.position, pulled < static_cast<double>(threshold) ? threshold : f32(pulled));
        }
      }
      break;
    }
    case kAccutron:
      ship.battle_flags |= kShipAccutron;
      break;
    case kRemoteRepair:
      ship.hit_points += g.special1;
      if (ship.max_hit_points < ship.hit_points) {
        ship.hit_points = ship.max_hit_points;
      }
      break;
    case kToroidalBlaster: {
      const std::int32_t drive = ship_find_gizmo_by_category(session, ship, kCategoryDrive);
      if (drive != -1) {
        ship_remove_gizmo(session, ship, drive);
        ship.battle_flags |= kShipToroidal;
      }
      break;
    }
    case kReplenisher:
      for (std::int32_t i = 0; i < ship.slot_count; ++i) {
        GizmoSlot& slot = ship.slots[static_cast<std::size_t>(i)];
        if (slot.gizmo != -1 && slot.gizmo != kReplenisher && gizmo_of(session, slot.gizmo).uses != 0) {
          slot.uses_left = static_cast<std::int16_t>(gizmo_of(session, slot.gizmo).uses);
        }
      }
      break;
    case kInvulnerablizer:
      ship.battle_flags |= kShipInvulnerable;
      consume_action_gizmo(session, ship);
      break;
    case kSelfDestructotron: {
      const float reach = static_cast<float>(g.range * 32);
      for (const ShipIndex i : cosmos_ships_at_star(cosmos, ship_location_star(cosmos, ship))) {
        Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
        if (&other != &ship && length(sub(other.position, ship.position)) <= static_cast<double>(reach)) {
          ship_take_damage(session, other, true, g.special1, owner);
        }
      }
      ship_destroy(session, ship);
      break;
    }
    case kHyperfuel:
      ship.power += g.special1;
      consume_action_gizmo(session, ship);
      break;
    default:
      break;
  }
  return kActionPossible;
}

}  // namespace oa::game
