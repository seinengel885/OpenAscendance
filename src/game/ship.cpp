#include "game/ship.h"

#include <cmath>
#include <cstring>
#include <string>

#include "core/fixed_width.h"
#include "game/cosmos.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/raceneg.h"
#include "game/tables.h"

namespace oa::game {

namespace {

// Slots by hull size (`i16` 0x8682c) and the empty hulls' industry (0x86864).
constexpr std::array<std::int32_t, kHullCount> kHullSlots = {5, 10, 15, 25};
constexpr std::array<std::int32_t, kHullCount> kHullCost = {70, 170, 240, 410};

// Species abilities read here (`Race::ability`).
constexpr std::uint8_t kAbilitySnovemdomas = 1;
constexpr std::uint8_t kAbilityChronomyst = 18;

// Gizmo categories.
constexpr std::uint8_t kCategoryWeapon = 0;
constexpr std::uint8_t kCategoryShield = 1;
constexpr std::uint8_t kCategoryDrive = 2;
constexpr std::uint8_t kCategoryScanner = 3;

// Gizmo flag bits (`GZ_*`).
constexpr std::uint16_t kGizmoToggleable = 0x20;

constexpr std::int8_t kGizmoCloaker = 42;
constexpr std::int8_t kGizmoXRayMegaglasses = 41;
constexpr std::int8_t kGizmoPhaseBomb = 70;

constexpr std::uint8_t kItemObservation = 16;

const Gizmo& gizmo_of(const Session& session, std::int8_t id) {
  return session.tables->gizmos.at(static_cast<std::uint8_t>(id));
}

// `Ship_has_ready_weapon` reads the record of an empty slot's id -1, i.e.
// the bytes before the gizmo table: the hull layout masks Ship_init builds
// at 0xf64b4 (computed once from the tables 0x86844 / 0x86854): category
// byte 0 (a weapon) and the `uses` dword 0x05010000.
constexpr std::uint8_t kEmptySlotCategory = 0;
constexpr std::int32_t kEmptySlotUses = 0x05010000;

bool race_knows(const Research& research, std::uint8_t item, std::int32_t race) {
  return ((research.items.at(item).known_mask >> (race & 0x1f)) & 1) != 0;
}

// strcpy of a hull name into the ship's name (the bytes after the NUL keep
// their old values).
void copy_hull_name(const Session& session, Ship& ship) {
  const std::string& name = session.tables->hull_names.at(ship.hull_size);
  const std::size_t n = std::min(name.size(), ship.name.size() - 1);
  std::memcpy(ship.name.data(), name.data(), n);
  ship.name[n] = '\0';
}

Star& ship_star(Cosmos& cosmos, const Ship& ship) {
  switch (ship.location_kind) {
    case ShipLocation::kBuilding:
    case ShipLocation::kRefit:
    case ShipLocation::kOrbit:
      return cosmos.stars.at(static_cast<std::size_t>(cosmos.planets.at(static_cast<std::size_t>(ship.location)).star));
    default:
      return cosmos.stars.at(static_cast<std::size_t>(ship.location));
  }
}

}  // namespace

// `Ship_reset_with_hull` 0x444bc: `Ship_init` 0x4470c, then the hull. The
// first call's one-time set-up (hull names, gizmo table, layout masks) is the
// game tables'.
void ship_init(Session& session, Ship& ship, std::uint8_t hull) {
  ship.gizmo_count = 0;
  ship.weapon_damage = 0;
  ship.shield_max = 0;
  ship.lane_speed = 0;
  ship.drive_speed = 0;
  ship.scanner_level = 0;
  ship.cloakers = 0;
  ship.generator_power = 0;
  ship.drive_power = 0;
  ship.reserved_2c = {0, 0};
  ship.has_colonizer = 0;
  ship.name[0] = '\0';
  ship.owner = -1;
  ship.shield = 0;
  ship.hit_points = 1;
  ship.destination_kind = ShipOrder::kNothing;
  ship.destination = kNone;
  ship.stale_destination = 0;
  ship.power = ship.shield;
  ship.max_hit_points = ship.hit_points;
  for (GizmoSlot& slot : ship.slots) {
    slot.gizmo = -1;
    slot.uses_left = 0;
    slot.toggled = 0;
  }
  if (antagonizer_rules(session.game->variant)) {
    // ANTAG 0x48eac also clears the location kind.
    ship.location_kind = ShipLocation::kNone;
  }
  ship_set_hull_size(session, ship, hull);
}

// 0x449a4. The hit points read the owner's ability: for a slot just reset
// (owner -1) the original reads the byte before the races (0, no doubling).
void ship_set_hull_size(Session& session, Ship& ship, std::uint8_t hull) {
  ship.hull_size = hull;
  ship.slot_count = kHullSlots.at(hull);
  if (ship.name[0] == '\0' || ship.owner != session.player_race) {
    copy_hull_name(session, ship);
  }
  ship.hit_points = (hull + 1) * 10;
  ship.drive_base = 0;
  if (ship.owner >= 0 &&
      session.game->cosmos.races.at(static_cast<std::size_t>(ship.owner)).ability == kAbilitySnovemdomas) {
    ship.hit_points *= 2;
  }
  ship.max_hit_points = ship.hit_points;
}

// 0x44b08. Slot `slot_count` passes the bound test (original off-by-one);
// past the 25th slot the original would write into `slot_count` itself, not
// modelled (no caller goes that far).
bool ship_install_gizmo(Session& session, Ship& ship, std::int8_t gizmo, std::int32_t slot) {
  if (slot > ship.slot_count || slot < 0 || static_cast<std::size_t>(slot) >= kShipSlots) {
    return false;
  }
  GizmoSlot& s = ship.slots[static_cast<std::size_t>(slot)];
  if (s.gizmo == gizmo) {
    return false;
  }
  if (s.gizmo == -1) {
    ++ship.gizmo_count;
  }
  s.gizmo = gizmo;
  ship_recompute_stats(session, ship);
  return true;
}

// 0x44b54.
bool ship_remove_gizmo(Session& session, Ship& ship, std::int32_t slot) {
  GizmoSlot& s = ship.slots.at(static_cast<std::size_t>(slot));
  if (s.gizmo == -1) {
    return false;
  }
  --ship.gizmo_count;
  s.gizmo = -1;
  ship_recompute_stats(session, ship);
  return true;
}

// 0x44b84.
std::int32_t ship_find_gizmo_by_category(const Session& session, const Ship& ship, std::uint8_t category) {
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const std::int8_t g = ship.slots[static_cast<std::size_t>(i)].gizmo;
    if (g != -1 && gizmo_of(session, g).category == category) {
      return i;
    }
  }
  return -1;
}

