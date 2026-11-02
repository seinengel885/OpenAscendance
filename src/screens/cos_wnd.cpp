#include "screens/cos_wnd.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>

#include "core/error.h"
#include "core/watcom_qsort.h"
#include "game/session.h"
#include "game/state.h"
#include "screens/neg_wnd.h"
#include "screens/new_game_wnd.h"

namespace oa {

namespace {

using game::f32;

// The screens the map is on.
constexpr int kScreenStarMap = 1;
constexpr int kScreenNewGame = 3;
constexpr int kScreenSpecies = 7;
constexpr int kScreenDiplomacy = 13;
constexpr int kScreenStarSystem = 16;

// The rectangles of the map: screen 1 (and 7), the new-game screen's, and
// the right and bottom edges of the diplomacy mini map; its children move
// left under it there.
constexpr Pane kStarMapRect{7, 7, 465, 472};
constexpr Pane kNewGameRect{17, 12, 324, 320};
constexpr int kDiplomacyX1 = 323;
constexpr int kDiplomacyY1 = 315;
constexpr int kDiplomacyShift = 467;

// The depth the shades start from (0x81062: -1500).
constexpr float kDepthOffset = -1500.0F;
// The degree factor (0x859ac).
constexpr float kDegToRad = 0.017453292F;
// The rotation step (degrees per pass) and the zoom step (degrees of field
// of view), times the rotation speed.
constexpr float kRotateStep = 2.0F;
// The species circle's radius (0x80f49).
constexpr double kCircleRadius = 170.0;

// The hover radii (squared distances) and the ship marker's offset.
constexpr int kStarHoverRadius = 40;
constexpr int kShipHoverRadius = 20;
// The remake's enlarged star radius (squared): the original's (sqrt 40)
// grown by the enlarged targets' margin, (sqrt 40 + 5)^2 = 128.25.
constexpr int kEnlargedStarRadius = 128;
static_assert(WinMgr::kTargetMargin == 5, "kEnlargedStarRadius assumes a margin of 5");
// The ship markers' offsets from a star, n-th race shown (0x86478 /
// 0x86480).
constexpr std::array<int, 7> kShipDx = {5, 5, 5, 14, 14, 14, 21};
constexpr std::array<int, 7> kShipDy = {-2, 5, -9, -2, -9, 5, -9};

// The selected star's ring colour (white).
constexpr std::uint8_t kSelectedColor = 0xf3;
// The lane colours: the normal ramp, the red links', the unknown lanes'
// in empire mode.
constexpr int kLaneColor = 0x68;
constexpr int kRedLinkColor = 0x70;
constexpr int kForeignLaneColor = 0x30;

// `gwshare.txt` tables.
constexpr int kLargePortraits = 7;
constexpr int kRaceFlags = 32;
constexpr int kRaceRings = 33;
// The haze table greying an extinct species (BW.HAZ).
constexpr std::size_t kGreyHaze = 22;
// The first star type name (static strings 0xb2..0xbe, `Cosmos_init`).
constexpr int kStringStarTypes = 0xb2;
// The click sound.
constexpr int kClickSound = 0;
// The label's position from the map's bottom-right corner.
constexpr int kLabelDx = 130;
constexpr int kLabelNameDy = 27;
constexpr int kLabelTypeDy = 12;
// The large portrait and flag of the species dealt with (screen 13).
constexpr int kPortraitX = 0x150;
constexpr int kPortraitY = 7;
// The species' themes (tunes 8..28).
constexpr int kSpeciesTunes = 8;

// The toggle `(v == -1) - 1`: -1 becomes 0, anything else -1.
std::int32_t flip(std::int32_t v) { return v == -1 ? 0 : -1; }

// `a x b`, each component stored as a float.
Vec3 cross(const float* a, const float* b) {
  return {f32(static_cast<double>(a[1]) * b[2] - static_cast<double>(a[2]) * b[1]),
          f32(static_cast<double>(a[2]) * b[0] - static_cast<double>(a[0]) * b[2]),
          f32(static_cast<double>(a[0]) * b[1] - static_cast<double>(a[1]) * b[0])};
}

// `orient_rotate_y` (0x4e9f4) / `orient_rotate_x` (0x4e9c4): rows 2 then 1
// of the basis turned, then `orient_orthonormalize_keep_forward` (0x4ec74):
// row 2 normalised, row 0 = row 1 x row 2, row 1 = row 2 x row 0, each
// normalised.
template <typename Rotate>
void orient_rotate(std::array<float, 9>& m, float degrees, Rotate rotate) {
  if (degrees == 0.0F) {
    return;
  }
  for (const std::size_t r : {2U, 1U}) {
    Vec3 v = {m[3 * r], m[3 * r + 1], m[3 * r + 2]};
    rotate(v, degrees);
    std::copy(v.begin(), v.end(), m.begin() + static_cast<std::ptrdiff_t>(3 * r));
  }
  const auto normalize_row = [&](std::size_t r, Vec3 v) {
    game::normalize(v);
    std::copy(v.begin(), v.end(), m.begin() + static_cast<std::ptrdiff_t>(3 * r));
  };
  normalize_row(2, {m[6], m[7], m[8]});
  normalize_row(0, cross(&m[3], &m[6]));
  normalize_row(1, cross(&m[6], &m[0]));
}

// The four depth shades of the stars and ships: farthest 0 .. nearest 3.
int shade4(std::int32_t d) {
  if (d > 220) {
    return 0;
  }
  if (d > 0) {
    return 1;
  }
  return d > -220 ? 2 : 3;
}

// The eight depth shades of the lanes.
int shade8(std::int32_t d) {
  constexpr std::array<int, 7> kLimits = {220, 147, 73, 0, -73, -147, -220};
  int s = 0;
  while (s < 7 && d <= kLimits[static_cast<std::size_t>(s)]) {
    ++s;
  }
  return s;
}

// The squared distance from (x, y) to an item's hover point, and the
// radius it is hit within (`CosWnd_update_hover`).
struct HoverDistance {
  int squared;
  int radius;
};
HoverDistance hover_distance(const CosWnd::Item& it, int x, int y) {
  int dx = it.x - x;
  int dy = it.y - y;
  int radius = kStarHoverRadius;
  if (it.kind == CosWnd::ItemKind::kShip) {
    dx += kShipDx[0];
    dy += kShipDy[0];
    radius = kShipHoverRadius;
  }
  return {dx * dx + dy * dy, radius};
}

// Whether a ship in transit goes from its lane's star B (the direction
// float at ship `+0xa2` is +-0), else from star A.
bool from_star_b(const game::Ship& ship) { return (std::bit_cast<std::uint32_t>(ship.position[1]) & 0x7fffffffU) == 0; }

// The depth of an item relative to the galaxy's centre (`chop(z - 1500)`).
std::int32_t relative_depth(float depth) {
  return game::chop(static_cast<double>(depth) + static_cast<double>(kDepthOffset));
}

void put_i32(std::span<std::uint8_t> out, std::size_t at, std::uint32_t v) {
  for (std::size_t k = 0; k < 4; ++k) {
    out[at + k] = static_cast<std::uint8_t>(v >> (8 * k));
  }
}

std::uint32_t get_i32(std::span<const std::uint8_t> in, std::size_t at) {
  std::uint32_t v = 0;
  for (std::size_t k = 0; k < 4; ++k) {
    v |= static_cast<std::uint32_t>(in[at + k]) << (8 * k);
  }
  return v;
}

// The record extra `key` as an integer, `fallback` when absent.
int extra_int(const WindowRecord& record, const std::string& key, int fallback) {
  const auto it = record.extras.find(key);
  return it == record.extras.end() ? fallback : std::stoi(it->second);
}

// The record extra `key` (a file name).
std::string extra_file(const WindowRecord& record, const std::string& key) {
  const auto it = record.extras.find(key);
  if (it == record.extras.end()) {
    throw DataError(std::format("windows.txt: {} lacks {}", record.name, key));
  }
  return it->second;
}

// The serialized layout (`CosWnd_serialize`).
constexpr std::size_t kStateFilter = 0x1c;
constexpr std::size_t kStateKnown = 0x1d;
constexpr std::size_t kStateCamera = 0x1e;
constexpr std::size_t kStateFov = 0xb6;
constexpr std::size_t kStateSize = 0xbe;

}  // namespace

CosWnd::CosWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes)
    : GWindow(wm, record, shapes),
      star_shapes_(&wm.shapes().get(extra_file(record, "STAR_SHAPES"))),
      ship_shapes_(&wm.shapes().get(extra_file(record, "SHIP_SHAPES"))),
      background_(static_cast<std::uint8_t>(extra_int(record, "BACKGRNDCOLOR", 0))),
      cursor_shape_(extra_int(record, "CURSORSHAPE", 2)) {
  // CosWnd_load_shapes 0x21d64.
  set_rect(pane_.x0, pane_.y0, pane_.x1, pane_.y1);
  reset_camera();
}

