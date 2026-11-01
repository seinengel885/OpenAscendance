#include "screens/bat_mode_wnd.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>

#include "assets/text_lines.h"
#include "core/c_format.h"
#include "core/error.h"
#include "core/watcom_qsort.h"
#include "game/cosmos.h"
#include "game/planet.h"
#include "game/player_actions.h"
#include "game/session.h"
#include "game/ship.h"
#include "gfx/vfx_poly.h"
#include "screens/bat_control_wnd.h"
#include "screens/cos_wnd.h"
#include "screens/deselect_button.h"
#include "ui/help_wnd.h"

namespace oa {

namespace {

using game::f32;

double d(float v) { return static_cast<double>(v); }

// The screens.
constexpr int kScreenPlanet = 17;

// `gwshare.txt`: the race slots' ship sprites, the race flags.
constexpr int kShipShapes = 14;
constexpr int kRaceFlags = 32;

// `batfx.txt` shape files of the scene.
constexpr int kFilePlanets = 0;
constexpr int kFileSuns = 1;
constexpr int kFileLanes = 2;
constexpr int kFileNebulae = 3;

// The camera: field of view, clip depths (`BatModeWnd_init_camera`).
constexpr std::int32_t kFov = 35;
constexpr float kNear = 1.0F;
constexpr float kFar = 100000.0F;
// The float constants of the view (`batmode.cpp` data).
constexpr float kDegToRad = 0.017453292F;     // 0x85874
constexpr float kHalf = 0.5F;                 // 0x804c1, 0x804c9, 0x8049e
constexpr float kThird = 0.33333334F;         // 0x804c5, the eye height
constexpr float kTwo = 2.0F;                  // 0x804cd
constexpr float kZoomUnit = 0.01F;            // 0x804e8
constexpr float kScaleMin = 16384.0F;         // 0x804a2
constexpr float kScaleMax = 131072.0F;        // 0x804ae
constexpr float kGridDrop = 0.125F;           // 0x804a6, 0x80656: the grid below the target
constexpr float kRadiusMargin = 32.0F;        // 0x805fa
constexpr float kOneScale = 1.0F / 65536.0F;  // 0x803a5 / 0x803a9
constexpr float kGuardMargin = 2.0F;          // 0x803b9
constexpr float kStarDepthOffset = -440.0F;   // 0x8066d
constexpr float kPickX = 0.00125F;            // 0x8045d, 0x80465
constexpr float kPickZ = 0.002F;              // 0x80461
// The pointer's off-view marker (`+0x1d0e` = 1e6): no move cursor.
constexpr float kNoCursor = 1000000.0F;

// The colours of the view.
constexpr std::uint8_t kLineAbove = 0x90;
constexpr std::uint8_t kLineBelow = 0x72;
constexpr std::uint8_t kWhite = 0xf3;
constexpr std::uint8_t kTargetBad = 0x6b;
constexpr std::uint8_t kGridMajor = 0x61;
constexpr std::uint8_t kGridMinor = 0x68;
constexpr std::uint8_t kQuadOutline = 0x6d;
constexpr std::uint8_t kBeamColor = 0x6a;
constexpr std::uint8_t kBeamCore = 0x6d;

// The static strings of the hover caption.
constexpr int kStringUnknown = 2;
constexpr int kStringSun = 3;
constexpr int kStringPlanet = 4;
constexpr int kStringOwnedBy = 5;
constexpr int kStringToLane = 6;
constexpr int kStringToPlanet = 7;
constexpr int kStringStarLane = 8;
constexpr int kStringRedLink = 9;
constexpr int kStringTo = 10;
constexpr int kStringSpeciesNames = 0x65;

// The click sound.
constexpr int kClickSound = 0;

// The race colour ramp's brightest entry.
std::uint8_t race_color_base(const game::Cosmos& c, int race) {
  return static_cast<std::uint8_t>(c.races[static_cast<std::size_t>(race)].color * 4 + 0x13);
}

void put_bytes(std::span<std::uint8_t> out, std::size_t& at, const void* data, std::size_t n) {
  std::memcpy(out.data() + at, data, n);
  at += n;
}
void get_bytes(std::span<const std::uint8_t> in, std::size_t& at, void* data, std::size_t n) {
  std::memcpy(data, in.data() + at, n);
  at += n;
}

// `sprintf` with one `%s`.
std::string format_s(const std::string& format, std::string_view arg) {
  const std::size_t p = format.find("%s");
  if (p == std::string::npos) {
    return format;
  }
  return format.substr(0, p) + std::string(arg) + format.substr(p + 2);
}

// `draw_rect_frame` (0x2b294) outline mode: the rectangle's four edges in
// `pane`.
void draw_rect_frame(Image8& screen, const Pane& pane, int x0, int y0, int x1, int y1, std::uint8_t color) {
  vfx_line_draw(screen, pane, x0, y0, x1, y0, color);
  vfx_line_draw(screen, pane, x0, y1, x1, y1, color);
  vfx_line_draw(screen, pane, x0, y0, x0, y1, color);
  vfx_line_draw(screen, pane, x1, y0, x1, y1, color);
}

}  // namespace

BatModeWnd::BatModeWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {
  for (auto& m : star_rotations_) {
    m = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  }
  const auto extra = [&](const std::string& key, int fallback) {
    const auto it = record.extras.find(key);
    return it == record.extras.end() ? fallback : std::stoi(it->second);
  };
  background_ = static_cast<std::uint8_t>(extra("BACKGRNDCOLOR", 0));
  cursor_shape_ = extra("CURSORSHAPE", 2);
  load_effects();
  // BatModeWnd_load_resources 0x18370: the projection centre, the haze
  // tables.
  cx_ = static_cast<std::int16_t>(static_cast<std::int16_t>(pane_.x1 - pane_.x0) >> 1);
  cy_ = static_cast<std::int16_t>(static_cast<std::int16_t>(pane_.y1 - pane_.y0) >> 1);
  load_haze();
}

void BatModeWnd::load_effects() {
  const std::vector<std::uint8_t> data = wm_.services().files.load("batfx.txt");
  TextLineReader reader(data);
  std::string line_buffer;
  const auto next = [&]() -> const std::string& {
    std::optional<std::string> line = reader.content_line();
    if (!line) {
      throw DataError("batfx.txt: truncated");
    }
    line_buffer = std::move(*line);
    return line_buffer;
  };
  int files = 0;
  if (std::sscanf(next().c_str(), "%d", &files) != 1) {
    throw DataError("batfx.txt: no shape file count");
  }
  if (files > 99) {
    throw DataError("batfx.txt: more than 99 shape files");
  }
  for (int i = 0; i < files; ++i) {
    // sscanf "%s": the first word.
    const std::string& line = next();
    const std::size_t end = line.find_first_of(" \t");
    effect_files_.push_back(line.substr(0, end));
  }
  for (std::size_t i = 0; i < game::kGizmoCount + 3; ++i) {
    std::array<int, 4> v{};
    const std::string& line = next();
    if (std::sscanf(line.c_str(), "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) != 4) {
      throw DataError(std::format("batfx.txt: bad effect row '{}'", line));
    }
    effect_rows_.push_back({static_cast<std::int16_t>(v[0]), static_cast<std::int16_t>(v[1]),
                            static_cast<std::int16_t>(v[2]), static_cast<std::int16_t>(v[3])});
  }
}

void BatModeWnd::load_haze() {
  const GameFiles& files = wm_.services().files;
  const auto load = [&](const std::string& name) { haze_.push_back(parse_haze_table(files.load(name), name)); };
  for (int i = 0; i < 31; ++i) {
    load(std::format("DATA\\BLACK{:02d}.HAZ", i));
  }
  load("DATA\\SLANE03.HAZ");
  for (char c = 'A'; c <= 'G'; ++c) {
    load(std::format("DATA\\RACE{}03.HAZ", c));
  }
}

const ShapeTable& BatModeWnd::file_shapes(int file) const {
  if (file < 0 || static_cast<std::size_t>(file) >= effect_files_.size()) {
    throw std::out_of_range(std::format("batfx.txt: no shape file {}", file));
  }
  return wm_.shapes().get(effect_files_[static_cast<std::size_t>(file)]);
}

void BatModeWnd::set_item_shape(Item& item, const ShapeTable& shapes, int frame) {
  item.scale = 1.0F;
  if (frame < 0 || static_cast<std::size_t>(frame) >= shapes.size()) {
    throw std::out_of_range(std::format("star system: no frame {} in a shape table of {}", frame, shapes.size()));
  }
  ShapeBounds b = vfx_shape_bounds(shapes.at(static_cast<std::size_t>(frame)));
  if (b.x0 > 1000000 || b.x0 < -1000000) {
    b = {};
  }
  item.bx0 = static_cast<std::int16_t>(b.x0);
  item.by0 = static_cast<std::int16_t>(b.y0);
  item.bx1 = static_cast<std::int16_t>(b.x1);
  item.by1 = static_cast<std::int16_t>(b.y1);
  item.shapes = &shapes;
  item.frame = frame;
}

void BatModeWnd::set_item_file(Item& item, int file, int frame) { set_item_shape(item, file_shapes(file), frame); }

bool BatModeWnd::next_frame(Item& item) {
  int frame = item.frame + 1;
  bool wrapped = false;
  if (item.shapes == nullptr || static_cast<std::size_t>(frame) >= item.shapes->size()) {
    wrapped = true;
    frame = 0;
  }
  if (item.shapes != nullptr) {
    set_item_shape(item, *item.shapes, frame);
  }
  return wrapped;
}

BatControlWnd* BatModeWnd::control() const { return dynamic_cast<BatControlWnd*>(wm_.find_window("BatControlWnd")); }

int BatModeWnd::find_item(ItemKind kind, std::int32_t object) const {
  for (std::size_t i = 0; i < item_count_; ++i) {
    if (items_[i].kind == kind && items_[i].object == object) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void BatModeWnd::build_scene() {
  const game::Session* s = core();
  item_count_ = 0;
  for (Item& item : items_) {
    item.flags = 0;
  }
  if (s == nullptr) {
    return;
  }
  const game::Cosmos& c = s->game->cosmos;
  const game::Star& star = c.stars.at(static_cast<std::size_t>(c.selected_star));
  const auto add = [&](ItemKind kind, std::int32_t object) -> Item& {
    if (item_count_ >= kMaxItems) {
      throw std::logic_error("star system: more than 128 display items");
    }
    Item& item = items_[item_count_++];
    item.kind = kind;
    item.object = object;
    return item;
  };
  Item& cursor = add(ItemKind::kCursor, 0);
  cursor.shapes = nullptr;
  cursor_item_ = 0;
  // One free item, the effects' sprite.
  add(ItemKind::kFree, 0);
  Item& sun = add(ItemKind::kStar, c.selected_star);
  set_item_file(sun, kFileSuns, star.type);
  for (std::int16_t i = 0; i < star.planet_count; ++i) {
    const game::PlanetIndex p = star.planets[static_cast<std::size_t>(i)];
    const game::Planet& planet = c.planets[static_cast<std::size_t>(p)];
    const int frame = planet.type * 5 + planet.size;
    if (frame > 0x36) {
      throw std::logic_error(std::format("star system: planet frame {}", frame));
    }
    set_item_file(add(ItemKind::kPlanet, p), kFilePlanets, frame);
  }
  // The scene radius starts from the last item built (the last planet, or
  // the star's first 12 bytes read as floats when it has none: a bug of the
  // original; in peCustom the star, at the centre).
  Vec3 last{};
  if (star.planet_count > 0) {
    last = c.planets[static_cast<std::size_t>(star.planets[static_cast<std::size_t>(star.planet_count - 1)])].position;
  } else if (s->game->variant != game::Variant::kPeCustom) {
    last = {std::bit_cast<float>(star.type),
            std::bit_cast<float>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(star.index)) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint16_t>(star.cluster)) << 16)),
            star.position[0]};
  }
  radius_ = f32(game::length(last));
  for (std::int16_t i = 0; i < star.lane_count; ++i) {
    const game::LaneIndex l = star.lanes[static_cast<std::size_t>(i)];
    const game::Lane& lane = c.lanes[static_cast<std::size_t>(l)];
    Item& item = add(ItemKind::kLaneEnd, l);
    int frame = (lane.flags & 1) != 0 ? 1 : 0;
    if ((lane.flags & 2) != 0) {
      frame += 2;
    }
    set_item_file(item, kFileLanes, frame);
    Vec3 end{};
    if (lane.star_a == c.selected_star) {
      item.flags |= kFlagStarSideA;
      end = lane.end_a;
    } else {
      item.flags &= static_cast<std::uint8_t>(~kFlagStarSideA);
      end = lane.end_b;
    }
    const float length = f32(game::length(end));
    if (!(radius_ >= length)) {
      radius_ = length;
    }
  }
  radius_ = f32(d(radius_) + kRadiusMargin);
  for (const game::ShipIndex i : game::cosmos_ships_at_star(c, star)) {
    const game::Ship& ship = c.ships[static_cast<std::size_t>(i)];
    if (ship.location_kind == game::ShipLocation::kSystem) {
      set_item_shape(add(ItemKind::kShip, i), wm_.shared_shapes(kShipShapes + ship.owner), ship.hull_size);
    }
  }
  for (std::int16_t i = 0; i < c.backdrop_count; ++i) {
    set_item_file(add(ItemKind::kBackdrop, i), kFileNebulae, c.backdrops[static_cast<std::size_t>(i)].frame);
  }
  for (std::size_t i = 0; i < item_count_; ++i) {
    items_[i].flags &= static_cast<std::uint8_t>(~kFlagCustomDraw);
  }
}