// 0x44bd8 (an id of -1 finds the first empty slot).
std::int32_t ship_find_gizmo(const Ship& ship, std::int8_t gizmo) {
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    if (ship.slots[static_cast<std::size_t>(i)].gizmo == gizmo) {
      return i;
    }
  }
  return -1;
}

// 0x44c18.
bool ship_change_hull_size(Session& session, Ship& ship, std::uint8_t hull) {
  if (hull == ship.hull_size) {
    return true;
  }
  if (kHullSlots.at(hull) < ship.gizmo_count) {
    return false;
  }
  std::int32_t next = 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const std::int8_t g = ship.slots[static_cast<std::size_t>(i)].gizmo;
    if (g != -1) {
      ship_remove_gizmo(session, ship, i);
      ship_install_gizmo(session, ship, g, next);
      ++next;
    }
  }
  ship_set_hull_size(session, ship, hull);
  copy_hull_name(session, ship);
  ship_recompute_stats(session, ship);
  return true;
}

// 0x44cf0.
void ship_enter_orbit(Session& session, Ship& ship, Planet& planet) {
  if (!planet_dock_ship(session, planet, ship)) {
    ship.location_kind = ShipLocation::kSystem;
    Vec3 offset{};
    for (float& c : offset) {
      c = static_cast<float>(session.rng.rand() % 1000 - 500);
    }
    set_length(offset, 250.0f);
    const Vec3 position = {f32(static_cast<double>(planet.position[0]) + offset[0]),
                           f32(static_cast<double>(planet.position[1]) + offset[1]),
                           f32(static_cast<double>(planet.position[2]) + offset[2])};
    ship_set_position(session, ship, position);
    return;
  }
  const std::int32_t gyro = ship_find_gizmo(ship, kGizmoGyroInductor);
  if (gyro != -1) {
    GizmoSlot& slot = ship.slots[static_cast<std::size_t>(gyro)];
    if (slot.uses_left != 0) {
      ship.power += gizmo_of(session, kGizmoGyroInductor).special1;
      --slot.uses_left;
    }
  }
  if (ship.owner == session.player_race && ship.destination_kind == ShipOrder::kOrbitPlanet) {
    if (session.screen != 16) {
      session.post_event(EventCode::kEnteredOrbit, ship_index(session.game->cosmos, ship), ship.destination);
    }
    ship.destination_kind = ShipOrder::kNothing;
    ship.destination = kNone;
    ship.stale_destination = 0;
  }
}