void CosWnd::set_rect(int x0, int y0, int x1, int y1) {
  pane_ = Pane{x0, y0, x1, y1};
  cx_ = static_cast<std::int16_t>(static_cast<std::int16_t>(x1 - x0) >> 1);
  cy_ = static_cast<std::int16_t>(static_cast<std::int16_t>(y1 - y0) >> 1);
}

void CosWnd::set_projection() { camera_.set_projection(fov_, kNear, kFar, cx_); }

void CosWnd::reset_camera() {
  fov_ = kFov;
  set_projection();
  camera_.position = {0.0F, 0.0F, -kDistance};
  camera_.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
}

void CosWnd::build_items() {
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    item_count_ = 0;
    return;
  }
  const game::Session& s = *core;
  const game::Cosmos& c = s.game->cosmos;
  item_count_ = 0;
  const auto add = [&](ItemKind kind, std::int16_t index) {
    if (item_count_ >= kMaxItems) {
      throw std::logic_error("star map: more than 355 display items");
    }
    items_[item_count_].kind = kind;
    items_[item_count_].index = index;
    ++item_count_;
  };
  for (std::int16_t i = 0; i < c.star_count; ++i) {
    add(ItemKind::kStar, i);
    if (c.stars[static_cast<std::size_t>(i)].ships_mask == 0) {
      continue;
    }
    // Cosmos_ships_at_star 0x1d794: the first ship in the system or at one
    // of its planets.
    std::int16_t used = 0;
    for (std::size_t k = 0; k < game::kMaxShips && used < c.ship_count; ++k) {
      const game::Ship& ship = c.ships[k];
      if (ship.owner == -1) {
        continue;
      }
      ++used;
      const auto kind = ship.location_kind;
      const bool at_planet = kind == game::ShipLocation::kBuilding || kind == game::ShipLocation::kRefit ||
                             kind == game::ShipLocation::kOrbit;
      if ((at_planet && c.planets[static_cast<std::size_t>(ship.location)].star == i) ||
          (kind == game::ShipLocation::kSystem && ship.location == i)) {
        add(ItemKind::kShip, static_cast<std::int16_t>(k));
        break;
      }
    }
  }
  for (std::int16_t i = 0; i < c.lane_count; ++i) {
    if ((c.lanes[static_cast<std::size_t>(i)].known_mask & s.player_mask) != 0) {
      add(ItemKind::kLane, i);
    }
  }
  std::int16_t used = 0;
  for (std::size_t k = 0; k < game::kMaxShips && used < c.ship_count; ++k) {
    if (item_count_ >= kMaxItems) {
      throw std::logic_error("star map: more than 355 display items");
    }
    const game::Ship& ship = c.ships[k];
    if (ship.owner != -1) {
      ++used;
      if (ship.location_kind == game::ShipLocation::kLane) {
        add(ItemKind::kShip, static_cast<std::int16_t>(k));
      }
    }
  }
}