void BatModeWnd::update_clip_planes() {
  const double half = static_cast<double>(fov_) * d(kDegToRad) * d(kHalf);
  grid_radius_ = f32(std::tan(half));
  grid_radius_ = f32(d(distance_) * grid_radius_ * d(kTwo));
  far_limit_ = f32(d(default_distance_) * d(kHalf) + distance_);
  near_limit_ = f32(d(distance_) - d(grid_radius_) * d(kHalf));
}

void BatModeWnd::init_camera() {
  target_ = {};
  fov_ = kFov;
  const double half = static_cast<double>(fov_) * d(kDegToRad) * d(kHalf);
  const float distance = f32(d(radius_) / std::tan(half));
  distance_ = distance;
  default_distance_ = distance;
  camera_.set_projection(static_cast<float>(fov_), kNear, kFar, cx_);
  default_position_[2] = -camera_.focal;
  default_position_[1] = f32(d(default_position_[2]) * d(kThird));
  default_position_[0] = 0.0F;
  game::set_length(default_position_, distance_);
  camera_.rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  default_rotation_ = camera_.rotation;
  default_rotation_[6] = -default_position_[0];
  default_rotation_[7] = -default_position_[1];
  default_rotation_[8] = -default_position_[2];
  orthonormalize_keep_forward(default_rotation_);
  camera_.rotation = default_rotation_;
  camera_.position = default_position_;
  update_clip_planes();
}