// 0x44e68.
bool ship_can_enter_lane(const Session& session, const Ship& ship, const Lane& lane) {
  const bool drive = ship.lane_speed + ship.hyper_speed > 0 ||
                     session.game->cosmos.races.at(static_cast<std::size_t>(ship.owner)).ability == kAbilityChronomyst;
  return drive && (lane.flags & 2) == 0;
}

// 0x44ea4.
void ship_enter_lane(Session& session, Ship& ship, Lane& lane) {
  Cosmos& cosmos = session.game->cosmos;
  if (!ship_can_enter_lane(session, ship, lane)) {
    return;
  }
  const StarIndex from = star_index(cosmos, ship_star(cosmos, ship));
  ship_leave_star_system(session, ship);
  ship.location_kind = ShipLocation::kLane;
  ship.position[0] = 0.0f;
  ship.location = lane_index(cosmos, lane);
  ship.position[1] = from == lane.star_a ? 1.0f : 0.0f;
}

// 0x44f18.
void ship_set_position(Session& session, Ship& ship, const Vec3& position) {
  ship.position = position;
  snap_to_grid(ship.position, session.grid);
}

// 0x44f3c.
void ship_recompute_stats(Session& session, Ship& ship) {
  ship.weapon_damage = 0;
  ship.shield_max = 0;
  ship.scanner_level = 0;
  ship.cloakers = 0;
  ship.generator_power = 0;
  ship.drive_power = 0;
  ship.has_colonizer = 0;
  ship.reserved_2c = {0, 0};
  ship.drive_speed = ship.drive_base;
  std::int32_t lane_drives = 0;
  std::int32_t hyper_drives = 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const std::int8_t id = ship.slots[static_cast<std::size_t>(i)].gizmo;
    if (id == -1) {
      continue;
    }
    const Gizmo& g = gizmo_of(session, id);
    ship.weapon_damage += g.weapon_damage;
    ship.shield_max += g.shield_strength;
    ship.drive_speed += g.drive_speed;
    ship.scanner_level += g.scanner_level;
    ship.generator_power += g.generator_power;
    if (g.category == kCategoryDrive) {
      ship.drive_power += g.power;
    }
    if (id == kGizmoColonizer) {
      ship.has_colonizer = -1;
    } else if (id == kGizmoStarLaneDrive) {
      ++lane_drives;
    } else if (id == kGizmoStarLaneHyperdrive) {
      ++hyper_drives;
    } else if (id == kGizmoCloaker) {
      ++ship.cloakers;
    }
  }
  ship.lane_speed = chop(std::sqrt(static_cast<double>(lane_drives)) * 10.0);
  ship.hyper_speed = chop(std::sqrt(static_cast<double>(hyper_drives)) * 20.0);
}

// 0x45084.
void ship_launch(Session& session, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  const PlanetIndex planet = ship.location;
  const bool built = ship.location_kind == ShipLocation::kBuilding;
  if (built) {
    ship.built_day = cosmos.day;
  }
  if (ship.owner == session.player_race && session.screen != 17) {
    session.post_event(built ? EventCode::kShipBuilt : EventCode::kShipRefitted, ship_index(cosmos, ship), planet);
  }
  ship.location_kind = ShipLocation::kOrbit;
  ship_enter_star_system(
      session, cosmos.stars.at(static_cast<std::size_t>(cosmos.planets.at(static_cast<std::size_t>(planet)).star)),
      ship);
  ship_set_hull_size(session, ship, ship.hull_size);
  ship_new_day(session, ship);
}