void CosWnd::project_items() {
  drawn_count_ = 0;
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return;
  }
  const game::Session& s = *core;
  const game::Cosmos& c = s.game->cosmos;
  camera_.update_view();
  const auto add = [&](std::size_t k) {
    if (drawn_count_ < kMaxDrawn) {
      drawn_[drawn_count_++] = static_cast<std::int32_t>(k);
    }
  };
  const auto visible = [&](float z) { return camera_.near_plane < z && z < camera_.far_plane; };
  const game::Star* last_star = nullptr;
  for (std::size_t k = 0; k < item_count_; ++k) {
    Item& it = items_[k];
    switch (it.kind) {
      case ItemKind::kStar: {
        last_star = &c.stars[static_cast<std::size_t>(it.index)];
        const Vec3 p = transform_mat34(camera_.view, last_star->position);
        it.clipped = true;
        if (visible(p[2])) {
          const ScreenPoint sp = project_point(p, camera_.focal, cx_, cy_);
          it.x = sp.x;
          it.y = sp.y;
          it.depth = p[2];
          it.clipped = false;
          add(k);
        }
        break;
      }
      case ItemKind::kLane: {
        const game::Lane& lane = c.lanes[static_cast<std::size_t>(it.index)];
        const Vec3 a = transform_mat34(camera_.view, c.stars[static_cast<std::size_t>(lane.star_a)].position);
        const Vec3 b = transform_mat34(camera_.view, c.stars[static_cast<std::size_t>(lane.star_b)].position);
        it.clipped = true;
        if (visible(a[2]) && visible(b[2])) {
          const ScreenPoint pa = project_point(a, camera_.focal, cx_, cy_);
          const ScreenPoint pb = project_point(b, camera_.focal, cx_, cy_);
          it.x = pa.x;
          it.y = pa.y;
          it.x2 = pb.x;
          it.y2 = pb.y;
          it.depth = f32((static_cast<double>(a[2]) + b[2]) * 0.5F);
          it.clipped = false;
          add(k);
        }
        break;
      }
      case ItemKind::kShip: {
        const game::Ship& ship = c.ships[static_cast<std::size_t>(it.index)];
        if (ship.location_kind == game::ShipLocation::kLane) {
          // Along its lane: from star A, or from B when the direction is 0.
          const game::Lane& lane = c.lanes[static_cast<std::size_t>(ship.location)];
          const bool from_b = from_star_b(ship);
          const Vec3& start = c.stars[static_cast<std::size_t>(from_b ? lane.star_b : lane.star_a)].position;
          const Vec3& end = c.stars[static_cast<std::size_t>(from_b ? lane.star_a : lane.star_b)].position;
          const Vec3 d = game::sub(end, start);
          const double inv = 1.0 / game::length(d);
          const float distance = ship.position[0];
          Vec3 pos{};
          for (std::size_t i = 0; i < 3; ++i) {
            const float scaled = f32(static_cast<double>(d[i]) * distance);
            pos[i] = f32(static_cast<double>(start[i]) + f32(static_cast<double>(scaled) * inv));
          }
          const Vec3 p = transform_mat34(camera_.view, pos);
          it.clipped = true;
          if (visible(p[2])) {
            const ScreenPoint sp = project_point(p, camera_.focal, cx_, cy_);
            it.x = static_cast<std::int16_t>(sp.x + 3);
            it.y = sp.y;
            it.clipped = false;
            it.depth = p[2];
          }
        } else if (k > 0 && !items_[k - 1].clipped && last_star != nullptr) {
          // Right of its star (the item before), past its colony rings.
          const Item& star = items_[k - 1];
          int dx = 4;
          for (int r = 0; r < 7; ++r) {
            if (((1 << r) & last_star->owner_mask & race_filter_) != 0) {
              dx += 2;
            }
          }
          if (star.index == c.selected_star) {
            dx += 2;
          }
          it.x = static_cast<std::int16_t>(star.x + dx);
          it.y = star.y;
          it.depth = f32(static_cast<double>(star.depth) + 1.0);
          it.clipped = false;
        }
        // Added even when clipped, with its last position.
        add(k);
        break;
      }
    }
  }
  // CosDrawList_compare_depth 0x21b50: farthest first. The list (CosWnd
  // +0x172) is not 4-aligned: Watcom's qsort moves the pivot to the base
  // (the order of equal depths, e.g. ships at the same point of a lane).
  watcom_qsort(
      std::span(drawn_).first(drawn_count_),
      [this](std::int32_t a, std::int32_t b) {
        const float da = items_[static_cast<std::size_t>(a)].depth;
        const float db = items_[static_cast<std::size_t>(b)].depth;
        if (!(da <= db)) {
          return -1;
        }
        return da < db ? 1 : 0;
      },
      false);
}

void CosWnd::rotate_step(bool y_axis, float degrees) {
  turn(y_axis, f32(static_cast<double>(degrees) * wm_.rotation_speed()));
}

void CosWnd::turn(bool y_axis, float angle) {
  drawn_count_ = 0;
  if (y_axis) {
    orient_rotate(camera_.orientation, angle, game::rotate_y);
  } else {
    orient_rotate(camera_.orientation, angle, rotate_x);
  }
}

void CosWnd::zoom_step(int dir) {
  if ((fov_ < kMinFov && dir < 0) || (fov_ > kMaxFov && dir > 0)) {
    actions_ &= ~static_cast<std::uint32_t>(kActionZoom);
    return;
  }
  fov_ = f32(static_cast<double>(dir) * wm_.rotation_speed() + fov_);
  set_projection();
  drawn_count_ = 0;
}

bool CosWnd::takes_view_gesture(int x, int y, bool /*mouse*/) const {
  return x > pane_.x0 && x < pane_.x1 && y > pane_.y0 && y < pane_.y1;
}

void CosWnd::view_drag(int dx, int dy) {
  if (dx != 0) {
    turn(true, -static_cast<float>(dx) * kDragDegrees);
  }
  if (dy != 0) {
    turn(false, static_cast<float>(dy) * kDragDegrees);
  }
  project_items();
  draw(kDrawNormal);
}

void CosWnd::view_pinch(float ratio) {
  if (!(ratio > 0.0F)) {
    return;
  }
  fov_ = std::clamp(fov_ / ratio, kMinFov, kMaxFov);
  set_projection();
  drawn_count_ = 0;
  project_items();
  draw(kDrawNormal);
}

void CosWnd::view_wheel(int notches) { view_pinch(std::pow(kWheelZoom, static_cast<float>(notches))); }