void BatModeWnd::reset_camera() {
  target_ = {};
  distance_ = default_distance_;
  camera_.position = default_position_;
  camera_.rotation = default_rotation_;
  update_clip_planes();
}

void BatModeWnd::zoom(int step) {
  float s = static_cast<float>(step);
  if (s > 50.0F) {
    s = 50.0F;
  } else if (s < -50.0F) {
    s = -50.0F;
  }
  s = f32(d(s) * d(kZoomUnit) * wm_.rotation_speed());
  s = f32(1.0 + d(s));
  set_distance(f32(d(distance_) * s));
}

void BatModeWnd::set_distance(float distance) {
  distance_ = distance;
  if (!(distance_ >= camera_.focal)) {
    distance_ = camera_.focal;
  } else if (distance_ > default_distance_) {
    distance_ = default_distance_;
  }
  game::set_length(camera_.position, distance_);
  update_clip_planes();
}

void BatModeWnd::rotate_step(int degrees) { orbit(f32(static_cast<double>(degrees) * wm_.rotation_speed())); }

void BatModeWnd::orbit(float angle) {
  // Camera_reset 0x1baf0 (an orbit step): the position, then the basis,
  // turned about the vertical axis.
  game::rotate_y(camera_.position, angle);
  orient_rotate_y(camera_.rotation, angle);
}

bool BatModeWnd::takes_view_gesture(int x, int y, bool /*mouse*/) const {
  return state_ != State::kAnimating && x >= pane_.x0 && x <= pane_.x1 && y >= pane_.y0 && y <= pane_.y1;
}

void BatModeWnd::view_drag(int dx, int /*dy*/) {
  if (dx == 0 || state_ == State::kAnimating) {
    return;
  }
  orbit(static_cast<float>(dx) * kDragDegrees);
  refresh();
}

void BatModeWnd::view_pinch(float ratio) {
  if (!(ratio > 0.0F) || state_ == State::kAnimating) {
    return;
  }
  set_distance(distance_ / ratio);
  refresh();
}

void BatModeWnd::view_wheel(int notches) { view_pinch(std::pow(kWheelZoom, static_cast<float>(notches))); }

void BatModeWnd::center_on_item(int i) {
  const game::Session* s = core();
  if (i < 0 || s == nullptr) {
    return;
  }
  const game::Cosmos& c = s->game->cosmos;
  const Item& item = items_[static_cast<std::size_t>(i)];
  switch (item.kind) {
    case ItemKind::kStar:
      target_ = {};
      break;
    case ItemKind::kPlanet:
      target_ = c.planets[static_cast<std::size_t>(item.object)].position;
      break;
    case ItemKind::kShip:
      target_ = c.ships[static_cast<std::size_t>(item.object)].position;
      break;
    case ItemKind::kLaneEnd: {
      const game::Lane& lane = c.lanes[static_cast<std::size_t>(item.object)];
      target_ = lane.star_a == c.selected_star ? lane.end_a : lane.end_b;
      break;
    }
    default:
      break;
  }
}

void BatModeWnd::open_star() {
  build_scene();
  const game::Session* s = core();
  if (s == nullptr) {
    return;
  }
  const auto star = static_cast<std::size_t>(s->game->cosmos.selected_star);
  if (star >= kMaxStars) {
    throw std::logic_error("star system: star index past 99");
  }
  if (star_visited_[star] == 0) {
    init_camera();
    star_visited_[star] = -1;
  } else {
    camera_.position = star_positions_[star];
    camera_.rotation = star_rotations_[star];
    target_ = star_targets_[star];
    distance_ = f32(game::length(camera_.position));
    update_clip_planes();
  }
  refresh();
}

void BatModeWnd::save_star_camera() {
  const game::Session* s = core();
  if (s != nullptr) {
    const auto star = static_cast<std::size_t>(s->game->cosmos.selected_star);
    if (star >= kMaxStars) {
      throw std::logic_error("star system: star index past 99");
    }
    star_positions_[star] = camera_.position;
    star_rotations_[star] = camera_.rotation;
    star_targets_[star] = target_;
  }
  actions_ = 0;
  state_ = State::kIdle;
  selected_ = -1;
}

void BatModeWnd::refresh(bool redraw) {
  front_.clear();
  back_.clear();
  backdrops_.clear();
  quads_.clear();
  // BatModeWnd_update_view 0x173f4.
  camera_.update_view_offset(target_);
  project_scene();
  const auto by_depth = [this](std::vector<int>& list) {
    // DrawList_compare_depth 0x157b4: farther first. The lists (BatModeWnd
    // +0x1d36, +0x253a, +0x2d3e) are not 4-aligned: Watcom's qsort moves
    // the pivot to the base (the order of equal depths).
    watcom_qsort(
        std::span(list),
        [this](int a, int b) {
          const float da = items_[static_cast<std::size_t>(a)].depth;
          const float db = items_[static_cast<std::size_t>(b)].depth;
          if (!(da <= db)) {
            return -1;
          }
          return da < db ? 1 : 0;
        },
        false);
  };
  by_depth(front_);
  by_depth(back_);
  if (redraw) {
    draw(kDrawNormal);
  }
  sync_buttons();
}

void BatModeWnd::sync_buttons() {
  if (GWindow* abandon = wm_.find_window("ABANDON")) {
    abandon->set_greyed(!can_abandon());
  }
  if (GWindow* button = wm_.find_window(DeselectButton::kName)) {
    button->set_greyed(!can_deselect());
  }
}