// 0x4519c.
void ship_destroy(Session& session, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  switch (ship.location_kind) {
    case ShipLocation::kBuilding:
    case ShipLocation::kRefit: {
      Planet& planet = cosmos.planets.at(static_cast<std::size_t>(ship.location));
      ship.location_kind = ShipLocation::kSystem;
      ship.location = planet.star;
      ship_leave_star_system(session, ship);
      planet.project_item = kNoItem;
      if (planet.project_square != kNoSquare) {
        planet_squares(cosmos, planet)[planet.project_square].item = kNoItem;
        planet.project_square = kNoSquare;
      }
      break;
    }
    case ShipLocation::kOrbit:
      planet_undock_ship(session, cosmos.planets.at(static_cast<std::size_t>(ship.location)), ship);
      ship_leave_star_system(session, ship);
      break;
    case ShipLocation::kSystem:
      ship_leave_star_system(session, ship);
      break;
    default:
      break;
  }
  cosmos_free_ship(session, ship);
}

// 0x4529c.
void ship_set_all_shields(Session& session, Ship& ship, bool on) {
  const std::int32_t state = on ? -1 : 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const GizmoSlot& slot = ship.slots[static_cast<std::size_t>(i)];
    if (slot.gizmo != -1 && gizmo_of(session, slot.gizmo).category == kCategoryShield && slot.toggled != state) {
      ship_toggle_gizmo(session, ship, i);
    }
  }
}

// 0x452e8.
void ship_toggle_gizmo(Session& session, Ship& ship, std::int32_t slot) {
  GizmoSlot& s = ship.slots.at(static_cast<std::size_t>(slot));
  const Gizmo& g = gizmo_of(session, s.gizmo);
  if ((g.flags & kGizmoToggleable) == 0) {
    return;
  }
  if (s.toggled == -1) {
    // Off: the power is refunded even when the shield was used up.
    ship.power += g.power;
    if (g.category == kCategoryShield) {
      ship.shield -= g.shield_strength;
      if (ship.shield < 0) {
        ship.shield = 0;
      }
    }
    s.toggled = 0;
    return;
  }
  if (g.power <= ship.power) {
    ship.power -= g.power;
    if (g.category == kCategoryShield) {
      ship.shield += g.shield_strength;
    }
    s.toggled = -1;
  }
}

// 0x45398.
std::uint32_t ship_do_action(Session& session, Ship& ship, bool apply) {
  std::uint32_t result = 0;
  if (ship.action == 0) {
    result = ship_battle_move(session, ship, apply);
  } else if (ship.action == 1) {
    result = ship_battle_use_gizmo(session, ship, apply);
  }
  ship.action_result = static_cast<std::int32_t>(result);
  return result;
}

// 0x453c4. Positions are relative to the star; one move point is 160 units
// (`0.00625`, then `* 32 * 5`).
std::uint32_t ship_battle_move(Session& session, Ship& ship, bool apply) {
  Cosmos& cosmos = session.game->cosmos;
  Vec3 target{};
  Planet* planet = nullptr;
  Lane* lane = nullptr;
  switch (ship.target_kind) {
    case kTargetPoint:
      target = ship.target_point;
      break;
    case kTargetPlanet:
      planet = &cosmos.planets.at(static_cast<std::size_t>(planet_at(session, ship.target).value()));
      target = planet->position;
      break;
    case kTargetLane:
      lane = &cosmos.lanes.at(static_cast<std::size_t>(lane_at(session, ship.target).value()));
      target = ship.location == lane->star_a ? lane->end_a : lane->end_b;
      break;
    default:
      return 0;
  }
  std::uint32_t result = kActionPossible;
  Vec3 dir = sub(target, ship.position);
  const float distance = f32(length(dir) * static_cast<double>(0.00625f));
  float step = distance;
  if (distance > static_cast<float>(ship.moves)) {
    step = static_cast<float>(ship.moves);
  }
  std::int32_t cost = chop(static_cast<double>(ship.drive_power) * (1.0 + static_cast<double>(step)) /
                           static_cast<double>(ship.drive_speed));
  if (cost < 1) {
    cost = 1;
  }
  if (cost > ship.power) {
    step = f32(static_cast<double>(ship.power) * step / static_cast<double>(cost));
    cost = ship.power;
  }
  if (step == distance) {
    result |= kActionReached;
  }
  normalize(dir);
  Vec3 d = {f32(static_cast<double>(dir[0]) * step), f32(static_cast<double>(dir[1]) * step),
            f32(static_cast<double>(dir[2]) * step)};
  for (float& c : d) {
    c = f32(static_cast<double>(c) * 32.0f);
  }
  for (float& c : d) {
    c = f32(static_cast<double>(c) * 5.0f);
  }
  ship.action_point = {f32(static_cast<double>(ship.position[0]) + d[0]),
                       f32(static_cast<double>(ship.position[1]) + d[1]),
                       f32(static_cast<double>(ship.position[2]) + d[2])};
  snap_to_grid(ship.action_point, session.grid);
  if (apply) {
    ship.position = ship.action_point;
    ship.power -= cost;
    ship.moves = chop(static_cast<double>(ship.moves) - step);
    if (result & kActionReached) {
      if (planet != nullptr) {
        ship_enter_orbit(session, ship, *planet);
      } else if (lane != nullptr) {
        ship_enter_lane(session, ship, *lane);
      }
    }
  }
  return result;
}

