// The effect animations of the System Display (`batfx.cpp`, `batfxa.cpp`;
// `docs/08-combat.md` "Effects and sounds"): `BatModeWnd`'s effect timer and
// one player per kind of action, moving the effect sprites (display item 1)
// and the items concerned through the effect positions (`+0x4f80`), with
// the `batfx.txt` shapes and sounds. They change nothing of the game: the
// action is applied when its effect ends (`BatControlWnd_finish_action`).
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "game/address.h"
#include "game/cosmos.h"
#include "game/race.h"
#include "game/session.h"
#include "game/ship.h"
#include "gfx/vfx_poly.h"
#include "screens/bat_control_wnd.h"
#include "screens/bat_mode_wnd.h"

namespace oa {

namespace {

using game::f32;

double d(float v) { return static_cast<double>(v); }

// The gizmos with an effect of their own (`BatModeWnd_effect_tick`).
constexpr int kGizmoFleetDisperser = 40;
constexpr int kGizmoGravityDistorter = 39;
constexpr int kGizmoLaneBlocker = 32;
constexpr int kGizmoRecaller = 36;
constexpr int kGizmoSmartBomb = 38;
constexpr int kGizmoPositronBouncer = 45;
constexpr int kGizmoGravCatapult = 46;
constexpr int kGizmoTractorBeam = 52;
constexpr int kGizmoHyperswapper = 55;
constexpr int kGizmoGravCondensor = 56;
constexpr int kGizmoRemoteRepair = 58;
constexpr int kGizmoSacrificialOrb = 59;
constexpr int kGizmoLaneMagnetron = 60;
constexpr int kGizmoDisintegrator = 61;
constexpr int kGizmoPlasmaCoupler = 68;
constexpr int kGizmoSelfDestruct = 72;
constexpr int kGizmoMassCondensor = 74;
// The planet items with an effect: the Tractor Beam; the batfx.txt rows of
// the Orbital Missile Base and the Short and Long Range Whoppers.
constexpr std::uint8_t kItemTractorBeam = 0x11;
constexpr std::uint8_t kItemMissileBase = 0x1c;
constexpr std::uint8_t kItemShortWhopper = 0x1d;
constexpr std::uint8_t kItemLongWhopper = 0x1e;
constexpr int kRowMissileBase = 0x4c;

// The GZ_NOTARGET flag.
constexpr std::uint16_t kGizmoNoTarget = 0x40;

// `batfx.txt` shape files: the shield hit, the explosions, the lane exits.
constexpr int kFileShield = 4;
constexpr int kFileDestroy = 5;
constexpr int kFileBlueExit = 7;
constexpr int kFileRedExit = 8;
// Sounds: the shield hit, the explosion, the thrust.
constexpr int kSfxShield = 1;
constexpr int kSfxExplosion = 5;
constexpr int kSfxThrust = 7;

// The effect sprite's display item (the scene's free item).
constexpr int kSpriteItem = 1;

// The effects (`+0x4f37`).
enum Effect : std::uint8_t {
  kFxMove = 0,
  kFxSelf = 1,
  kFxProjectile = 2,
  kFxTractor = 3,
  kFxHyperswap = 4,
  kFxLaneBlocker = 5,
  kFxCondensor = 6,
  kFxRemoteRepair = 7,
  kFxGravCatapult = 8,
  kFxLaneMagnetron = 9,
  kFxTransfer = 10,
  kFxPositronBouncer = 11,
  kFxDisperser = 12,
  kFxRecaller = 13,
  kFxSelfDestruct = 14,
  kFxSmartBomb = 15,
  kFxNone = 0xff,
};

}  // namespace

bool BatModeWnd::step_toward(Vec3& p, const Vec3& target, float step) {
  const Vec3 dv = game::sub(target, p);
  const float len = f32(game::length(dv));
  if (step >= len) {
    p = target;
    return true;
  }
  const Vec3 a = {f32(d(dv[0]) * step), f32(d(dv[1]) * step), f32(d(step) * dv[2])};
  const double inv = 1.0 / d(len);
  const Vec3 t = {f32(d(a[0]) * inv), f32(d(a[1]) * inv), f32(d(a[2]) * inv)};
  p = {f32(d(p[0]) + t[0]), f32(d(p[1]) + t[1]), f32(d(p[2]) + t[2])};
  return false;
}

BatModeWnd::Item& BatModeWnd::effect_sprite() { return items_[kSpriteItem]; }

void BatModeWnd::start_effect() {
  effect_ = kFxNone;
  selected_ = -1;
  fx_count_ = 0;
  state_ = State::kAnimating;
  wm_.set_timer(id_, kMessageEffectTick, 0, 0, 0);
}

void BatModeWnd::effect_tick() {
  game::Session* s = core();
  BatControlWnd* ctrl = control();
  if (s == nullptr || ctrl == nullptr) {
    return;
  }
  const game::Cosmos& c = s->game->cosmos;
  bool done = false;
  if (effect_ == kFxNone) {
    build_scene();
    refresh();
    tick_ = 0;
    const BatControlWnd::Selection sel = ctrl->selection();
    if (sel.kind == BatControlWnd::kObjectShip) {
      const game::Ship& ship = c.ships[static_cast<std::size_t>(sel.object)];
      if (ship.action == 0) {
        effect_ = kFxMove;
      } else if (ship.action == 1) {
        const auto slot = game::gizmo_slot_at(*s, static_cast<game::ShipIndex>(sel.object), ship.action_slot);
        const int g = slot ? ship.slots[static_cast<std::size_t>(*slot)].gizmo : -1;
        switch (g) {
          case kGizmoTractorBeam:
            effect_ = kFxTractor;
            break;
          case kGizmoHyperswapper:
            effect_ = kFxHyperswap;
            break;
          case kGizmoLaneBlocker:
            effect_ = kFxLaneBlocker;
            break;
          case kGizmoGravCondensor:
          case kGizmoMassCondensor:
            effect_ = kFxCondensor;
            break;
          case kGizmoRemoteRepair:
            effect_ = kFxRemoteRepair;
            break;
          case kGizmoGravCatapult:
            effect_ = kFxGravCatapult;
            break;
          case kGizmoLaneMagnetron:
            effect_ = kFxLaneMagnetron;
            break;
          case kGizmoSacrificialOrb:
          case kGizmoPlasmaCoupler:
            effect_ = kFxTransfer;
            break;
          case kGizmoPositronBouncer:
            effect_ = kFxPositronBouncer;
            break;
          case kGizmoFleetDisperser:
          case kGizmoGravityDistorter:
            effect_ = kFxDisperser;
            break;
          case kGizmoRecaller:
            effect_ = kFxRecaller;
            break;
          case kGizmoSelfDestruct:
            effect_ = kFxSelfDestruct;
            break;
          case kGizmoSmartBomb:
            effect_ = kFxSmartBomb;
            break;
          default:
            effect_ = g >= 0 && (s->tables->gizmos.at(static_cast<std::size_t>(g)).flags & kGizmoNoTarget) != 0
                          ? kFxSelf
                          : kFxProjectile;
            break;
        }
      }
    } else if (sel.kind == BatControlWnd::kObjectPlanet) {
      const game::Planet& planet = c.planets[static_cast<std::size_t>(sel.object)];
      const auto square = game::square_at(*s, planet.fire_square);
      const std::uint8_t item = square ? c.squares[static_cast<std::size_t>(*square)].item : 0;
      effect_ = item == kItemTractorBeam ? kFxTractor : kFxProjectile;
    }
  }
  switch (effect_) {
    case kFxMove:
      done = fx_move();
      break;
    case kFxSelf:
      done = fx_self();
      break;
    case kFxProjectile:
      done = fx_projectile();
      break;
    case kFxTractor:
      done = fx_tractor();
      break;
    case kFxHyperswap:
      done = fx_hyperswap();
      break;
    case kFxLaneBlocker:
      done = fx_lane_blocker();
      break;
    case kFxCondensor:
      done = fx_condensor();
      break;
    case kFxRemoteRepair:
      done = fx_remote_repair();
      break;
    case kFxGravCatapult:
      done = fx_grav_catapult();
      break;
    case kFxLaneMagnetron:
      done = fx_lane_magnetron();
      break;
    case kFxTransfer:
      done = fx_transfer();
      break;
    case kFxPositronBouncer:
      done = fx_positron_bouncer();
      break;
    case kFxDisperser:
      done = fx_disperser();
      break;
    case kFxRecaller:
      done = fx_recaller();
      break;
    case kFxSelfDestruct:
      done = fx_self_destruct();
      break;
    case kFxSmartBomb:
      done = fx_smart_bomb();
      break;
    default:
      done = true;
      break;
  }
  if (skip_) {
    skip_ = false;
    done = true;
  }
  refresh(false);
  draw(1);
  ++tick_;
  if (done) {
    state_ = State::kIdle;
    wm_.kill_timer(id_, kMessageEffectTick);
    ctrl->finish_action(true);
    build_scene();
    refresh();
    effect_ = kFxNone;
    for (int i = 0; i < fx_count_; ++i) {
      items_[static_cast<std::size_t>(fx_items_[static_cast<std::size_t>(i)])].flags &= kFlagStarSideA;
    }
  }
}

bool BatModeWnd::fx_move() {
  game::Session* s = core();
  const BatControlWnd::Selection sel = control()->selection();
  if (sel.kind != BatControlWnd::kObjectShip) {
    throw std::logic_error("star system: a move effect without a ship");
  }
  const game::Ship& ship = s->game->cosmos.ships[static_cast<std::size_t>(sel.object)];
  if (ship.target_kind != game::kTargetPoint && ship.target_kind != game::kTargetPlanet &&
      ship.target_kind != game::kTargetLane) {
    throw std::logic_error("star system: a move effect without a target");
  }
  bool done = false;
  if (tick_ == 0) {
    fx_count_ = 1;
    fx_items_[0] = find_item(ItemKind::kShip, sel.object);
    Item& item = items_[static_cast<std::size_t>(fx_items_[0])];
    item.flags = kFlagNoLine | kFlagFxPosition;
    item.fx_position = 0;
    fx_pos_[0] = ship.position;
    phase_ = 0;
    if (ship.moves != 0 && ship.power != 0) {
      wm_.services().sound.play_sfx(kSfxThrust);
    }
  }
  if (phase_ == 0) {
    if (step_toward(fx_pos_[0], ship.action_point, 20.0F)) {
      if ((ship.action_result & game::kActionReached) != 0 && ship.target_kind == game::kTargetLane) {
        const auto lane = game::lane_at(*s, ship.target);
        if (lane && game::ship_can_enter_lane(*s, ship, s->game->cosmos.lanes[static_cast<std::size_t>(*lane)])) {
          items_[static_cast<std::size_t>(fx_items_[0])].flags |= kFlagHidden;
          fx_count_ = 2;
          fx_items_[1] = find_item(ItemKind::kLaneEnd, *lane);
          phase_ = 1;
          return false;
        }
      }
      return true;
    }
  } else {
    const auto lane = game::lane_at(*s, ship.target);
    const int file =
        lane && (s->game->cosmos.lanes[static_cast<std::size_t>(*lane)].flags & 1) != 0 ? kFileRedExit : kFileBlueExit;
    int frame = phase_ >> 2;
    const int last = static_cast<int>(file_shapes(file).size()) - 1;
    if (last < frame && frame <= last * 2) {
      frame = last * 2 - frame;
    } else if (last * 2 < frame) {
      done = true;
      frame = 0;
    }
    set_item_file(items_[static_cast<std::size_t>(fx_items_[1])], file, frame);
    ++phase_;
  }
  return done;
}

bool BatModeWnd::fx_self() {
  game::Session* s = core();
  const BatControlWnd::Selection sel = control()->selection();
  if (sel.kind != BatControlWnd::kObjectShip) {
    throw std::logic_error("star system: a gizmo effect without a ship");
  }
  const game::Ship& ship = s->game->cosmos.ships[static_cast<std::size_t>(sel.object)];
  const auto slot = game::gizmo_slot_at(*s, static_cast<game::ShipIndex>(sel.object), ship.action_slot);
  const int g = slot ? ship.slots[static_cast<std::size_t>(*slot)].gizmo : 0;
  if (tick_ == 0) {
    phase_ = 0;
    fx_count_ = 1;
    fx_items_[0] = kSpriteItem;
    Item& sprite = effect_sprite();
    sprite.kind = ItemKind::kEffect;
    sprite.flags = kFlagNoLine | kFlagFxPosition;
    sprite.fx_position = 0;
    fx_pos_[0] = ship.position;
    const EffectRow& row = effect_rows_.at(static_cast<std::size_t>(g));
    set_item_file(sprite, row.shape1, 0);
    wm_.services().sound.play_sfx(row.sfx1);
  }
  return (tick_ & 3) == 0 && next_frame(effect_sprite());
}

bool BatModeWnd::fx_projectile() {
  game::Session* s = core();
  const BatControlWnd::Selection sel = control()->selection();
  const game::Cosmos& c = s->game->cosmos;
  Vec3 start{};
  Vec3 end{};
  int row = 0;
  std::uint32_t result = 0;
  std::uint8_t target_kind = game::kTargetShip;
  // The display item hit and, for a ship, its hull.
  int target_item = -1;
  int target_hull = 0;
  const auto find_target = [&](game::RawAddress raw) {
    if (const auto ship = game::ship_at(*s, raw)) {
      target_item = find_item(ItemKind::kShip, *ship);
      target_hull = static_cast<std::int8_t>(c.ships[static_cast<std::size_t>(*ship)].hull_size);
    } else if (const auto planet = game::planet_at(*s, raw)) {
      target_item = find_item(ItemKind::kPlanet, *planet);
    } else if (const auto lane = game::lane_at(*s, raw)) {
      target_item = find_item(ItemKind::kLaneEnd, *lane);
    }
  };
  if (sel.kind == BatControlWnd::kObjectShip) {
    const game::Ship& ship = c.ships[static_cast<std::size_t>(sel.object)];
    start = ship.position;
    end = ship.action_point;
    const auto slot = game::gizmo_slot_at(*s, static_cast<game::ShipIndex>(sel.object), ship.action_slot);
    row = slot ? ship.slots[static_cast<std::size_t>(*slot)].gizmo : 0;
    result = static_cast<std::uint32_t>(ship.action_result);
    target_kind = ship.target_kind;
    find_target(ship.target);
  } else if (sel.kind == BatControlWnd::kObjectPlanet) {
    const game::Planet& planet = c.planets[static_cast<std::size_t>(sel.object)];
    start = planet.position;
    const auto target = game::ship_at(*s, planet.fire_target);
    if (target) {
      end = c.ships[static_cast<std::size_t>(*target)].position;
    }
    result = planet.fire_result;
    const auto square = game::square_at(*s, planet.fire_square);
    const std::uint8_t item = square ? c.squares[static_cast<std::size_t>(*square)].item : 0;
    row = item == kItemMissileBase    ? kRowMissileBase
          : item == kItemShortWhopper ? kRowMissileBase + 1
          : item == kItemLongWhopper  ? kRowMissileBase + 2
                                      : 0;
    find_target(planet.fire_target);
  } else {
    throw std::logic_error("star system: a projectile effect without a shooter");
  }
  const EffectRow& rows = effect_rows_.at(static_cast<std::size_t>(row));
  bool done = false;
  if (tick_ == 0) {
    fx_count_ = 2;
    phase_ = 0;
    fx_items_[0] = kSpriteItem;
    Item& sprite = effect_sprite();
    sprite.kind = ItemKind::kEffect;
    sprite.flags = kFlagNoLine | kFlagFxPosition;
    sprite.fx_position = 0;
    fx_pos_[0] = start;
    set_item_file(sprite, rows.shape1, 0);
    fx_items_[1] = target_item;
    wm_.services().sound.play_sfx(rows.sfx1);
  }
  if (phase_ == 0) {
    const Vec3 dv = game::sub(end, start);
    const float step = f32(std::trunc(game::length(dv) * d(0.015625F) + d(20.0F)));
    if (step_toward(fx_pos_[0], end, step)) {
      phase_ = 1;
      return false;
    }
    next_frame(effect_sprite());
    return false;
  }
  float scale = 1.0F;
  int sfx = kSfxShield;
  int shape = kFileShield;
  if ((result & game::kActionDestroyed) != 0 || row == kGizmoDisintegrator) {
    if (target_item >= 0 && items_[static_cast<std::size_t>(target_item)].kind == ItemKind::kShip) {
      scale = f32(static_cast<double>(target_hull) * (1.0 / 3.0) + 1.0);
    }
    sfx = kSfxExplosion;
    if (target_item >= 0) {
      items_[static_cast<std::size_t>(target_item)].flags |= kFlagHidden;
    }
    shape = (row & 1) == 0 ? kFileDestroy + 1 : kFileDestroy;
    if (row == kGizmoDisintegrator) {
      sfx = rows.sfx2;
      shape = rows.shape2;
    }
  } else if ((result & game::kActionPenetrated) != 0 || (result & game::kActionNoDamage) != 0 ||
             target_kind == game::kTargetPlanet || target_kind == game::kTargetLane) {
    sfx = rows.sfx2;
    shape = rows.shape2;
  }
  Item& sprite = effect_sprite();
  if (phase_ == 1) {
    set_item_file(sprite, shape, 0);
    wm_.services().sound.play_sfx(sfx);
    phase_ = 2;
  }
  if ((tick_ & 3) == 0 && next_frame(sprite)) {
    sprite.flags |= kFlagHidden;
    done = true;
  }
  sprite.scale = scale;
  return done;
}

game::Ship& BatModeWnd::fx_ship() {
  const BatControlWnd::Selection sel = control()->selection();
  if (sel.kind != BatControlWnd::kObjectShip) {
    throw std::logic_error("star system: a gizmo effect without a ship");
  }
  return core()->game->cosmos.ships[static_cast<std::size_t>(sel.object)];
}

int BatModeWnd::fx_gizmo() {
  const game::Session& s = *core();
  const BatControlWnd::Selection sel = control()->selection();
  const game::Ship& ship = fx_ship();
  const auto slot = game::gizmo_slot_at(s, static_cast<game::ShipIndex>(sel.object), ship.action_slot);
  return slot ? ship.slots[static_cast<std::size_t>(*slot)].gizmo : 0;
}

int BatModeWnd::fx_item_at(game::RawAddress raw) const {
  const game::Session& s = *core();
  if (const auto ship = game::ship_at(s, raw)) {
    return find_item(ItemKind::kShip, *ship);
  }
  if (const auto planet = game::planet_at(s, raw)) {
    return find_item(ItemKind::kPlanet, *planet);
  }
  if (const auto lane = game::lane_at(s, raw)) {
    return find_item(ItemKind::kLaneEnd, *lane);
  }
  return -1;
}

void BatModeWnd::fx_add_ships(game::StarIndex star, int except) {
  const game::Cosmos& c = core()->game->cosmos;
  for (const game::ShipIndex i : game::cosmos_ships_at_star(c, c.stars[static_cast<std::size_t>(star)])) {
    if (fx_count_ > 14) {
      break;
    }
    const game::Ship& ship = c.ships[static_cast<std::size_t>(i)];
    if (ship.location_kind != game::ShipLocation::kSystem || i == except) {
      continue;
    }
    const int k = find_item(ItemKind::kShip, i);
    Item& item = items_[static_cast<std::size_t>(k)];
    item.flags |= kFlagNoLine | kFlagFxPosition;
    item.fx_position = static_cast<std::uint8_t>(fx_count_);
    fx_pos_[item.fx_position] = ship.position;
    fx_items_[static_cast<std::size_t>(fx_count_++)] = k;
  }
}

bool BatModeWnd::fx_tractor() {
  game::Session* s = core();
  const BatControlWnd::Selection sel = control()->selection();
  const game::Cosmos& c = s->game->cosmos;
  Vec3 shooter{};
  float reach = 0;
  int shooter_item = -1;
  int target = -1;  // ship index
  if (sel.kind == BatControlWnd::kObjectShip) {
    const game::Ship& ship = c.ships[static_cast<std::size_t>(sel.object)];
    if (ship.target_kind != game::kTargetShip) {
      throw std::logic_error("star system: a tractor beam on no ship");
    }
    target = game::ship_at(*s, ship.target).value_or(-1);
    shooter_item = find_item(ItemKind::kShip, sel.object);
    shooter = ship.position;
    reach = static_cast<float>(s->tables->gizmos.at(kGizmoTractorBeam).special1 << 5);
  } else if (sel.kind == BatControlWnd::kObjectPlanet) {
    const game::Planet& planet = c.planets[static_cast<std::size_t>(sel.object)];
    target = game::ship_at(*s, planet.fire_target).value_or(-1);
    shooter_item = find_item(ItemKind::kPlanet, sel.object);
    shooter = planet.position;
    reach = 3200.0F;
  } else {
    throw std::logic_error("star system: a tractor beam without a shooter");
  }
  if (target < 0) {
    throw std::logic_error("star system: a tractor beam on no ship");
  }
  const game::Ship& pulled = c.ships[static_cast<std::size_t>(target)];
  if (tick_ == 0) {
    fx_count_ = 2;
    phase_ = 0;
    fx_items_[0] = kSpriteItem;
    Item& beam = effect_sprite();
    beam.kind = ItemKind::kBeam;
    beam.object = shooter_item;
    // The beam draws only with a shape slot: the original's item keeps a
    // stale one.
    if (beam.shapes == nullptr) {
      beam.shapes = &file_shapes(0);
    }
    beam.flags |= kFlagNoLine | kFlagFxPosition;
    beam.fx_position = 0;
    fx_pos_[0] = shooter;
    fx_items_[1] = find_item(ItemKind::kShip, target);
    Item& item = items_[static_cast<std::size_t>(fx_items_[1])];
    item.flags = kFlagNoLine | kFlagFxPosition;
    item.fx_position = 1;
    fx_pos_[1] = pulled.position;
    wm_.services().sound.play_sfx(kSfxShield);
  }
  if (phase_ == 0) {
    if (step_toward(fx_pos_[0], fx_pos_[1], 40.0F)) {
      items_[static_cast<std::size_t>(fx_items_[1])].flags |= kFlagNoLine;
      Vec3 dv = game::sub(fx_pos_[1], shooter);
      const double left = game::length(dv) - d(reach);
      float length = f32(left);
      // The pull's minimum: 64, 1280 in the Antagonizer (ANTAG 0x13370) for
      // both tractors, whatever the rule did.
      const float minimum = game::antagonizer_rules(s->game->variant) ? 1280.0F : 64.0F;
      if (left < d(minimum)) {
        length = minimum;
      }
      game::set_length(dv, length);
      fx_pos_[2] = {f32(d(shooter[0]) + dv[0]), f32(d(shooter[1]) + dv[1]), f32(d(shooter[2]) + dv[2])};
      phase_ = 1;
    }
    return false;
  }
  if (phase_ == 1) {
    if (step_toward(fx_pos_[1], fx_pos_[2], 4.0F)) {
      phase_ = 2;
    }
    fx_pos_[0] = fx_pos_[1];
    return false;
  }
  return true;
}

bool BatModeWnd::fx_hyperswap() {
  const game::Session& s = *core();
  const game::Ship& ship = fx_ship();
  const auto target = game::ship_at(s, ship.target);
  if (tick_ == 0) {
    fx_count_ = 2;
    fx_items_[0] = find_item(ItemKind::kShip, control()->selection().object);
    Item& user = items_[static_cast<std::size_t>(fx_items_[0])];
    user.flags = kFlagCustomDraw | kFlagFxPosition;
    user.fx_position = 0;
    fx_pos_[0] = ship.position;
    fx_items_[1] = target ? find_item(ItemKind::kShip, *target) : -1;
    if (fx_items_[1] >= 0) {
      Item& other = items_[static_cast<std::size_t>(fx_items_[1])];
      other.flags = kFlagCustomDraw | kFlagFxPosition;
      other.fx_position = 1;
      fx_pos_[1] = s.game->cosmos.ships[static_cast<std::size_t>(*target)].position;
    }
  }
  if (tick_ == 0x14) {
    std::swap(fx_pos_[0], fx_pos_[1]);
  } else if (tick_ == 0x29) {
    return true;
  }
  return false;
}

bool BatModeWnd::fx_lane_blocker() {
  const game::Ship& ship = fx_ship();
  if (ship.target_kind != game::kTargetLane) {
    throw std::logic_error("star system: a lane blocker on no lane");
  }
  const int g = fx_gizmo();
  const EffectRow& row = effect_rows_.at(static_cast<std::size_t>(g));
  if (tick_ == 0) {
    fx_count_ = 2;
    phase_ = 0;
    fx_items_[0] = kSpriteItem;
    Item& sprite = effect_sprite();
    sprite.kind = ItemKind::kEffect;
    sprite.flags = kFlagNoLine | kFlagFxPosition;
    sprite.fx_position = 0;
    fx_pos_[0] = ship.position;
    set_item_file(sprite, row.shape1, 0);
    fx_items_[1] = fx_item_at(ship.target);
    wm_.services().sound.play_sfx(row.sfx1);
  }
  if (phase_ == 0) {
    if (step_toward(fx_pos_[0], ship.action_point, 40.0F)) {
      phase_ = 1;
      effect_sprite().kind = ItemKind::kFree;
      items_[static_cast<std::size_t>(fx_items_[1])].flags |= kFlagCustomDraw;
      wm_.services().sound.play_sfx(row.sfx2);
    }
    return false;
  }
  return ++phase_ > 0x14;
}

bool BatModeWnd::fx_condensor() {
  game::Session& s = *core();
  const game::Cosmos& c = s.game->cosmos;
  const game::Ship& ship = fx_ship();
  const int g = fx_gizmo();
  const bool mass = g == kGizmoMassCondensor;
  int target = -1;
  int first = 1;
  if (mass) {
    if (ship.target_kind != game::kTargetShip) {
      throw std::logic_error("star system: a mass condensor on no ship");
    }
    target = game::ship_at(s, ship.target).value_or(-1);
    first = 2;
  }
  const EffectRow& row = effect_rows_.at(static_cast<std::size_t>(g));
  if (tick_ == 0) {
    fx_count_ = 1;
    if (mass) {
      fx_count_ = 2;
      fx_items_[0] = find_item(ItemKind::kShip, target);
      fx_items_[1] = kSpriteItem;
      Item& sprite = effect_sprite();
      sprite.kind = ItemKind::kEffect;
      sprite.flags = kFlagNoLine | kFlagFxPosition;
      sprite.fx_position = 1;
      fx_pos_[1] = ship.position;
      set_item_file(sprite, row.shape1, 0);
    } else {
      fx_items_[0] = find_item(ItemKind::kStar, ship.location);
    }
    fx_add_ships(ship.location, target);
    phase_ = 0;
    wm_.services().sound.play_sfx(row.sfx1);
  }
  Item& centre = items_[static_cast<std::size_t>(fx_items_[0])];
  switch (phase_) {
    case 0:
      if (mass) {
        if (!step_toward(fx_pos_[1], c.ships[static_cast<std::size_t>(target)].position, 10.0F)) {
          next_frame(effect_sprite());
          return false;
        }
        effect_sprite().flags |= kFlagHidden;
      }
      phase_ = 1;
      return false;
    case 1:
      centre.scale = f32(d(centre.scale) + d(0.04F));
      condense_ = 0.0F;
      if (centre.scale >= 2.0F) {
        phase_ = 2;
      }
      return false;
    case 2: {
      const Vec3 at = mass ? c.ships[static_cast<std::size_t>(target)].position : Vec3{};
      const game::Gizmo& gizmo = s.tables->gizmos.at(static_cast<std::size_t>(g));
      const float pull = static_cast<float>(gizmo.special1);
      const float minimum = static_cast<float>(gizmo.special2 << 5);
      const double left = d(pull) - condense_;
      float step = f32(left);
      if (left > 0.2) {
        step = 0.2F;
      }
      condense_ = f32(d(condense_) + step);
      bool moved = false;
      for (int i = first; i < fx_count_; ++i) {
        Vec3& p = fx_pos_[items_[static_cast<std::size_t>(fx_items_[static_cast<std::size_t>(i)])].fx_position];
        Vec3 dv = game::sub(p, at);
        const double len = game::length(dv);
        if (!(len > minimum)) {
          continue;
        }
        const double shorter = d(f32(len)) - d(step) * d(32.0F);
        float length = f32(shorter);
        if (shorter >= minimum) {
          moved = true;
        } else {
          length = minimum;
        }
        game::set_length(dv, length);
        p = {f32(d(at[0]) + dv[0]), f32(d(at[1]) + dv[1]), f32(d(at[2]) + dv[2])};
      }
      if (condense_ >= pull || !moved) {
        phase_ = 3;
      }
      return false;
    }
    default:
      centre.scale = f32(d(centre.scale) + d(-0.04F));
      if (1.0F >= centre.scale) {
        centre.scale = 1.0F;
        return true;
      }
      return false;
  }
}

bool BatModeWnd::fx_remote_repair() {
  const int g = fx_gizmo();
  if (tick_ == 0) {
    fx_count_ = 1;
    fx_items_[0] = find_item(ItemKind::kShip, control()->selection().object);
    items_[static_cast<std::size_t>(fx_items_[0])].flags |= kFlagCustomDraw;
    phase_ = 0;
    fade_ = 0;
    wm_.services().sound.play_sfx(effect_rows_.at(static_cast<std::size_t>(g)).sfx1);
  }
  fade_ += tick_ & 1;
  return tick_ == 0x24;
}

bool BatModeWnd::fx_grav_catapult() {
  const game::Ship& ship = fx_ship();
  const int g = fx_gizmo();
  if (tick_ == 0) {
    fx_count_ = 1;
    fx_items_[0] = find_item(ItemKind::kShip, control()->selection().object);
    Item& item = items_[static_cast<std::size_t>(fx_items_[0])];
    item.flags |= kFlagNoLine | kFlagFxPosition;
    item.fx_position = 0;
    fx_pos_[0] = ship.position;
    fx_pos_[1] = {-ship.position[0], -ship.position[1], -ship.position[2]};
    phase_ = 0;
    wm_.services().sound.play_sfx(effect_rows_.at(static_cast<std::size_t>(g)).sfx1);
  }
  const float to = f32(game::length(fx_pos_[1]));
  const float at = f32(game::length(fx_pos_[0]));
  float step = 0.0F;
  if (1.0F < to) {
    step = f32((d(to) - at) * d(96.0F) / to);
  }
  if (step < 3.2) {
    step = 3.2F;
  }
  return step_toward(fx_pos_[0], fx_pos_[1], step);
}

bool BatModeWnd::fx_lane_magnetron() {
  const game::Session& s = *core();
  const game::Ship& ship = fx_ship();
  if (ship.target_kind != game::kTargetLane) {
    throw std::logic_error("star system: a lane magnetron on no lane");
  }
  const int g = fx_gizmo();
  if (tick_ == 0) {
    fx_count_ = 1;
    fx_items_[0] = find_item(ItemKind::kShip, control()->selection().object);
    Item& item = items_[static_cast<std::size_t>(fx_items_[0])];
    item.flags |= kFlagNoLine | kFlagFxPosition;
    item.fx_position = 0;
    fx_pos_[0] = ship.position;
    if (const auto lane = game::lane_at(s, ship.target)) {
      const game::Lane& l = s.game->cosmos.lanes[static_cast<std::size_t>(*lane)];
      fx_pos_[1] = ship.location == l.star_a ? l.end_a : l.end_b;
    }
    phase_ = 0;
    wm_.services().sound.play_sfx(effect_rows_.at(static_cast<std::size_t>(g)).sfx1);
  }
  return step_toward(fx_pos_[0], fx_pos_[1], f32(static_cast<double>(tick_) * 3.2));
}

bool BatModeWnd::fx_transfer() {
  const game::Session& s = *core();
  const game::Ship& ship = fx_ship();
  if (ship.target_kind != game::kTargetShip) {
    throw std::logic_error("star system: a transfer to no ship");
  }
  const auto target = game::ship_at(s, ship.target);
  const int g = fx_gizmo();
  const EffectRow& row = effect_rows_.at(static_cast<std::size_t>(g));
  if (tick_ == 0) {
    fx_count_ = 3;
    fx_items_[0] = find_item(ItemKind::kShip, control()->selection().object);
    items_[static_cast<std::size_t>(fx_items_[0])].flags |= kFlagCustomDraw;
    fx_items_[1] = kSpriteItem;
    Item& sprite = effect_sprite();
    sprite.kind = ItemKind::kEffect;
    sprite.flags = kFlagNoLine | kFlagHidden | kFlagFxPosition;
    sprite.fx_position = 1;
    fx_pos_[1] = ship.position;
    set_item_file(sprite, row.shape1, 0);
    fx_items_[2] = target ? find_item(ItemKind::kShip, *target) : -1;
    phase_ = 0;
    fade_ = 0;
    wm_.services().sound.play_sfx(row.sfx1);
  }
  if (phase_ == 0) {
    fade_ += tick_ & 1;
    if (fade_ == 0x12) {
      items_[static_cast<std::size_t>(fx_items_[0])].flags &= static_cast<std::uint8_t>(~kFlagCustomDraw);
      effect_sprite().flags &= static_cast<std::uint8_t>(~kFlagHidden);
      phase_ = 1;
      fade_ = 0;
    }
    return false;
  }
  if (phase_ == 1) {
    if (step_toward(fx_pos_[1], s.game->cosmos.ships[static_cast<std::size_t>(*target)].position, 20.0F)) {
      effect_sprite().flags |= kFlagHidden;
      items_[static_cast<std::size_t>(fx_items_[2])].flags |= kFlagCustomDraw;
      phase_ = 2;
      return false;
    }
    next_frame(effect_sprite());
    return false;
  }
  fade_ += tick_ & 1;
  return fade_ == 0x12;
}

bool BatModeWnd::fx_positron_bouncer() {
  game::Session& s = *core();
  const game::Ship& ship = fx_ship();
  if (ship.target_kind != game::kTargetShip) {
    throw std::logic_error("star system: a positron bouncer on no ship");
  }
  const auto target = game::ship_at(s, ship.target);
  const int g = fx_gizmo();
  if (tick_ == 0) {
    const EffectRow& row = effect_rows_.at(static_cast<std::size_t>(g));
    fx_count_ = 2;
    fx_items_[0] = kSpriteItem;
    Item& sprite = effect_sprite();
    sprite.kind = ItemKind::kEffect;
    sprite.flags |= kFlagNoLine | kFlagFxPosition;
    sprite.fx_position = 0;
    fx_pos_[0] = ship.position;
    set_item_file(sprite, row.shape1, 0);
    fx_items_[1] = target ? find_item(ItemKind::kShip, *target) : -1;
    Item& item = items_[static_cast<std::size_t>(fx_items_[1])];
    item.flags |= kFlagNoLine | kFlagFxPosition;
    item.fx_position = 1;
    fx_pos_[1] = s.game->cosmos.ships[static_cast<std::size_t>(*target)].position;
    Vec3 push = game::sub(fx_pos_[1], fx_pos_[0]);
    game::set_length(push, static_cast<float>(s.tables->gizmos.at(static_cast<std::size_t>(g)).special1 << 5));
    fx_pos_[2] = {f32(d(fx_pos_[1][0]) + push[0]), f32(d(fx_pos_[1][1]) + push[1]), f32(d(fx_pos_[1][2]) + push[2])};
    phase_ = 0;
    wm_.services().sound.play_sfx(row.sfx1);
  }
  if (phase_ == 0) {
    next_frame(effect_sprite());
    if (step_toward(fx_pos_[0], fx_pos_[1], 50.0F)) {
      effect_sprite().flags |= kFlagHidden;
      phase_ = 1;
    }
    return false;
  }
  const double far = game::length(game::sub(fx_pos_[2], fx_pos_[1])) * d(0.05F);
  float step = f32(far);
  if (far >= 3.2) {
    if (step > 50.0F) {
      step = 50.0F;
    }
  } else {
    step = 3.2F;
  }
  return step_toward(fx_pos_[1], fx_pos_[2], step);
}

bool BatModeWnd::fx_disperser() {
  game::Session& s = *core();
  const game::Cosmos& c = s.game->cosmos;
  const game::Ship& ship = fx_ship();
  const int g = fx_gizmo();
  const bool fleet = g == kGizmoFleetDisperser;
  int centre_ship = control()->selection().object;
  if (fleet) {
    if (ship.target_kind != game::kTargetShip) {
      throw std::logic_error("star system: a fleet disperser on no ship");
    }
    centre_ship = game::ship_at(s, ship.target).value_or(-1);
  }
  const game::Ship& centre = c.ships[static_cast<std::size_t>(centre_ship)];
  if (tick_ == 0) {
    fx_count_ = 2;
    fx_items_[0] = find_item(ItemKind::kShip, centre_ship);
    fx_items_[1] = kSpriteItem;
    Item& sprite = effect_sprite();
    sprite.kind = ItemKind::kFree;
    if (fleet) {
      sprite.kind = ItemKind::kEffect;
      sprite.flags = kFlagNoLine | kFlagFxPosition;
      sprite.fx_position = 1;
      fx_pos_[1] = ship.position;
      set_item_file(sprite, effect_rows_.at(kGizmoFleetDisperser).shape1, 0);
    }
    fx_add_ships(ship.location, centre_ship);
    phase_ = fleet ? 0 : 1;
    fade_ = 0;
    wm_.services().sound.play_sfx(effect_rows_.at(static_cast<std::size_t>(g)).sfx1);
    disperse_ = 0.0F;
  }
  switch (phase_) {
    case 0:
      next_frame(effect_sprite());
      if (step_toward(fx_pos_[1], centre.position, 10.0F)) {
        effect_sprite().flags |= kFlagHidden;
        phase_ = 1;
      }
      return false;
    case 1:
      fade_ += tick_ & 1;
      items_[static_cast<std::size_t>(fx_items_[0])].flags |= kFlagCustomDraw;
      if (fade_ == 9) {
        phase_ = 2;
      }
      return false;
    case 2: {
      const Vec3 at = centre.position;
      const auto reach = static_cast<float>(s.tables->gizmos.at(static_cast<std::size_t>(g)).special1);
      disperse_ = f32(d(disperse_) + d(0.2F));
      bool moved = false;
      for (int i = 2; i < fx_count_; ++i) {
        Vec3& p = fx_pos_[items_[static_cast<std::size_t>(fx_items_[static_cast<std::size_t>(i)])].fx_position];
        Vec3 dv = game::sub(p, at);
        const double further = game::length(dv) * d(0.03125F) + d(0.2F);
        float length = f32(further);
        if (further < reach) {
          moved = true;
        } else {
          length = reach;
        }
        game::set_length(dv, f32(d(length) * d(32.0F)));
        p = {f32(d(at[0]) + dv[0]), f32(d(at[1]) + dv[1]), f32(d(at[2]) + dv[2])};
      }
      if (disperse_ >= reach || !moved) {
        phase_ = 3;
      }
      return false;
    }
    default:
      fade_ += tick_ & 1;
      return fade_ == 0x12;
  }
}

bool BatModeWnd::fx_recaller() {
  const int g = fx_gizmo();
  if (tick_ == 0) {
    fx_count_ = 1;
    fx_items_[0] = find_item(ItemKind::kShip, control()->selection().object);
    items_[static_cast<std::size_t>(fx_items_[0])].flags |= kFlagCustomDraw;
    phase_ = 0;
    fade_ = 0;
    wm_.services().sound.play_sfx(effect_rows_.at(static_cast<std::size_t>(g)).sfx1);
  }
  if (phase_ == 0) {
    fade_ += (tick_ & 3) == 0 ? 1 : 0;
    if (fade_ == 9) {
      phase_ = 1;
    }
    return false;
  }
  Item& item = items_[static_cast<std::size_t>(fx_items_[0])];
  item.scale = f32(d(item.scale) + d(-0.04F));
  return d(item.scale) < 0.04;
}

bool BatModeWnd::fx_self_destruct() {
  const game::Ship& ship = fx_ship();
  if (tick_ == 0) {
    fx_count_ = 2;
    fx_items_[0] = find_item(ItemKind::kShip, control()->selection().object);
    items_[static_cast<std::size_t>(fx_items_[0])].flags |= kFlagCustomDraw;
    fx_items_[1] = kSpriteItem;
    effect_sprite().kind = ItemKind::kFree;
    phase_ = 0;
    fade_ = 0;
    wm_.services().sound.play_sfx(effect_rows_.at(kGizmoSelfDestruct).sfx1);
  }
  Item& sprite = effect_sprite();
  if (phase_ == 0) {
    fade_ += tick_ & 1;
    if (fade_ == 9) {
      items_[static_cast<std::size_t>(fx_items_[0])].flags |= kFlagHidden;
      sprite.kind = ItemKind::kEffect;
      sprite.flags |= kFlagFxPosition;
      sprite.fx_position = 1;
      fx_pos_[1] = ship.position;
      set_item_file(sprite, effect_rows_.at(kGizmoSelfDestruct).shape1, 0);
      sprite.scale = 5.0F;
      phase_ = 1;
    }
    return false;
  }
  bool done = false;
  if (phase_ == 1) {
    if ((tick_ & 3) == 0 && next_frame(sprite)) {
      done = true;
    }
    sprite.scale = 5.0F;
  }
  return done;
}

bool BatModeWnd::fx_smart_bomb() {
  game::Session& s = *core();
  game::Cosmos& c = s.game->cosmos;
  const game::Ship& ship = fx_ship();
  const EffectRow& row = effect_rows_.at(kGizmoSmartBomb);
  // The bomb flies as the display item 0 (the move cursor's).
  Item& bomb = items_[0];
  if (tick_ == 0) {
    fx_count_ = 1;
    fx_items_[0] = 0;
    bomb.kind = ItemKind::kFree;
    bomb.flags |= kFlagNoLine | kFlagFxPosition;
    bomb.fx_position = 0;
    fx_pos_[0] = ship.position;
    phase_ = 0;
    wm_.services().sound.play_sfx(row.sfx1);
  }
  if (bomb.kind == ItemKind::kFree) {
    // The next ship at war with the bomb's owner, from the one after the
    // last hit (the phase's low byte).
    const std::vector<game::ShipIndex> ships =
        game::cosmos_ships_at_star(c, c.stars[static_cast<std::size_t>(ship.location)]);
    const game::Race& owner = c.races[static_cast<std::size_t>(ship.owner)];
    for (int i = phase_ & 0xff; i < static_cast<int>(ships.size()); ++i) {
      const game::Ship& other = c.ships[static_cast<std::size_t>(ships[static_cast<std::size_t>(i)])];
      if (other.location_kind == game::ShipLocation::kSystem && other.owner != ship.owner &&
          owner.relation[static_cast<std::size_t>(other.owner)] == game::kWar) {
        fx_count_ = 2;
        fx_pos_[1] = other.position;
        bomb.kind = ItemKind::kEffect;
        fx_items_[1] = find_item(ItemKind::kShip, ships[static_cast<std::size_t>(i)]);
        set_item_file(bomb, row.shape1, 0);
        phase_ = i + 1;
        break;
      }
    }
    if (bomb.kind == ItemKind::kFree) {
      return true;
    }
  }
  if ((phase_ & 0x100) == 0) {
    next_frame(bomb);
    if (step_toward(fx_pos_[0], fx_pos_[1], 50.0F)) {
      Item& hit = items_[static_cast<std::size_t>(fx_items_[1])];
      const std::uint8_t result = game::ship_take_damage(s, c.ships[static_cast<std::size_t>(hit.object)], false,
                                                         s.tables->gizmos.at(kGizmoSmartBomb).special1,
                                                         static_cast<game::RaceIndex>(ship.owner));
      int shape = kFileShield;
      int sfx = kSfxShield;
      if ((result & game::kActionDestroyed) != 0) {
        shape = kFileDestroy;
        sfx = kSfxExplosion;
        hit.flags |= kFlagHidden;
      } else if ((result & game::kActionPenetrated) != 0) {
        shape = row.shape2;
        sfx = row.sfx2;
      }
      set_item_file(bomb, shape, 0);
      wm_.services().sound.play_sfx(sfx);
      phase_ |= 0x100;
    }
  } else if ((tick_ & 3) == 0 && next_frame(bomb)) {
    bomb.kind = ItemKind::kFree;
    phase_ &= 0xff;
  }
  return false;
}

void BatModeWnd::fx_draw_item(const Item& shown) {
  Item& item = items_[static_cast<std::size_t>(&shown - items_.data())];
  Image8& screen = wm_.screen();
  switch (effect_) {
    case kFxHyperswap: {
      // BatModeWnd_fx_draw_hyperswap 0x13a28: shrinking to the swap, then
      // growing back.
      if (item.kind != ItemKind::kShip || item.shapes == nullptr) {
        throw std::logic_error("star system: a hyperswap draw of no ship");
      }
      const float size = f32((tick_ < 0x14 ? 20.0 - tick_ : tick_ + -20.0) * 0.05);
      const std::int32_t scale = game::chop(static_cast<double>(item.draw_scale) * size);
      vfx_shape_transform_draw(screen, pane_, item.shapes->at(static_cast<std::size_t>(item.frame)), item.x, item.y,
                               scale, scale);
      return;
    }
    case kFxLaneBlocker: {
      // BatModeWnd_fx_draw_lane_blocker 0x13ad8: the lane end, then a field
      // growing to tick 10 and shrinking with the blocker's shape.
      int level = phase_;
      if (level > 10) {
        level = 20 - level;
        set_item_file(item, effect_rows_.at(kGizmoLaneBlocker).shape2, 2);
      }
      if (item.shapes != nullptr) {
        vfx_shape_transform_draw(screen, pane_, item.shapes->at(static_cast<std::size_t>(item.frame)), item.x, item.y,
                                 item.draw_scale, item.draw_scale);
      }
      const std::int32_t width = level * (item.bx1 - item.bx0);
      const int r = game::chop(static_cast<double>(static_cast<std::uint32_t>(item.draw_scale)) * (1.0 / 65536.0) *
                               static_cast<double>(width) * d(0.05F));
      vfx_ellipse_fill(screen, pane_, item.x, item.y, r, r, 0x96);
      return;
    }
    case kFxRemoteRepair:
    case kFxTransfer:
    case kFxDisperser:
    case kFxRecaller:
    case kFxSelfDestruct:
      break;
    default:
      throw std::logic_error("star system: a custom draw without its effect");
  }
  // BatModeWnd_fx_draw_translucent 0x15630: WHITE00..09 by the fade level.
  int level = fade_;
  if (level > 9) {
    level = 18 - level;
  }
  level = std::clamp(level, 0, 9);
  if (item.shapes != nullptr) {
    vfx_shape_transform_draw(screen, pane_, item.shapes->at(static_cast<std::size_t>(item.frame)), item.x, item.y,
                             item.draw_scale, item.draw_scale,
                             &wm_.services().gsystem.haze_tables().at(static_cast<std::size_t>(11 + level)));
  }
}

}  // namespace oa