void BatModeWnd::project_scene() {
  const game::Session* s = core();
  if (s == nullptr) {
    return;
  }
  const game::Cosmos& c = s->game->cosmos;
  const std::array<float, 12>& view = camera_.view;
  // The foot quad's corners, turned by the view without its translation.
  const Vec3 origin = transform_mat34(view, {0, 0, 0});
  std::array<Vec3, 4> corners = {Vec3{16, 0, 16}, Vec3{16, 0, -16}, Vec3{-16, 0, -16}, Vec3{-16, 0, 16}};
  for (Vec3& v : corners) {
    const Vec3 t = transform_mat34(view, v);
    v = game::sub(t, origin);
  }
  const float half_grid = f32(d(grid_radius_) * d(kHalf));
  const auto project = [&](const Vec3& v) { return project_point(v, camera_.focal, cx_, cy_); };
  Vec3 pos{};  // stale between items, as the original's locals
  for (std::size_t k = 0; k < item_count_; ++k) {
    Item& item = items_[k];
    if (item.kind == ItemKind::kCursor && !((state_ == State::kMovePoint || state_ == State::kMoveElevation) &&
                                            hovered_ == -1 && move_point_[1] != kNoCursor)) {
      continue;
    }
    if (item.kind == ItemKind::kFree) {
      continue;
    }
    item.unprojected = -1;
    if (item.kind == ItemKind::kBackdrop) {
      const Vec3 v = mul_mat3(camera_.rotation, c.backdrops[static_cast<std::size_t>(item.object)].position);
      if (0.0F < v[2]) {
        const ScreenPoint p = project(v);
        item.x = p.x;
        item.y = p.y;
        item.unprojected = 0;
        item.line = 0;
        item.depth = v[2];
        item.draw_scale = kScaleOne;
        backdrops_.push_back(static_cast<int>(k));
      }
      continue;
    }
    std::int32_t race = -1;
    if ((item.flags & kFlagFxPosition) != 0) {
      pos = fx_pos_[item.fx_position];
    } else {
      switch (item.kind) {
        case ItemKind::kStar:
          pos = {0, 0, 0};
          break;
        case ItemKind::kPlanet:
          pos = c.planets[static_cast<std::size_t>(item.object)].position;
          break;
        case ItemKind::kShip: {
          const game::Ship& ship = c.ships[static_cast<std::size_t>(item.object)];
          pos = ship.position;
          race = ship.owner;
          break;
        }
        case ItemKind::kLaneEnd: {
          const game::Lane& lane = c.lanes[static_cast<std::size_t>(item.object)];
          pos = (item.flags & kFlagStarSideA) != 0 ? lane.end_a : lane.end_b;
          break;
        }
        case ItemKind::kCursor:
          pos = move_point_;
          game::snap_to_grid(pos, s->grid);
          break;
        default:
          // verify_fail: the position of the item before.
          break;
      }
    }
    const Vec3 t = transform_mat34(view, pos);
    const Vec3 h = {f32(d(pos[0]) - target_[0]), f32(d(target_[1]) - target_[1]), f32(d(pos[2]) - target_[2])};
    const double horizontal = game::length(h);
    item.line = horizontal >= half_grid ? 0 : -1;
    if (!(camera_.near_plane < t[2]) || !(t[2] < camera_.far_plane)) {
      continue;
    }
    const ScreenPoint p = project(t);
    item.x = p.x;
    item.y = p.y;
    item.depth = t[2];
    item.unprojected = 0;
    if (item.kind == ItemKind::kEffect) {
      item.depth = f32(d(item.depth) + -1.0);
    }
    const float base = f32(d(default_distance_) * d(kScaleMin));
    float scale = kScaleMax;
    if (!(item.depth < camera_.near_plane)) {
      scale = f32(d(base) / item.depth);
    }
    if (scale >= kScaleMin) {
      if (scale > kScaleMax) {
        scale = kScaleMax;
      }
    } else {
      scale = kScaleMin;
    }
    item.draw_scale = game::chop(d(scale) * item.scale);
    const float grid_y = f32(d(distance_) * d(kGridDrop) + target_[1]);
    if (item.line != 0 && (item.flags & kFlagNoLine) == 0) {
      const Vec3 foot = transform_mat34(view, {pos[0], grid_y, pos[2]});
      const ScreenPoint f = project(foot);
      item.foot_x = f.x;
      item.foot_y = f.y;
      Quad quad;
      for (std::size_t i = 0; i < 4; ++i) {
        const Vec3 q = {f32(d(foot[0]) + corners[i][0]), f32(d(foot[1]) + corners[i][1]),
                        f32(d(foot[2]) + corners[i][2])};
        const ScreenPoint qp = project(q);
        quad.corners[i] = {qp.x, qp.y};
      }
      quad.race = race;
      if (quads_.size() < 64) {
        quads_.push_back(quad);
      }
      if (item.kind == ItemKind::kCursor) {
        cursor_quad_ = static_cast<std::int16_t>(quads_.size() - 1);
      }
    }
    if (0.0 < d(pos[1]) - grid_y) {
      item.above = 0;
      back_.push_back(static_cast<int>(k));
    } else {
      item.above = -1;
      front_.push_back(static_cast<int>(k));
    }
  }
}

void BatModeWnd::draw_list(const std::vector<int>& list) {
  const game::Session* s = core();
  if (s == nullptr) {
    return;
  }
  const game::Cosmos& c = s->game->cosmos;
  Image8& screen = wm_.screen();
  for (const int k : list) {
    const Item& item = items_[static_cast<std::size_t>(k)];
    if ((item.kind == ItemKind::kBackdrop && backdrops_on_ == 0) || item.kind == ItemKind::kFree) {
      continue;
    }
    const bool line =
        item.line != 0 && (elevation_ != 0 || item.kind == ItemKind::kCursor) && (item.flags & kFlagNoLine) == 0;
    if (line) {
      std::uint8_t color = item.above != 0 ? kLineAbove : kLineBelow;
      if (item.kind == ItemKind::kCursor && state_ == State::kMoveElevation) {
        color = kWhite;
      }
      vfx_line_draw(screen, pane_, item.foot_x, item.foot_y, item.x, item.y, color);
    }
    if ((item.flags & kFlagHidden) == 0) {
      if ((item.flags & kFlagCustomDraw) != 0) {
        fx_draw_item(item);
      } else if (item.shapes != nullptr) {
        if (item.kind == ItemKind::kBeam) {
          // Three lines to the item joined; the offsets use the x distance
          // twice (a bug of the original, fixed in peCustom: the y distance
          // in the sum and the x offset).
          const Item& to = items_[static_cast<std::size_t>(item.object)];
          const int dx = std::abs(item.x - to.x);
          const int dy = s->game->variant == game::Variant::kPeCustom ? std::abs(item.y - to.y) : dx;
          const int sum = dx + dy;
          int ox = 0;
          int oy = 0;
          if (sum != 0) {
            const double q = d(2.5F) / sum;
            oy = game::chop(dx * q + 0.5);
            ox = game::chop(q * dy + 0.5);
          }
          vfx_line_draw(screen, pane_, item.x + ox, item.y + oy, to.x + ox, to.y + oy, kBeamColor);
          vfx_line_draw(screen, pane_, item.x - ox, item.y - oy, to.x - ox, to.y - oy, kBeamColor);
          vfx_line_draw(screen, pane_, item.x, item.y, to.x, to.y, kBeamCore);
        } else {
          vfx_shape_transform_draw(screen, pane_, item.shapes->at(static_cast<std::size_t>(item.frame)), item.x, item.y,
                                   item.draw_scale, item.draw_scale);
        }
      }
    }
    const double rs = static_cast<double>(static_cast<std::uint32_t>(item.draw_scale)) * d(kOneScale);
    const float frs = f32(rs);
    if (item.kind == ItemKind::kPlanet) {
      const game::Planet& planet = c.planets[static_cast<std::size_t>(item.object)];
      if (game::planet_has_orbital_guard(*s, planet)) {
        const int r = static_cast<std::int16_t>(game::chop(d(item.bx1) * frs + d(kGuardMargin)));
        vfx_ellipse_draw(screen, pane_, item.x, item.y, r, r, race_color_base(c, planet.owner));
      }
    }
    if (k == selected_) {
      const int x0 = game::chop(d(item.bx0) * frs + item.x);
      const int y0 = game::chop(d(item.by0) * frs + item.y);
      const int x1 = game::chop(d(item.bx1) * frs + item.x);
      const int y1 = game::chop(d(item.by1) * frs + item.y);
      draw_rect_frame(screen, pane_, static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y0),
                      static_cast<std::int16_t>(x1), static_cast<std::int16_t>(y1), kWhite);
    }
  }
}

void BatModeWnd::fill_quad(const Quad& quad) {
  const game::Session* s = core();
  std::size_t table = kLaneHaze;
  if (quad.race >= 0 && quad.race < 7 && s != nullptr) {
    table = kRaceHaze + s->game->cosmos.races[static_cast<std::size_t>(quad.race)].color;
  }
  vfx_translate_polygon(wm_.screen(), pane_, quad.corners, haze_.at(table));
}

void BatModeWnd::outline_quads() {
  const game::Session* s = core();
  for (const Quad& quad : quads_) {
    std::uint8_t color = kQuadOutline;
    if (quad.race >= 0 && quad.race < 7 && s != nullptr) {
      color = race_color_base(s->game->cosmos, quad.race);
    }
    for (std::size_t i = 0; i < 4; ++i) {
      const PolyPoint& a = quad.corners[i];
      const PolyPoint& b = quad.corners[(i + 1) & 3];
      vfx_line_draw(wm_.screen(), pane_, a.x, a.y, b.x, b.y, color);
    }
  }
}