// 0x457d0. A limited gizmo uses one of its day's uses on apply, before its
// effect is checked.
std::uint32_t ship_battle_use_gizmo(Session& session, Ship& ship, bool apply) {
  const ShipIndex self = ship_index(session.game->cosmos, ship);
  GizmoSlot& slot = ship.slots.at(static_cast<std::size_t>(gizmo_slot_at(session, self, ship.action_slot).value()));
  if (gizmo_of(session, slot.gizmo).uses != 0) {
    if (slot.uses_left == 0) {
      return 0;
    }
    if (apply) {
      --slot.uses_left;
    }
  }
  switch (ship.target_kind) {
    case kTargetShip:
      return ship_use_gizmo(session, ship, apply);
    case kTargetLane:
      return ship_use_gizmo_on_lane(session, ship, apply);
    case kTargetPlanet:
      return ship_use_gizmo_on_planet(session, ship, apply);
    case kTargetNone:
      return ship_use_gizmo_untargeted(session, ship, apply);
    default:
      return 0;
  }
}

// 0x45844.
std::int32_t ship_gizmos_industry(const Session& session, const Ship& ship) {
  std::int32_t sum = 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const std::int8_t g = ship.slots[static_cast<std::size_t>(i)].gizmo;
    if (g != -1) {
      sum += gizmo_of(session, g).industry;
    }
  }
  return sum;
}

// 0x4588c.
std::int32_t ship_industry_cost(const Session& session, const Ship& ship) {
  return kHullCost.at(ship.hull_size) + ship_gizmos_industry(session, ship);
}

// 0x458b8 (`level` compared with the best so far as a short; ties go to the
// last).
std::int8_t ship_best_gizmo_for_race(const Session& session, std::uint8_t category, RaceIndex race) {
  const Research& research = session.game->research;
  std::int16_t best_level = 0;
  std::int8_t best = -1;
  for (std::size_t i = 0; i < kGizmoCount; ++i) {
    const Gizmo& g = session.tables->gizmos[i];
    if (g.category == category && race_knows(research, g.research_requirement, race) && best_level <= g.level) {
      best = static_cast<std::int8_t>(i);
      best_level = wrap_i16(g.level);
    }
  }
  return best;
}

// 0x45950.
bool ship_race_has_gizmo(const Session& session, const Ship& ship, std::int8_t gizmo) {
  return race_knows(session.game->research, gizmo_of(session, gizmo).research_requirement, ship.owner);
}