void CosWnd::draw_label(int background) {
  if (hovered_ < 0 || items_[static_cast<std::size_t>(hovered_)].kind != ItemKind::kStar) {
    return;
  }
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return;
  }
  const game::Star& star =
      core->game->cosmos.stars[static_cast<std::size_t>(items_[static_cast<std::size_t>(hovered_)].index)];
  const std::string name =
      (star.explored_mask & core->player_mask) != 0 ? std::string(game::name_view(star.name)) : std::string();
  Image8& screen = wm_.screen();
  // In the hover the name is drawn only when known; in the draw always.
  if (background == kDefault || !name.empty()) {
    GameFont& large = wm_.large_font();
    large.pane() = GSystem::kScreenPane;
    large.draw_wrapped(screen, pane_.x1 - kLabelDx, pane_.y1 - kLabelNameDy, name, 0, kDefault, background, 0);
  }
  GameFont& small = wm_.small_font();
  small.pane() = GSystem::kScreenPane;
  small.draw_wrapped(screen, pane_.x1 - kLabelDx, pane_.y1 - kLabelTypeDy,
                     wm_.static_string(kStringStarTypes + star.type), 0, kDefault, background, 0);
}

bool CosWnd::remake_picking() const {
  const int screen_id = wm_.current_screen();
  return wm_.enlarged_targets() && (screen_id == kScreenStarMap || screen_id == kScreenDiplomacy);
}

int CosWnd::ship_star_item(std::size_t k) const {
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return -1;
  }
  const game::Cosmos& c = core->game->cosmos;
  const game::Ship& ship = c.ships[static_cast<std::size_t>(items_[k].index)];
  int star = -1;
  if (ship.location_kind != game::ShipLocation::kLane) {
    // A star's marker: right after that star's item (`build_items`).
    star = k > 0 && items_[k - 1].kind == ItemKind::kStar ? static_cast<int>(k) - 1 : -1;
  } else if (ship.position[0] == 0.0F) {
    // Entered its lane, not moved yet (`ship_enter_lane`).
    const game::Lane& lane = c.lanes[static_cast<std::size_t>(ship.location)];
    const std::int16_t from = from_star_b(ship) ? lane.star_b : lane.star_a;
    for (std::size_t i = 0; i < item_count_ && star == -1; ++i) {
      if (items_[i].kind == ItemKind::kStar && items_[i].index == from) {
        star = static_cast<int>(i);
      }
    }
  }
  return star != -1 && !items_[static_cast<std::size_t>(star)].clipped ? star : -1;
}

int CosWnd::pick(int x, int y) const {
  for (int k = static_cast<int>(item_count_) - 1; k >= 0; --k) {
    const Item& it = items_[static_cast<std::size_t>(k)];
    if (it.kind == ItemKind::kLane) {
      continue;
    }
    if (const HoverDistance d = hover_distance(it, x, y); d.squared < d.radius) {
      if (it.kind == ItemKind::kShip && remake_picking()) {
        const int star = ship_star_item(static_cast<std::size_t>(k));
        return star != -1 ? star : k;
      }
      return k;
    }
  }
  if (!remake_picking()) {
    return -1;
  }
  int found = -1;
  for (std::size_t k = 0; k < item_count_; ++k) {
    const Item& it = items_[k];
    if (it.kind == ItemKind::kStar && !it.clipped && hover_distance(it, x, y).squared <= kEnlargedStarRadius) {
      if (found != -1) {
        return -1;
      }
      found = static_cast<int>(k);
    }
  }
  return found;
}

void CosWnd::update_hover(int x, int y) {
  if (species_mode_ == -1) {
    return;
  }
  if (hovered_ != -1) {
    const HoverDistance d = hover_distance(items_[static_cast<std::size_t>(hovered_)], x, y);
    // The remake's picks are kept while picked.
    if (d.squared <= d.radius || (remake_picking() && pick(x, y) == hovered_)) {
      return;
    }
    hovered_ = -1;
    wm_.set_pointer(cursor_shape_);
    draw(10);
  }
  hovered_ = pick(x, y);
  if (hovered_ != -1) {
    wm_.set_pointer(cursor_shape_ + 1);
    draw_label(0xff);
  }
}

void CosWnd::click_item() {
  const Item& it = items_[static_cast<std::size_t>(hovered_)];
  game::Session* core = wm_.services().game.core();
  if (it.kind != ItemKind::kStar || core == nullptr) {
    return;
  }
  game::Cosmos& c = core->game->cosmos;
  if (wm_.current_screen() == kScreenStarMap) {
    if ((c.stars[static_cast<std::size_t>(it.index)].explored_mask & core->player_mask) != 0) {
      c.selected_star = it.index;
      hovered_ = -1;
      wm_.post_message(kMessageChangeScreen, kScreenStarSystem, 1);
    }
  } else if (wm_.current_screen() == kScreenDiplomacy) {
    // The negotiation window names this system in its options.
    c.selected_star = it.index;
    auto* negotiation = dynamic_cast<NegWnd*>(wm_.find_window("NEGSCREEN"));
    if (negotiation == nullptr) {
      throw DataError("windows.txt: no NEGSCREEN");
    }
    negotiation->on_star_selected();
    draw(kDrawNormal);
  }
}