void BatModeWnd::draw_grid() {
  Image8& screen = wm_.screen();
  float r2 = f32(d(grid_radius_) * d(kHalf));
  r2 = f32(d(r2) * r2);
  const std::int32_t n = game::chop(d(grid_radius_) * d(0.03125F)) + 2;
  const std::int32_t start = ((n / 2) << 5) + 16;
  const std::int32_t m = game::chop(d(grid_radius_) * d(0.00625F)) + 2;
  std::int32_t bright = (m / 2) * 160 + 16;
  if (bright > start) {
    bright -= 160;
  }
  const bool minor = grid_radius_ < 960.0F;
  const float grid_y = f32(d(distance_) * d(kGridDrop));
  const auto project = [&](const Vec3& v) {
    return project_point(transform_mat34(camera_.view, v), camera_.focal, cx_, cy_);
  };
  for (int pass = 0; pass < 2; ++pass) {
    float c = static_cast<float>(start);
    int bright_index = -1;
    for (int i = 0; i < n; ++i) {
      if (static_cast<float>(bright) == c) {
        bright_index = i;
      }
      float t = f32(d(r2) - d(c) * c);
      if (0.0F < t) {
        t = f32(std::sqrt(d(t)));
        const float neg = -t;
        const float y = f32(d(grid_y) + target_[1]);
        Vec3 a{};
        Vec3 b{};
        if (pass == 0) {
          a = {f32(d(c) + target_[0]), y, f32(d(t) + target_[2])};
          b = {f32(d(c) + target_[0]), y, f32(d(neg) + target_[2])};
        } else {
          a = {f32(d(t) + target_[0]), y, f32(d(c) + target_[2])};
          b = {f32(d(neg) + target_[0]), y, f32(d(c) + target_[2])};
        }
        const ScreenPoint pa = project(a);
        const ScreenPoint pb = project(b);
        int color = -1;
        if (bright_index >= 0 && (i - bright_index) % 5 == 0) {
          color = kGridMajor;
        } else if (minor) {
          color = kGridMinor;
        }
        if (color != -1) {
          vfx_line_draw(screen, pane_, pa.x, pa.y, pb.x, pb.y, static_cast<std::uint8_t>(color));
        }
      }
      c = f32(d(c) + -32.0);
    }
  }
}

void BatModeWnd::draw_stars() {
  const game::Session* s = core();
  const auto* map = dynamic_cast<const CosWnd*>(wm_.find_window("COSMOSWnd"));
  if (s == nullptr || map == nullptr) {
    return;
  }
  const game::Cosmos& c = s->game->cosmos;
  const Vec3& here = c.stars[static_cast<std::size_t>(c.selected_star)].position;
  for (std::int16_t i = 0; i < c.star_count; ++i) {
    const game::Star& star = c.stars[static_cast<std::size_t>(i)];
    Vec3 v = game::sub(star.position, here);
    if ((std::bit_cast<std::uint32_t>(v[0]) & 0x7fffffffU) == 0 &&
        (std::bit_cast<std::uint32_t>(v[1]) & 0x7fffffffU) == 0 &&
        (std::bit_cast<std::uint32_t>(v[2]) & 0x7fffffffU) == 0) {
      continue;
    }
    v = mul_mat3(camera_.rotation, v);
    if (!(0.0F < v[2])) {
      continue;
    }
    const ScreenPoint p = project_point(v, camera_.focal, cx_, cy_);
    const auto depth = static_cast<std::int16_t>(game::chop(game::length(v) + d(kStarDepthOffset)));
    int shade = 3;
    if (depth > 220) {
      shade = 0;
    } else if (depth > 0) {
      shade = 1;
    } else if (depth > -220) {
      shade = 2;
    }
    vfx_shape_draw(wm_.screen(), pane_, map->star_shapes().at(static_cast<std::size_t>(star.type * 4 + shade)), p.x,
                   p.y);
  }
}

void BatModeWnd::draw_caption() {
  const game::Session* s = core();
  if (s == nullptr || hovered_ < 0) {
    return;
  }
  const game::Cosmos& c = s->game->cosmos;
  const Item& item = items_[static_cast<std::size_t>(hovered_)];
  std::string text = wm_.static_string(kStringUnknown);
  int color = kWhite;
  const auto species_name = [&](int race) {
    return wm_.static_string(kStringSpeciesNames + c.races[static_cast<std::size_t>(race)].species);
  };
  switch (item.kind) {
    case ItemKind::kStar:
      text =
          format_s(wm_.static_string(kStringSun), game::name_view(c.stars[static_cast<std::size_t>(item.object)].name));
      break;
    case ItemKind::kPlanet: {
      const game::Planet& planet = c.planets[static_cast<std::size_t>(item.object)];
      text = format_s(wm_.static_string(kStringPlanet), game::name_view(planet.name));
      if (planet.owner != game::kNoOwner) {
        text += format_s(wm_.static_string(kStringOwnedBy), species_name(planet.owner));
        color = race_color_base(c, planet.owner);
      }
      break;
    }
    case ItemKind::kShip: {
      const game::Ship& ship = c.ships[static_cast<std::size_t>(item.object)];
      text = species_name(ship.owner) + " " + std::string(game::name_view(ship.name));
      if (ship.owner == s->player_race && ship.destination_kind != game::ShipOrder::kNothing) {
        text += wm_.static_string(ship.destination_kind == game::ShipOrder::kGoStar ? kStringToLane : kStringToPlanet);
      }
      color = race_color_base(c, ship.owner);
      break;
    }
    case ItemKind::kLaneEnd: {
      const game::Lane& lane = c.lanes[static_cast<std::size_t>(item.object)];
      text = wm_.static_string((lane.flags & 1) != 0 ? kStringRedLink : kStringStarLane);
      const game::Star& other =
          c.stars[static_cast<std::size_t>((item.flags & kFlagStarSideA) != 0 ? lane.star_b : lane.star_a)];
      if ((other.explored_mask & s->player_mask) != 0) {
        text += wm_.static_string(kStringTo);
        text += game::name_view(other.name);
      }
      break;
    }
    default:
      break;
  }
  GameFont& font = wm_.large_font();
  font.pane() = GSystem::kScreenPane;
  font.draw_wrapped(wm_.screen(), pane_.x1 - 200, pane_.y1 - 30, text, 0, color, 0xff, 0);
}

void BatModeWnd::draw(int /*mode*/) {
  vfx_pane_wipe(wm_.screen(), pane_, background_);
  draw_list(backdrops_);
  draw_stars();
  draw_list(back_);
  if (quads_on_ != 0) {
    for (const Quad& quad : quads_) {
      fill_quad(quad);
    }
  }
  if (hovered_ == -1 && (state_ == State::kMovePoint || state_ == State::kMoveElevation) && cursor_quad_ >= 0 &&
      static_cast<std::size_t>(cursor_quad_) < quads_.size()) {
    fill_quad(quads_[static_cast<std::size_t>(cursor_quad_)]);
  }
  if (grid_ != 0 && (actions_ & 1) == 0) {
    draw_grid();
  }
  if (quads_on_ != 0) {
    outline_quads();
  }
  draw_list(front_);
  if (const game::Session* s = core()) {
    const game::Race& player = s->game->cosmos.races[s->player_race];
    wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(player.species), 3, 3, player.color);
  }
  draw_caption();
  update_cursor();
}