// 0x459cc. The Antagonizer (0x4a480) also fires the Phase Bomb at planets.
std::int32_t ship_ai_select_weapon(Session& session, Ship& ship, std::uint16_t target_kind, RawAddress target) {
  if (ship.power == 0) {
    return -1;
  }
  ship.action = 1;
  if (target_kind == 0) {
    ship.target_kind = kTargetLane;
  } else if (target_kind < 3) {
    ship.target_kind = kTargetPlanet;
  } else {
    ship.target_kind = kTargetShip;
  }
  ship.target = target;
  const bool antagonizer = antagonizer_rules(session.game->variant);
  const ShipIndex self = ship_index(session.game->cosmos, ship);
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const std::int8_t g = ship.slots[static_cast<std::size_t>(i)].gizmo;
    if (g == -1) {
      continue;
    }
    if (gizmo_of(session, g).category == kCategoryWeapon ||
        (antagonizer && ship.target_kind == kTargetPlanet && g == kGizmoPhaseBomb)) {
      ship.action_slot = gizmo_slot_address(session, self, i);
      if (ship_do_action(session, ship, false) & kActionPossible) {
        return i;
      }
    }
  }
  return -1;
}

// 0x45a74. An empty slot is not skipped: it reads as a weapon with limited
// uses (see `kEmptySlotUses`), ready when its stale `uses_left` is not 0.
bool ship_has_ready_weapon(const Session& session, const Ship& ship) {
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const GizmoSlot& slot = ship.slots[static_cast<std::size_t>(i)];
    const std::uint8_t category = slot.gizmo == -1 ? kEmptySlotCategory : gizmo_of(session, slot.gizmo).category;
    const std::int32_t uses = slot.gizmo == -1 ? kEmptySlotUses : gizmo_of(session, slot.gizmo).uses;
    if (category == kCategoryWeapon && (uses == 0 || slot.uses_left != 0)) {
      return true;
    }
  }
  return false;
}

// 0x45ac8.
void ship_reset_battle_moves(Session& session, Ship& ship) {
  if (ship.battle_flags & kShipImmobile) {
    ship.moves = 0;
  } else if (ship.battle_flags & kShipToroidal) {
    ship.moves = ship.drive_speed + session.tables->gizmos[63].special1;
  } else {
    ship.moves = ship.drive_speed;
  }
  ship.shield = 0;
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    const GizmoSlot& slot = ship.slots[static_cast<std::size_t>(i)];
    if (slot.gizmo == -1) {
      continue;
    }
    const Gizmo& g = gizmo_of(session, slot.gizmo);
    if ((g.flags & kGizmoToggleable) != 0 && slot.toggled == -1 && g.power <= ship.power) {
      ship.power -= g.power;
      if (g.category == kCategoryShield) {
        ship.shield += g.shield_strength;
      }
    }
  }
  if (ship.battle_flags & kShipShieldsDown) {
    ship.shield = 0;
  }
}

// 0x45bbc.
void ship_new_day(Session& session, Ship& ship) {
  Cosmos& cosmos = session.game->cosmos;
  ship.battle_flags = 0;
  ship.power = ship_battle_power(session, ship);
  for (std::int32_t i = 0; i < ship.slot_count; ++i) {
    GizmoSlot& slot = ship.slots[static_cast<std::size_t>(i)];
    if (slot.gizmo != -1) {
      slot.uses_left = wrap_i16(gizmo_of(session, slot.gizmo).uses);
    }
  }
  if (ship.location_kind == ShipLocation::kLane) {
    const Lane& lane = cosmos.lanes.at(static_cast<std::size_t>(ship.location));
    const Star& a = cosmos.stars.at(static_cast<std::size_t>(lane.star_a));
    const Star& b = cosmos.stars.at(static_cast<std::size_t>(lane.star_b));
    const float lane_length = f32(length(sub(a.position, b.position)));
    float step = static_cast<float>(ship.lane_speed + ship.hyper_speed);
    if (lane.flags & 1) {
      step = f32(static_cast<double>(step) * 0.33333334f);
    }
    if (cosmos.races.at(static_cast<std::size_t>(ship.owner)).ability == kAbilityChronomyst) {
      if (step < 1.0f) {
        step = 1.0f;
      }
      step = f32(static_cast<double>(step) * 2.0f);
    }
    // The clamp compares the unrounded sum.
    const double travelled = static_cast<double>(ship.position[0]) + step;
    ship.position[0] = f32(travelled);
    if (travelled > lane_length) {
      ship.position[0] = lane_length;
    }
    if (ship.position[0] == lane_length) {
      Star& star = cosmos.stars.at(static_cast<std::size_t>(ship.position[1] == 0.0f ? lane.star_a : lane.star_b));
      Race& player = cosmos.races.at(session.player_race);
      if (ship.owner == session.player_race) {
        race_on_player_ship_arrival(session, player, ship, star);
      } else if (((star.ships_mask | star.owner_mask) >> session.player_race) & 1) {
        race_on_alien_ship_arrival(session, player, ship, star);
      }
      ship_enter_star_system(session, star, ship);
    }
  }
  ship_reset_battle_moves(session, ship);
}