void CosWnd::draw_items() {
  Image8& screen = wm_.screen();
  vfx_pane_wipe(screen, pane_, background_);
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return;
  }
  const game::Session& s = *core;
  const game::Cosmos& c = s.game->cosmos;
  const auto player_bit = static_cast<std::uint8_t>(1U << s.player_race);
  const auto color_base = [&](std::size_t race) { return static_cast<std::uint8_t>(c.races[race].color * 4 + 0x13); };
  const std::uint8_t player_color = color_base(s.player_race);
  const bool new_game = wm_.current_screen() == kScreenNewGame;
  for (std::size_t n = 0; n < drawn_count_; ++n) {
    const Item& it = items_[static_cast<std::size_t>(drawn_[n])];
    const std::int32_t d = relative_depth(it.depth);
    switch (it.kind) {
      case ItemKind::kStar: {
        const int shade = shade4(d);
        const game::Star& star = c.stars[static_cast<std::size_t>(it.index)];
        vfx_shape_draw(screen, pane_, star_shapes_->at(static_cast<std::size_t>(star.type * 4 + shade)), it.x, it.y);
        const bool explored = (star.explored_mask & player_bit) != 0;
        int radius = 4;
        if ((explored || new_game) && colonies_ != 0 && star.owner_mask != 0) {
          for (std::size_t r = 0; r < game::kMaxRaces; ++r) {
            if (((1U << r) & star.owner_mask & race_filter_) != 0) {
              const auto color = static_cast<std::uint8_t>(color_base(r) - 3 + shade);
              vfx_ellipse_draw(screen, pane_, it.x, it.y, radius, radius, color);
              radius += 2;
            }
          }
        }
        if (it.index == c.selected_star && !new_game && !wm_.selection_ring_hidden()) {
          vfx_ellipse_draw(screen, pane_, it.x, it.y, radius, radius, kSelectedColor);
        }
        if ((explored || new_game) && (star.owner_mask & 0x80) != 0 && flags_ == -1) {
          int home = 0;
          while (home < c.race_count && c.races[static_cast<std::size_t>(home)].home != it.index) {
            ++home;
          }
          if (home >= c.race_count) {
            throw std::logic_error(std::format("star map: no home race at star {}", it.index));
          }
          if (((1 << home) & race_filter_) != 0 || new_game) {
            const game::Race& race = c.races[static_cast<std::size_t>(home)];
            wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(race.species), it.x - 7, it.y + 3, race.color);
          }
        }
        if (explored && colonies_ != 0 && (star.owner_mask & player_bit) == 0) {
          vfx_pixel_write(screen, pane_, it.x - 4, it.y, player_color);
          vfx_pixel_write(screen, pane_, it.x, it.y - 4, player_color);
          vfx_pixel_write(screen, pane_, it.x + 4, it.y, player_color);
          vfx_pixel_write(screen, pane_, it.x, it.y + 4, player_color);
        }
        break;
      }
      case ItemKind::kLane: {
        if (lanes_ == 0 && empire_ == 0) {
          break;
        }
        const int shade = shade8(d);
        const game::Lane& lane = c.lanes[static_cast<std::size_t>(it.index)];
        int color = kLaneColor + shade;
        if (empire_ == -1) {
          color = lane.visited != 0 ? 0x10 + 4 * c.races[s.player_race].color + shade / 2 : kForeignLaneColor + shade;
        }
        if (empire_ == 0 && (lane.flags & 1) != 0) {
          color = kRedLinkColor + shade / 2;
        }
        vfx_line_draw(screen, pane_, it.x, it.y, it.x2, it.y2, static_cast<std::uint8_t>(color));
        break;
      }
      case ItemKind::kShip: {
        if (ships_ == 0) {
          break;
        }
        const Shape& shape = ship_shapes_->at(static_cast<std::size_t>(shade4(d)));
        const game::Ship& ship = c.ships[static_cast<std::size_t>(it.index)];
        if (ship.location_kind == game::ShipLocation::kLane) {
          const game::Lane& lane = c.lanes[static_cast<std::size_t>(ship.location)];
          if ((lane.known_mask & player_bit) != 0 && ((1 << ship.owner) & race_filter_) != 0) {
            wm_.draw_colored(pane_, shape, it.x, it.y, c.races[static_cast<std::size_t>(ship.owner)].color);
          }
          break;
        }
        const std::size_t star_index =
            ship.location_kind == game::ShipLocation::kSystem
                ? static_cast<std::size_t>(ship.location)
                : static_cast<std::size_t>(c.planets[static_cast<std::size_t>(ship.location)].star);
        const game::Star& star = c.stars[star_index];
        if ((star.explored_mask & player_bit) == 0) {
          break;
        }
        std::size_t shown = 0;
        for (int r = 0; r < c.race_count; ++r) {
          if ((star.ships_mask & (1 << r) & race_filter_) != 0) {
            wm_.draw_colored(pane_, shape, it.x + kShipDx.at(shown), it.y + kShipDy.at(shown),
                             c.races[static_cast<std::size_t>(r)].color);
            ++shown;
          }
        }
        break;
      }
    }
  }
}

void CosWnd::draw_race_portraits() {
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return;
  }
  const game::Cosmos& c = core->game->cosmos;
  const game::Race& player = c.races[core->player_race];
  Image8& screen = wm_.screen();
  for (int r = 0; r < c.race_count; ++r) {
    const auto slot = static_cast<std::size_t>(r);
    const ScreenPoint p = circle_[slot];
    if (((1 << r) & race_filter_) == 0) {
      continue;
    }
    if (player.relation[slot] != 0 || r == core->player_race) {
      const Shape& portrait = wm_.shared_shapes(r).at(0);
      if (c.races[slot].extinct == -1) {
        vfx_shape_translate_draw(screen, pane_, portrait, p.x, p.y, wm_.services().gsystem.haze_tables().at(kGreyHaze));
      } else {
        vfx_shape_draw(screen, pane_, portrait, p.x, p.y);
      }
    }
    vfx_shape_draw(screen, pane_, wm_.shared_shapes(kRaceRings).at(c.races[slot].color), p.x, p.y);
  }
}

void CosWnd::draw(int /*mode*/) {
  draw_items();
  const int screen_id = wm_.current_screen();
  if (screen_id == kScreenNewGame) {
    if (GWindow* box = wm_.find_window("NEWGAMEWND")) {
      box->draw(1);
    }
  }
  if (species_mode_ == -1) {
    draw_race_portraits();
  }
  const game::Session* core = wm_.services().game.core();
  if (screen_id != kScreenNewGame && core != nullptr) {
    const game::Race& player = core->game->cosmos.races[core->player_race];
    wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(player.species), 3, 3, player.color);
  }
  draw_label(kDefault);
}