int BatModeWnd::pick(const std::vector<int>& list, int x, int y) const {
  for (auto it = list.rbegin(); it != list.rend(); ++it) {
    const Item& item = items_[static_cast<std::size_t>(*it)];
    const int dx = item.x - x;
    const int dy = item.y - y;
    if (item.kind == ItemKind::kSpot) {
      if (dy * dy + dx * dx < 0x50) {
        return *it;
      }
    } else if (item.kind != ItemKind::kBackdrop) {
      const float s = f32(static_cast<double>(static_cast<std::uint32_t>(item.draw_scale)) * d(kOneScale));
      const auto edge = [&](std::int16_t b, std::int16_t at) { return d(b) * s + at; };
      if (edge(item.bx0, item.x) <= x && edge(item.by0, item.y) <= y && x <= edge(item.bx1, item.x) &&
          y <= edge(item.by1, item.y)) {
        return *it;
      }
    }
  }
  return -1;
}

void BatModeWnd::pick_item(int x, int y) {
  hovered_ = pick(front_, x, y);
  if (hovered_ == -1) {
    hovered_ = pick(back_, x, y);
  }
  if (hovered_ == -1) {
    hovered_ = pick(backdrops_, x, y);
  }
}

void BatModeWnd::pick_grid_point(int x, int y) {
  const game::Session* s = core();
  const float sx = static_cast<float>(x - cx_);
  const float sy = static_cast<float>(-(y - cy_));
  const float dist = distance_;
  Vec3 v = {f32(d(sx) * dist), f32(0.0 * dist), f32(d(sy) * dist)};
  v[0] = f32(d(v[0]) * d(kPickX));
  v[2] = f32(d(v[2]) * d(kPickZ));
  const Vec3 forward = {camera_.rotation[6], 0.0F, camera_.rotation[8]};
  const Vec3 axis = {0.0F, 0.0F, 1.0F};
  const float angle = f32(angle_between(forward, axis));
  const float cy = f32(d(forward[2]) * axis[0] - d(forward[0]) * axis[2]);
  game::rotate_y(v, 0.0F > cy ? angle : -angle);
  move_point_ = {f32(d(v[0]) + target_[0]), f32(d(v[1]) + target_[1]), f32(d(v[2]) + target_[2])};
  game::snap_to_grid(move_point_, s != nullptr ? s->grid : 5);
}

void BatModeWnd::pick_elevation(int y) {
  const game::Session* s = core();
  move_point_[1] = f32(static_cast<double>(y - 240) * distance_ * d(kPickX) + target_[1]);
  game::snap_to_grid(move_point_, s != nullptr ? s->grid : 5);
}

void BatModeWnd::update_cursor() {
  if (wm_.hot_window() != id_) {
    return;
  }
  int frame = cursor_shape_;
  if (state_ == State::kTargeting) {
    frame += 8;
  } else if (state_ == State::kMovePoint || state_ == State::kMoveElevation) {
    frame += 6;
  }
  if (hovered_ != -1) {
    frame += 1;
  }
  wm_.set_pointer(frame);
}

int BatModeWnd::control_kind(int i) const {
  switch (items_[static_cast<std::size_t>(i)].kind) {
    case ItemKind::kShip:
      return BatControlWnd::kObjectShip;
    case ItemKind::kPlanet:
      return BatControlWnd::kObjectPlanet;
    case ItemKind::kLaneEnd:
      return BatControlWnd::kObjectLane;
    default:
      return BatControlWnd::kObjectNone;
  }
}

void BatModeWnd::click_item(bool left) {
  game::Session* s = core();
  BatControlWnd* ctrl = control();
  if (hovered_ < 0 || s == nullptr || ctrl == nullptr) {
    return;
  }
  bool notify = false;
  const Item& item = items_[static_cast<std::size_t>(hovered_)];
  if (!left) {
    center_on_item(hovered_);
  } else if (state_ == State::kTargeting) {
    // The kinds of the BatControlWnd: ship 3, planet 2, lane 4.
    const int kind = item.kind == ItemKind::kShip      ? BatControlWnd::kObjectShip
                     : item.kind == ItemKind::kPlanet  ? BatControlWnd::kObjectPlanet
                     : item.kind == ItemKind::kLaneEnd ? BatControlWnd::kObjectLane
                                                       : BatControlWnd::kObjectNone;
    if (ctrl->check_target(kind, item.object)) {
      state_ = State::kIdle;
      ctrl->execute(kind, item.object);
      selected_ = -1;
    }
  } else if (item.kind == ItemKind::kShip) {
    if (state_ == State::kIdle) {
      selected_ = hovered_;
      notify = true;
      const game::Ship& ship = s->game->cosmos.ships[static_cast<std::size_t>(item.object)];
      if (ship.owner == s->player_race && ship.power > 0) {
        state_ = State::kMovePoint;
      }
    }
  } else if (item.kind == ItemKind::kPlanet) {
    if (state_ != State::kMovePoint && state_ != State::kMoveElevation) {
      if (hovered_ == selected_ && state_ == State::kIdle) {
        open_planet_screen(item.object);
      } else {
        notify = true;
        selected_ = hovered_;
      }
    } else {
      state_ = State::kIdle;
      ctrl->execute(BatControlWnd::kObjectPlanet, item.object);
      build_scene();
      refresh();
    }
  } else if (item.kind == ItemKind::kLaneEnd) {
    if (state_ == State::kMovePoint || state_ == State::kMoveElevation) {
      state_ = State::kIdle;
      ctrl->execute(BatControlWnd::kObjectLane, item.object);
      build_scene();
      refresh();
    }
  }
  if (notify) {
    ctrl->select(control_kind(selected_), items_[static_cast<std::size_t>(selected_)].object);
  }
  update_cursor();
}

void BatModeWnd::open_planet_screen(std::int32_t planet) {
  core()->game->cosmos.selected_planet = static_cast<game::PlanetIndex>(planet);
  hovered_ = -1;
  wm_.post_message(kMessageChangeScreen, kScreenPlanet, 1);
}

bool BatModeWnd::can_open_selected_planet() const {
  return core() != nullptr && control() != nullptr && state_ == State::kIdle && selected_ >= 0 &&
         static_cast<std::size_t>(selected_) < item_count_ &&
         items_[static_cast<std::size_t>(selected_)].kind == ItemKind::kPlanet;
}

void BatModeWnd::open_selected_planet() {
  if (!can_open_selected_planet()) {
    return;
  }
  // The view's left click on its hovered selected planet.
  wm_.services().sound.play_sfx(kClickSound);
  open_planet_screen(items_[static_cast<std::size_t>(selected_)].object);
  update_cursor();
  refresh();
}

bool BatModeWnd::can_deselect() const { return selected_ != -1 && state_ != State::kAnimating; }

void BatModeWnd::deselect() {
  if (!can_deselect()) {
    return;
  }
  wm_.services().sound.play_sfx(kClickSound);
  drop_selection();
  state_ = State::kIdle;
  refresh();
  update_cursor();
}

bool BatModeWnd::can_abandon() const {
  const game::Session* s = core();
  if (selected_ < 0 || s == nullptr || state_ == State::kAnimating) {
    return false;
  }
  const Item& item = items_[static_cast<std::size_t>(selected_)];
  const game::Cosmos& c = s->game->cosmos;
  return (item.kind == ItemKind::kShip && c.ships[static_cast<std::size_t>(item.object)].owner == s->player_race) ||
         (item.kind == ItemKind::kPlanet && c.planets[static_cast<std::size_t>(item.object)].owner == s->player_race);
}