// 0x45ddc.
std::int32_t ship_experience(const Session& session, const Ship& ship) {
  const std::int32_t e = (session.game->cosmos.day - ship.built_day) / 100;
  return e < 0 ? 0 : e > 10 ? 10 : e;
}

// 0x45e0c: `generator * clamp(1 + experience * 0.1f, 1, 2)` (the factor
// stored as a float).
std::int32_t ship_battle_power(const Session& session, const Ship& ship) {
  float factor = f32(static_cast<double>(ship_experience(session, ship)) * static_cast<double>(0.1f) + 1.0);
  if (1.0f > factor) {
    factor = 1.0f;
  } else if (factor > 2.0f) {
    factor = 2.0f;
  }
  return chop(static_cast<double>(ship.generator_power) * factor);
}

// 0x45e80 (the ship must be in space).
std::uint8_t ship_set_destination_star(Session& session, Ship& ship, const Star& star) {
  const Cosmos& cosmos = session.game->cosmos;
  const StarIndex target = star_index(cosmos, star);
  if (target == ship.location) {
    return 0;
  }
  ship.destination_kind = ShipOrder::kGoStar;
  ship.destination = target;
  return cosmos.hops.at(static_cast<std::size_t>(ship.location)).at(static_cast<std::size_t>(target));
}

// 0x45ef4: 0 unseen, 1 seen, 2 seen by a ship with X Ray Megaglasses.
std::int32_t ship_detected_by_race(const Session& session, const Ship& ship, RaceIndex race) {
  const Cosmos& cosmos = session.game->cosmos;
  std::int32_t seen = 0;
  if (ship.location_kind != ShipLocation::kSystem) {
    return 0;
  }
  const Star& star = cosmos.stars.at(static_cast<std::size_t>(ship.location));
  for (const ShipIndex i : cosmos_ships_at_star(cosmos, star)) {
    const Ship& other = cosmos.ships[static_cast<std::size_t>(i)];
    if (other.owner != race || other.location_kind != ShipLocation::kSystem) {
      continue;
    }
    const float distance = f32(length(sub(ship.position, other.position)));
    std::int32_t scanners = 0;
    for (std::int32_t s = 0; s < other.slot_count; ++s) {
      const std::int8_t g = other.slots[static_cast<std::size_t>(s)].gizmo;
      if (g != -1 && gizmo_of(session, g).category == kCategoryScanner &&
          distance <= static_cast<float>(gizmo_of(session, g).range * 32)) {
        scanners += gizmo_of(session, g).scanner_level;
      }
    }
    if (scanners > 0 && ship.cloakers * 2 < scanners) {
      seen = 1;
      if (ship_find_gizmo(other, kGizmoXRayMegaglasses) != -1) {
        return 2;
      }
    }
  }
  if (seen == 0) {
    for (std::int16_t p = 0; p < star.planet_count; ++p) {
      const Planet& planet = cosmos.planets.at(static_cast<std::size_t>(star.planets[static_cast<std::size_t>(p)]));
      if (planet.owner != race) {
        continue;
      }
      const float distance = f32(length(sub(ship.position, planet.position)));
      const std::uint16_t square = planet_find_item(cosmos, planet, kItemObservation);
      if (square != kNoSquare && (planet_squares(cosmos, planet)[square].flags & kSquareBuilt) != 0 &&
          distance < 1600.0f) {
        return 1;
      }
    }
  }
  return seen;
}

bool ship_has_gizmo(const Ship& ship, std::int8_t gizmo) {
  for (std::int32_t i = 0; i < ship.slot_count && i < static_cast<std::int32_t>(kShipSlots); ++i) {
    if (ship.slots[static_cast<std::size_t>(i)].gizmo == gizmo) {
      return true;
    }
  }
  return false;
}

}  // namespace oa::game