void CosWnd::activate(int a, int b) {
  GameSession& game = wm_.services().game;
  const int screen_id = wm_.current_screen();
  known_races_ = static_cast<std::uint8_t>((1U << game.race_count()) - 1);
  for (int i = 0; i < 7; ++i) {
    const int bit = 1 << i;
    int flags = 0;
    if ((known_races_ & bit) != 0) {
      flags = (race_filter_ & bit) != 0 ? 5 : 1;
    }
    for (GWindow* child : children_) {
      child->handle_event(kEventSetControl, bit, flags);
    }
  }
  wm_.set_timer(id_, kMessageTick, 0, 0, 0);
  actions_ = 0;
  // The species circle: race i at (cx - 170 sin a, cy - 170 cos a).
  float angle = 0.0F;
  for (int i = 0; i < game.race_count() && i < 7; ++i) {
    const double radians = static_cast<double>(angle) * static_cast<double>(kDegToRad);
    circle_[static_cast<std::size_t>(i)] = {
        static_cast<std::int16_t>(game::chop(cx_ - std::sin(radians) * kCircleRadius)),
        static_cast<std::int16_t>(game::chop(cy_ - std::cos(radians) * kCircleRadius))};
    angle = f32(static_cast<double>(360 / game.race_count()) + angle);
  }
  if (screen_id == kScreenNewGame) {
    set_rect(kNewGameRect.x0, kNewGameRect.y0, kNewGameRect.x1, kNewGameRect.y1);
    fov_ = kFov;
    set_projection();
    actions_ = kActionRotateY;
    lanes_ = 0;
    ships_ = 0;
    colonies_ = 0;
    flags_ = 0;
    empire_ = 0;
    race_filter_ = 0xff;
  }
  species_mode_ = screen_id == kScreenSpecies ? -1 : 0;
  if (screen_id == kScreenDiplomacy) {
    set_rect(pane_.x0, pane_.y0, kDiplomacyX1, kDiplomacyY1);
    fov_ = kFov;
    set_projection();
    for (GWindow* child : children_) {
      child->move(-kDiplomacyShift, 0);
    }
    const int target = wm_.target_race();
    vfx_shape_draw(wm_.screen(), GSystem::kScreenPane, wm_.shared_shapes(kLargePortraits + target).at(0), kPortraitX,
                   kPortraitY);
    const int species = game.race_species(target);
    wm_.draw_colored(GSystem::kScreenPane, wm_.shared_shapes(kRaceFlags).at(static_cast<std::size_t>(species)),
                     kPortraitX + 1, kPortraitY + 1, game.race_color(target));
    wm_.services().sound.play_tune(species + kSpeciesTunes, false);
  }
  build_items();
  drawn_count_ = 0;
  project_items();
  active_ = true;
  shown_ = true;
  draw(kDrawNormal);
  if (screen_id != kScreenNewGame) {
    for (GWindow* child : children_) {
      if (child->name() != "SHIPDESIGN" || screen_id == kScreenStarMap) {
        wm_.send_event(child->id(), kEventActivate, a, b);
      }
    }
  }
}

void CosWnd::deactivate(int a, int b) {
  wm_.kill_timer(id_, -1);
  const int screen_id = wm_.current_screen();
  if (screen_id == kScreenStarMap) {
    wm_.set_auto_turn(false);
  }
  if (screen_id == kScreenDiplomacy) {
    for (GWindow* child : children_) {
      child->move(kDiplomacyShift, 0);
      child->handle_event(kEventDeactivate, a, b);
    }
    set_rect(pane_.x0, pane_.y0, kStarMapRect.x1, kStarMapRect.y1);
    fov_ = kFov;
    set_projection();
  } else if (screen_id == kScreenNewGame) {
    set_rect(kStarMapRect.x0, kStarMapRect.y0, kStarMapRect.x1, kStarMapRect.y1);
    fov_ = kFov;
    set_projection();
    lanes_ = -1;
    ships_ = -1;
    colonies_ = -1;
    flags_ = -1;
    race_filter_ = 0xff;
  }
  GWindow::handle_event(kEventDeactivate, a, b);
}

LongPress CosWnd::child_long_press(const GWindow& child, int /*x*/, int /*y*/) const {
  return child.message() >= kMessageRotateLeft && child.message() <= kMessageZoomOut ? LongPress::kLeftHold
                                                                                     : LongPress::kHelp;
}