void BatModeWnd::abandon_selected() {
  game::Session* s = core();
  BatControlWnd* ctrl = control();
  if (selected_ < 0 || s == nullptr) {
    return;
  }
  const Item& item = items_[static_cast<std::size_t>(selected_)];
  game::Cosmos& c = s->game->cosmos;
  std::string topic;
  std::string name;
  if (item.kind == ItemKind::kShip && c.ships[static_cast<std::size_t>(item.object)].owner == s->player_race) {
    topic = "abanship";
    name = game::name_view(c.ships[static_cast<std::size_t>(item.object)].name);
  } else if (item.kind == ItemKind::kPlanet &&
             c.planets[static_cast<std::size_t>(item.object)].owner == s->player_race) {
    topic = "abanplan";
    name = game::name_view(c.planets[static_cast<std::size_t>(item.object)].name);
  }
  if (topic.empty()) {
    return;
  }
  auto* help = dynamic_cast<HelpWnd*>(wm_.find_window("HELPWINDOW"));
  if (help == nullptr) {
    return;
  }
  help->open_topic("help.txt", topic);
  HelpTopic& content = help->content();
  content.elements[0].text = c_format(content.elements[0].text, name.c_str());
  if (wm_.do_modal(*help) == 0) {
    return;
  }
  if (item.kind == ItemKind::kShip) {
    game::ship_abandon(*s, c.ships[static_cast<std::size_t>(item.object)]);
  } else {
    game::planet_abandon(*s, c.planets[static_cast<std::size_t>(item.object)]);
  }
  selected_ = -1;
  state_ = State::kIdle;
  if (ctrl != nullptr) {
    ctrl->select(BatControlWnd::kObjectNone, 0);
  }
  build_scene();
  refresh();
}

void BatModeWnd::select_from_control() {
  BatControlWnd* ctrl = control();
  const game::Session* s = core();
  if (ctrl == nullptr || s == nullptr) {
    return;
  }
  const BatControlWnd::Selection sel = ctrl->selection();
  const int i = find_item(sel.kind == BatControlWnd::kObjectShip ? ItemKind::kShip : ItemKind::kPlanet, sel.object);
  if (i < 0) {
    return;
  }
  selected_ = i;
  const Item& item = items_[static_cast<std::size_t>(i)];
  if (item.x < 0 || item.y < 0 || item.x > pane_.x1 - pane_.x0 || item.y > pane_.y1 - pane_.y0) {
    center_on_item(i);
  }
  if (item.kind == ItemKind::kShip) {
    const game::Ship& ship = s->game->cosmos.ships[static_cast<std::size_t>(item.object)];
    if (ship.owner == s->player_race && ship.power > 0) {
      state_ = State::kMovePoint;
    }
  }
  refresh();
}

void BatModeWnd::activate(int a, int b) {
  wm_.set_timer(id_, kMessageTick, 0, 0, 0);
  open_star();
  GWindow::handle_event(kEventActivate, a, b);
  actions_ = 0;
}

void BatModeWnd::deactivate(int a, int b) {
  if (state_ == State::kAnimating) {
    if (BatControlWnd* ctrl = control()) {
      ctrl->finish_action(false);
    }
  }
  wm_.kill_timer(id_, -1);
  save_star_camera();
  GWindow::handle_event(kEventDeactivate, a, b);
}

void BatModeWnd::press_areas(std::vector<PressArea>& out) const {
  out.push_back(PressArea{state_ == State::kAnimating ? Pane{} : pane_, false});
}

LongPress BatModeWnd::long_press(int /*x*/, int /*y*/) const {
  return state_ == State::kMovePoint || state_ == State::kMoveElevation ? LongPress::kMoveDrag : LongPress::kRight;
}

LongPress BatModeWnd::child_long_press(const GWindow& child, int /*x*/, int /*y*/) const {
  switch (child.message()) {
    case kMessageRotateLeft:
    case kMessageRotateRight:
    case kMessageZoomIn:
    case kMessageZoomOut:
      return LongPress::kLeftHold;
    default:
      return LongPress::kHelp;
  }
}

bool BatModeWnd::cancel_action() {
  if (state_ != State::kMovePoint && state_ != State::kMoveElevation) {
    return false;
  }
  wm_.services().sound.play_sfx(kClickSound);
  drop_selection();
  state_ = State::kIdle;
  refresh();
  update_cursor();
  return true;
}

void BatModeWnd::drop_selection() {
  if (selected_ != -1) {
    selected_ = -1;
    if (BatControlWnd* ctrl = control()) {
      ctrl->select(BatControlWnd::kObjectNone, 0);
    }
    state_ = State::kIdle;
  }
}