bool CosWnd::handle_event(int type, int a, int b) {
  const auto strictly_inside = [&](int x, int y) {
    return x > pane_.x0 && x < pane_.x1 && y > pane_.y0 && y < pane_.y1;
  };
  const InputState& input = wm_.input();
  switch (type) {
    case kEventActivate:
      activate(a, b);
      return false;
    case kEventDeactivate:
      deactivate(a, b);
      return false;
    case kEventMouseLeave:
      wm_.set_pointer(kPointerNormal);
      wm_.cancel_notify(id_, kEventMouseMove);
      return true;
    case kEventMouseEnter:
      wm_.set_pointer(cursor_shape_);
      wm_.request_notify(id_, WinMgr::kNotifyMove, kEventMouseMove, 0, 0);
      if ((actions_ & (kActionRotateY | kActionRotateX | kActionZoom)) == 0 && !wm_.auto_turn()) {
        hovered_ = -1;
        update_hover(input.x - pane_.x0, input.y - pane_.y0);
      }
      return true;
    case kEventMouseMove:
      if ((actions_ & (kActionRotateY | kActionRotateX | kActionZoom)) == 0 && !wm_.auto_turn()) {
        update_hover(input.x - pane_.x0, input.y - pane_.y0);
      }
      return false;
    case kEventLeftClick:
    case kEventRightClick: {
      if (!strictly_inside(a, b)) {
        return false;
      }
      wm_.services().sound.play_sfx(kClickSound);
      game::Session* core = wm_.services().game.core();
      if (species_mode_ == -1 && core != nullptr) {
        // CosWnd_handle_event: the window-relative centres against the
        // pointer's screen position.
        const game::Cosmos& c = core->game->cosmos;
        const game::Race& player = c.races[core->player_race];
        for (int r = 0; r < c.race_count; ++r) {
          const auto slot = static_cast<std::size_t>(r);
          const ScreenPoint p = circle_[slot];
          if (((1 << r) & race_filter_) != 0 && player.relation[slot] != 0 && c.races[slot].extinct == 0 &&
              p.x - 50 < a && a < p.x + 50 && p.y - 50 < b && b < p.y + 50) {
            wm_.set_target_race(r);
            wm_.post_message(kMessageChangeScreen, kScreenDiplomacy, 1);
            return true;
          }
        }
      } else if (hovered_ != -1 && !wm_.auto_turn()) {
        click_item();
      }
      return true;
    }
    case kEventHelpClick:
      if (!strictly_inside(a, b)) {
        return false;
      }
      if (wm_.current_screen() == kScreenDiplomacy) {
        wm_.post_message(kMessageHelp, 0, kHelpTopic, "cosdip");
      } else if (wm_.current_screen() == kScreenStarMap && !help_.starts_with('0')) {
        wm_.post_message(kMessageHelp, 0, kHelpTopic, help_);
      }
      return true;
    case kEventKey: {
      // The rotation speed: `-` slower, `=` faster (the developer keys need
      // `nougat.lf`).
      constexpr int kScanMinus = 0x0c;
      constexpr int kScanEquals = 0x0d;
      const float speed = wm_.rotation_speed();
      if (b == kScanMinus && static_cast<double>(speed) > 0.1) {
        wm_.set_rotation_speed(f32(static_cast<double>(speed) * 0.8));
      } else if (b == kScanEquals && speed < 10.0F) {
        wm_.set_rotation_speed(f32(static_cast<double>(speed) * 1.25));
      }
      return false;
    }
    case kEventRefresh: {
      const game::Session* core = wm_.services().game.core();
      if (core != nullptr) {
        const game::Cosmos& c = core->game->cosmos;
        known_races_ = static_cast<std::uint8_t>(1U << core->player_race);
        for (int r = 0; r < c.race_count; ++r) {
          if (c.races[core->player_race].relation[static_cast<std::size_t>(r)] != 0) {
            known_races_ = static_cast<std::uint8_t>(known_races_ | (1U << r));
          }
        }
      }
      if (wm_.current_screen() == kScreenStarMap) {
        if (GWindow* races = wm_.find_window("RACES")) {
          races->draw(kDrawNormal);
        }
      }
      build_items();
      project_items();
      draw(kDrawNormal);
      return false;
    }
    case kMessageKnownRaces:
      known_races_ = static_cast<std::uint8_t>(a);
      return false;
    case kMessageRotateLeft:
    case kMessageRotateRight:
    case kMessageRotateUp:
    case kMessageRotateDown:
    case kMessageZoomIn:
    case kMessageZoomOut: {
      // Each button starts its action, stops it (same direction) or
      // reverses it.
      const int pair = (type - kMessageRotateLeft) / 2;
      const std::uint32_t on = pair == 0 ? kActionRotateY : pair == 1 ? kActionRotateX : kActionZoom;
      const std::uint32_t dir = on << 1;
      const bool second = (type - kMessageRotateLeft) % 2 == 1;
      if ((actions_ & on) == 0) {
        actions_ = second ? (actions_ | on | dir) : ((actions_ & ~dir) | on);
      } else if (((actions_ & dir) != 0) == second) {
        actions_ &= ~on;
      } else {
        actions_ = second ? (actions_ | dir) : (actions_ & ~dir);
      }
      if (a == kEventLeftClick) {
        // Held: stopped at the next button change.
        wm_.request_notify(id_, WinMgr::kNotifyLeft, kMessageStopAction, static_cast<int>(on), 0);
      }
      return true;
    }
    case kMessageCenter:
      reset_camera();
      project_items();
      draw(kDrawNormal);
      return true;
    case kMessageLanes:
      lanes_ = flip(lanes_);
      draw(kDrawNormal);
      return true;
    case kMessageShips:
      ships_ = flip(ships_);
      draw(kDrawNormal);
      return true;
    case kMessageEmpire:
      empire_ = flip(empire_);
      draw(kDrawNormal);
      return true;
    case kMessageColonies:
      flags_ = flip(flags_);
      draw(kDrawNormal);
      colonies_ = flip(colonies_);
      draw(kDrawNormal);
      return true;
    case kMessageRaceFilter:
      race_filter_ = static_cast<std::uint8_t>(race_filter_ ^ (a & 0xff));
      draw(kDrawNormal);
      return true;
    case kMessageStopAction:
      actions_ &= ~static_cast<std::uint32_t>(a);
      wm_.cancel_notify(id_, kMessageStopAction);
      return true;
    case kMessageTick:
      if ((actions_ & kActionRotateY) != 0) {
        rotate_step(true, (actions_ & kActionRight) != 0 ? -kRotateStep : kRotateStep);
      }
      if ((actions_ & kActionRotateX) != 0) {
        rotate_step(false, (actions_ & kActionDown) != 0 ? kRotateStep : -kRotateStep);
      }
      if ((actions_ & kActionZoom) != 0) {
        zoom_step((actions_ & kActionOut) != 0 ? 1 : -1);
      }
      if ((actions_ & (kActionRotateY | kActionRotateX | kActionZoom)) != 0) {
        project_items();
        draw(kDrawNormal);
      }
      return false;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

std::uint32_t CosWnd::stale_cluster_bytes() const {
  // `Game_save` 0x4f458 calls `Cosmos_save` 0x20a94 then the windows'
  // serialization at the same stack depth: the record's last 4 bytes
  // (never written by `CosWnd_serialize`) lie over entries 82 (high half)
  // and 83 (low half) of `Cosmos_save`'s cluster buffer, which holds the
  // last cluster's star indices (0 past its count: the clusters are
  // cleared when built).
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr || core->game->cosmos.cluster_count <= 0) {
    return 0;
  }
  const game::Cosmos& cosmos = core->game->cosmos;
  const game::Cluster& last = cosmos.clusters.at(static_cast<std::size_t>(cosmos.cluster_count - 1));
  const auto entry = [&](std::size_t k) -> std::uint32_t {
    return k < static_cast<std::size_t>(last.star_count) ? static_cast<std::uint32_t>(last.stars[k]) : 0;
  };
  return (entry(82) >> 16) | (entry(83) << 16);
}

bool CosWnd::save_state(std::span<std::uint8_t> out) const {
  if (out.size() < kStateSize) {
    return false;
  }
  put_i32(out, 0x00, actions_);
  put_i32(out, 0x04, static_cast<std::uint32_t>(lanes_));
  put_i32(out, 0x08, static_cast<std::uint32_t>(empire_));
  put_i32(out, 0x0c, static_cast<std::uint32_t>(ships_));
  put_i32(out, 0x10, static_cast<std::uint32_t>(colonies_));
  put_i32(out, 0x14, static_cast<std::uint32_t>(flags_));
  put_i32(out, 0x18, static_cast<std::uint32_t>(species_mode_));
  out[kStateFilter] = race_filter_;
  out[kStateKnown] = known_races_;
  const std::array<std::uint8_t, Camera3d::kSize> camera = camera_.serialize();
  std::copy(camera.begin(), camera.end(), out.begin() + kStateCamera);
  put_i32(out, kStateFov, static_cast<std::uint32_t>(game::chop(fov_)));
  put_i32(out, kStateFov + 4, stale_cluster_bytes());
  return true;
}

void CosWnd::load_state(std::span<const std::uint8_t> in) {
  if (in.size() < kStateSize) {
    return;
  }
  actions_ = get_i32(in, 0x00);
  lanes_ = static_cast<std::int32_t>(get_i32(in, 0x04));
  empire_ = static_cast<std::int32_t>(get_i32(in, 0x08));
  ships_ = static_cast<std::int32_t>(get_i32(in, 0x0c));
  colonies_ = static_cast<std::int32_t>(get_i32(in, 0x10));
  flags_ = static_cast<std::int32_t>(get_i32(in, 0x14));
  species_mode_ = static_cast<std::int32_t>(get_i32(in, 0x18));
  race_filter_ = in[kStateFilter];
  known_races_ = in[kStateKnown];
  camera_.deserialize(in.subspan<kStateCamera, Camera3d::kSize>());
  fov_ = static_cast<float>(static_cast<std::int32_t>(get_i32(in, kStateFov)));
}

void RacesButton::draw(int mode) {
  GWindow::draw(mode);
  GameSession& game = wm_.services().game;
  const game::Session* core = game.core();
  int x = 0x2f;
  for (int r = 0; r < game.race_count(); ++r) {
    const bool met =
        core != nullptr && core->game->cosmos.races[core->player_race].relation[static_cast<std::size_t>(r)] != 0;
    if (r == game.player_race() || met) {
      wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(static_cast<std::size_t>(game.race_species(r))), x, 3,
                       game.race_color(r));
      x += 16;
    }
  }
}

bool RaceSelButton::handle_event(int type, int a, int b) {
  // The event setting the button's race slot (`a`).
  constexpr int kEventSetSlot = 0xb;
  GameSession& game = wm_.services().game;
  const game::Session* core = game.core();
  switch (type) {
    case kEventActivate:
      slot_ = name_.size() > 5 ? name_[5] - '1' : 0;
      if (slot_ >= game.race_count()) {
        return false;
      }
      sync_greyed();
      break;
    case kEventDeactivate:
      active_ = false;
      shown_ = false;
      vfx_pane_wipe(wm_.screen(), pane_, 0);
      return false;
    case kEventLeftClick:
    case kEventRightClick: {
      if (core == nullptr) {
        return false;
      }
      const game::Cosmos& c = core->game->cosmos;
      const auto slot = static_cast<std::size_t>(slot_);
      if (c.races[core->player_race].relation[slot] == 0 || slot_ == core->player_race || c.races[slot].extinct == -1) {
        return false;
      }
      wm_.set_target_race(slot_);
      break;
    }
    case kEventMouseEnter:
      draw_hilite(kDrawNormal);
      return true;
    case kEventSetSlot:
      slot_ = a;
      sync_greyed();
      draw(kDrawNormal);
      return true;
    case kEventMouseLeave:
      draw(kDrawNormal);
      return true;
    case kEventRefresh:
      sync_greyed();
      draw(kDrawNormal);
      break;
    default:
      break;
  }
  return GWindow::handle_event(type, a, b);
}

void RaceSelButton::sync_greyed() {
  const game::Session* core = wm_.services().game.core();
  set_greyed(core != nullptr && slot_ != core->player_race &&
             core->game->cosmos.races[static_cast<std::size_t>(slot_)].extinct == -1);
}

void RaceSelButton::draw_hilite(int /*mode*/) {
  if (active_) {
    draw(kHotBackColor);
  }
}

void RaceSelButton::draw(int mode) {
  game::Session* core = wm_.services().game.core();
  if (!active_ || core == nullptr) {
    return;
  }
  // The static strings: "%d of %d Species Known", "Extinct", the species.
  constexpr int kStringKnown = 0x0b;
  constexpr int kStringExtinct = 0x0c;
  constexpr int kStringSpeciesNames = 0x65;
  constexpr int kIntelShapes = 41;
  const game::Cosmos& c = core->game->cosmos;
  const auto slot = static_cast<std::size_t>(slot_);
  const game::Race& race = c.races[slot];
  const std::uint8_t relation = c.races[core->player_race].relation[slot];
  bool wipe = true;
  bool skip = false;
  int background = mode != 0 && !greyed() ? mode : kBackColor;
  int color = 0;
  std::string text;
  if (relation != 0 && race.extinct == 0) {
    color = race.color * 4 + 0x13;
    text = wm_.static_string(kStringSpeciesNames + race.species);
  } else {
    skip = true;
  }
  if (slot_ == core->player_race) {
    int known = 0;
    for (int r = 0; r < c.race_count; ++r) {
      if (r != core->player_race && c.races[core->player_race].relation[static_cast<std::size_t>(r)] != 0) {
        ++known;
      }
    }
    const std::string& format = wm_.static_string(kStringKnown);
    const std::size_t first = format.find("%d");
    const std::size_t second = first == std::string::npos ? std::string::npos : format.find("%d", first + 2);
    text = format;
    if (second != std::string::npos) {
      text = format.substr(0, first) + std::to_string(known) + format.substr(first + 2, second - first - 2) +
             std::to_string(c.race_count - 1) + format.substr(second + 2);
    }
    color = kTextColor;
    background = kBackColor;
    skip = false;
    wipe = false;
  }
  if (race.extinct == -1) {
    text = wm_.static_string(kStringExtinct);
    color = kTextColor;
    skip = false;
  }
  Image8& screen = wm_.screen();
  if (!skip) {
    if (wipe) {
      vfx_pane_wipe(screen, pane_, static_cast<std::uint8_t>(background));
    }
    GameFont& font = wm_.large_font();
    font.pane() = pane_;
    font.draw_wrapped(screen, 0, 0, text, 0x40 | kFontCenterH | kFontCenterV, color, wipe ? background : 0xff, 0);
  }
  if (relation != 0 && slot_ != core->player_race && !skip) {
    wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(race.species), 14, 3, race.color);
    int mark = -1;
    if (race.extinct == 0) {
      mark = relation == game::kAlliance ? 9 : relation == game::kWar ? 8 : -1;
    }
    if (mark != -1) {
      vfx_shape_draw(screen, pane_, wm_.shared_shapes(kIntelShapes).at(static_cast<std::size_t>(mark)), 14, 3);
    }
  }
  if (greyed()) {
    grey_out(kBackColor);
  }
}

void add_star_map_windows(WindowFactory& factory) {
  factory.add(2, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<CosWnd>(wm, record, shapes);
  });
  factory.add(5, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<RacesButton>(wm, record, shapes);
  });
  factory.add(6, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<RaceSelButton>(wm, record, shapes);
  });
}

}  // namespace oa