bool BatModeWnd::handle_event(int type, int a, int b) {
  const auto inside = [&](int x, int y) { return x >= pane_.x0 && x <= pane_.x1 && y >= pane_.y0 && y <= pane_.y1; };
  switch (type) {
    case kEventActivate:
      activate(a, b);
      return false;
    case kEventDeactivate:
      deactivate(a, b);
      return false;
    case kEventKey: {
      constexpr int kScanMinus = 0x0c;
      constexpr int kScanEquals = 0x0d;
      constexpr int kScanEnter = 0x1c;
      constexpr int kScanSpace = 0x39;
      const float speed = wm_.rotation_speed();
      if (b == kScanMinus) {
        if (static_cast<double>(speed) > 0.1) {
          wm_.set_rotation_speed(f32(static_cast<double>(speed) * 0.8));
        }
      } else if (b == kScanEquals) {
        if (speed < 10.0F) {
          wm_.set_rotation_speed(f32(static_cast<double>(speed) * 1.25));
        }
      } else if ((b == kScanEnter || b == kScanSpace) && state_ == State::kAnimating) {
        skip_ = true;
      }
      return false;
    }
    case kEventRightClick:
      if (state_ == State::kAnimating) {
        skip_ = true;
        return true;
      }
      if (!inside(a, b)) {
        return false;
      }
      wm_.services().sound.play_sfx(kClickSound);
      if (hovered_ == -1) {
        drop_selection();
      } else {
        click_item(false);
      }
      refresh();
      update_cursor();
      return true;
    case kEventLeftClick:
      if (state_ == State::kAnimating) {
        return true;
      }
      if (!inside(a, b)) {
        return false;
      }
      wm_.services().sound.play_sfx(kClickSound);
      if (hovered_ == -1) {
        if (state_ == State::kMovePoint) {
          if (cursor_quad_ != -1) {
            state_ = State::kMoveElevation;
            pick_elevation(b - pane_.y0);
          } else {
            state_ = State::kIdle;
          }
        } else if (state_ == State::kMoveElevation) {
          state_ = State::kIdle;
          if (BatControlWnd* ctrl = control()) {
            ctrl->execute(BatControlWnd::kObjectPoint, 0, BatControlWnd::stale_from_view_point(b));
          }
          selected_ = -1;
          build_scene();
        } else if (state_ == State::kIdle) {
          selected_ = -1;
          if (BatControlWnd* ctrl = control()) {
            ctrl->select(BatControlWnd::kObjectNone, 0);
          }
        } else {
          return true;
        }
      } else {
        click_item(true);
      }
      refresh();
      return true;
    case kEventMouseLeave:
      wm_.set_pointer(kPointerNormal);
      wm_.cancel_notify(id_, kEventMouseMove);
      return true;
    case kEventMouseEnter:
      update_cursor();
      wm_.request_notify(id_, WinMgr::kNotifyMove, kEventMouseMove, 0, 0);
      return true;
    case kEventMouseMove: {
      if (state_ == State::kAnimating) {
        return false;
      }
      if (!inside(a, b)) {
        hovered_ = -1;
        move_point_[1] = kNoCursor;
        refresh();
        return false;
      }
      if (actions_ != 0) {
        return false;
      }
      const int x = a - pane_.x0;
      const int y = b - pane_.y0;
      pick_item(x, y);
      if (hovered_ == -1) {
        if (state_ == State::kMovePoint) {
          pick_grid_point(x, y);
        } else if (state_ == State::kMoveElevation) {
          pick_elevation(y);
        }
      }
      refresh();
      if (state_ == State::kTargeting && hovered_ != -1 && selected_ != -1) {
        const Item& item = items_[static_cast<std::size_t>(hovered_)];
        bool possible = false;
        if (BatControlWnd* ctrl = control()) {
          possible = ctrl->check_target(control_kind(hovered_), item.object);
        }
        const Item& from = items_[static_cast<std::size_t>(selected_)];
        vfx_line_draw(wm_.screen(), pane_, from.x, from.y, item.x, item.y, possible ? kWhite : kTargetBad);
      }
      return false;
    }
    case kEventRefresh:
      if (state_ == State::kAnimating) {
        return false;
      }
      build_scene();
      refresh();
      return false;
    case kMessageRotateLeft:
    case kMessageRotateRight:
    case kMessageZoomIn:
    case kMessageZoomOut: {
      if (!active_) {
        return true;
      }
      int bits = 0;
      std::uint8_t v = static_cast<std::uint8_t>(actions_);
      if (type == kMessageRotateLeft) {
        if ((v & 1) == 0) {
          v = static_cast<std::uint8_t>((v & ~2) | 1);
        } else if ((v & 2) == 0) {
          v = static_cast<std::uint8_t>(v & ~1);
          actions_ = (actions_ & ~0xffU) | v;
          refresh();
        } else {
          v = static_cast<std::uint8_t>(v & ~2);
        }
        bits = 3;
      } else if (type == kMessageRotateRight) {
        if ((v & 1) == 0) {
          v |= 3;
        } else if ((v & 2) == 0) {
          v |= 2;
        } else {
          v = static_cast<std::uint8_t>(v & ~1);
          actions_ = (actions_ & ~0xffU) | v;
          refresh();
        }
        bits = 3;
      } else if (type == kMessageZoomIn) {
        if ((v & 4) == 0) {
          v = static_cast<std::uint8_t>((v & ~8) | 4);
        } else if ((v & 8) == 0) {
          v = static_cast<std::uint8_t>(v & ~4);
        } else {
          v = static_cast<std::uint8_t>(v & ~8);
        }
        bits = 0xc;
      } else {
        if ((v & 4) == 0) {
          v |= 0xc;
        } else if ((v & 8) == 0) {
          v |= 8;
        } else {
          v = static_cast<std::uint8_t>(v & ~4);
        }
        bits = 0xc;
      }
      actions_ = (actions_ & ~0xffU) | v;
      if (a == kEventLeftClick) {
        wm_.request_notify(id_, WinMgr::kNotifyLeft, kMessageStopAction, bits, 0);
      }
      return true;
    }
    case kMessageTick:
      if ((actions_ & 1) != 0) {
        rotate_step((actions_ & 2) != 0 ? 2 : -2);
      }
      if ((actions_ & 4) != 0) {
        zoom((actions_ & 8) != 0 ? 10 : -10);
      }
      if ((actions_ & 5) != 0) {
        refresh();
      }
      return false;
    case kMessageStopAction:
      actions_ &= ~static_cast<std::uint32_t>(a);
      if ((a & 1) != 0) {
        refresh();
      }
      wm_.cancel_notify(id_, kMessageStopAction);
      return true;
    case kMessageRecenter:
      actions_ = 0;
      reset_camera();
      refresh();
      return true;
    case kMessageGrid:
      grid_ = ~grid_;
      refresh();
      return false;
    case kMessageElevation:
      quads_on_ = ~elevation_;
      elevation_ = ~elevation_;
      refresh();
      return false;
    case kMessagePlanetWindow: {
      game::Session* s = core();
      if (s == nullptr) {
        return false;
      }
      game::Cosmos& c = s->game->cosmos;
      const game::Star& star = c.stars[static_cast<std::size_t>(c.selected_star)];
      if (a < 1 || a > star.planet_count) {
        return false;
      }
      c.selected_planet = star.planets[static_cast<std::size_t>(a - 1)];
      wm_.post_message(kMessageChangeScreen, kScreenPlanet, 1);
      return false;
    }
    case kMessageTarget:
      state_ = State::kTargeting;
      return false;
    case kMessageEffectTick:
      effect_tick();
      return false;
    case kMessageSelect:
      select_from_control();
      return false;
    case kMessageAbandon:
      abandon_selected();
      return false;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

bool BatModeWnd::save_state(std::span<std::uint8_t> out) const {
  if (out.size() < kStateSize) {
    return false;
  }
  std::size_t at = 0;
  put_bytes(out, at, star_positions_.data(), sizeof(star_positions_));
  put_bytes(out, at, star_rotations_.data(), sizeof(star_rotations_));
  put_bytes(out, at, star_visited_.data(), sizeof(star_visited_));
  put_bytes(out, at, star_targets_.data(), sizeof(star_targets_));
  put_bytes(out, at, target_.data(), sizeof(target_));
  const std::array<std::uint8_t, Camera3d::kSize> camera = camera_.serialize();
  put_bytes(out, at, camera.data(), camera.size());
  put_bytes(out, at, default_position_.data(), sizeof(default_position_));
  put_bytes(out, at, default_rotation_.data(), sizeof(default_rotation_));
  for (const float f : {near_limit_, far_limit_, distance_, default_distance_, radius_}) {
    put_bytes(out, at, &f, 4);
  }
  put_bytes(out, at, &cx_, 2);
  put_bytes(out, at, &cy_, 2);
  put_bytes(out, at, &fov_, 4);
  put_bytes(out, at, &grid_radius_, 4);
  for (const std::int32_t v : {grid_, unknown_503e_, elevation_, quads_on_, backdrops_on_}) {
    put_bytes(out, at, &v, 4);
  }
  return at == kStateSize;
}

void BatModeWnd::load_state(std::span<const std::uint8_t> in) {
  if (in.size() < kStateSize) {
    return;
  }
  std::size_t at = 0;
  get_bytes(in, at, star_positions_.data(), sizeof(star_positions_));
  get_bytes(in, at, star_rotations_.data(), sizeof(star_rotations_));
  get_bytes(in, at, star_visited_.data(), sizeof(star_visited_));
  get_bytes(in, at, star_targets_.data(), sizeof(star_targets_));
  get_bytes(in, at, target_.data(), sizeof(target_));
  camera_.deserialize(in.subspan(at).first<Camera3d::kSize>());
  at += Camera3d::kSize;
  get_bytes(in, at, default_position_.data(), sizeof(default_position_));
  get_bytes(in, at, default_rotation_.data(), sizeof(default_rotation_));
  for (float* f : {&near_limit_, &far_limit_, &distance_, &default_distance_, &radius_}) {
    get_bytes(in, at, f, 4);
  }
  get_bytes(in, at, &cx_, 2);
  get_bytes(in, at, &cy_, 2);
  get_bytes(in, at, &fov_, 4);
  get_bytes(in, at, &grid_radius_, 4);
  for (std::int32_t* v : {&grid_, &unknown_503e_, &elevation_, &quads_on_, &backdrops_on_}) {
    get_bytes(in, at, v, 4);
  }
}

}  // namespace oa
