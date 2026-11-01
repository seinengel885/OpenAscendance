#include "screens/res_wnd.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/c_format.h"
#include "core/watcom_qsort.h"
#include "game/player_actions.h"
#include "game/research.h"
#include "game/save.h"
#include "game/session.h"
#include "game/tables.h"
#include "gfx/vfx_poly.h"

namespace oa {

namespace {

constexpr int kScreenResearch = 20;

// The static strings of the screen.
constexpr int kStringNoEnding = 0x1c;        // ""
constexpr int kStringPlural = 0x1d;          // "s"
constexpr int kStringCurrentProject = 0x7a;  // "Current Project:"
constexpr int kStringNoProgress = 0x7b;      // "(No Progress)"
constexpr int kStringDays = 0x7c;            // "(%d day%s)"
constexpr int kStringAllows = 0x7d;          // "Allows"
constexpr int kStringShips = 0x7e;           // "Ships: Small and Medium Hulls"
constexpr int kStringLargeHull = 0x7f;       // "Large Ship Hull"
constexpr int kStringGiganticHull = 0x80;    // "Gigantic Ship Hull"
constexpr int kStringSelfManaged = 0x81;     // "(Self Managed)"
constexpr int kStringTitle = 0x82;           // "%s Knowledge %s"
constexpr int kStringSpecies = 101;          // the species names, 101..121

// `gwshare.txt` tables.
constexpr int kSmallShips = 14;  // smship0N.shp of race slot N (14 + slot)
constexpr int kGizmoShapes = 28;
constexpr int kPlanItemShapes = 30;
constexpr int kResearchShapes = 31;  // restree.shp, frame = item
constexpr int kRaceFlags = 32;
constexpr const char* kRingFile = "data\\resring.shp";

// The ring frames of `resring.shp`.
constexpr int kRingProject = 0;
constexpr int kRingNormal = 1;
constexpr int kRingHovered = 2;
constexpr int kRingNew = 3;

// The haze tables (`windows.txt` TYPE 200): 1..7 BLACK00..BLACK24.
constexpr int kDepthHazeBase = 4;

// The scan codes of the window's keys.
constexpr int kScanMinus = 0x0c;
constexpr int kScanEquals = 0x0d;
constexpr int kScanD = 0x20;
constexpr int kScanF = 0x21;
constexpr int kScanC = 0x2e;
constexpr int kScanM = 0x32;
constexpr int kScanHome = 0x47;
constexpr int kScanPgUp = 0x49;
constexpr int kScanEnd = 0x4f;
constexpr int kScanPgDn = 0x51;

// The planet items and ship hulls of the "Allows" list.
constexpr std::uint8_t kItemShip = 23;
constexpr std::uint16_t kResearchLargeHull = 19;
constexpr std::uint16_t kResearchGiganticHull = 31;

// The info column, the title bar and the tree's depth shading.
constexpr Pane kInfoPane{485, 7, 632, 426};
constexpr Pane kTitlePane{7, 7, 476, 37};
constexpr int kInfoWidth = 0x82;
constexpr int kInfoIconBottom = 120;
constexpr int kInfoNameY = 0x46 + 0x19;
constexpr int kInfoAllowsY = 0x8e;
constexpr int kInfoPictureHeight = 0x48;
constexpr int kRingOffset = 27;  // half the ring's height (line ends)
constexpr int kHoverDistance = 25;

int race_text_color(int color) { return color * 4 + 0x13; }

bool knows(const game::Research& research, std::uint16_t item, std::uint8_t race) {
  return ((research.items.at(item).known_mask >> (race & 0x1f)) & 1) != 0;
}

bool unseen(const game::Research& research, std::uint16_t item, std::uint8_t race) {
  return ((research.items.at(item).unseen_mask >> (race & 0x1f)) & 1) != 0;
}

// The player's research project (`0xf647a`), `ResWnd::kNone` for none.
std::uint16_t project_of(const game::Session& session) {
  return static_cast<std::uint16_t>(session.game->research.project.at(session.player_race));
}

int clamp3(std::int32_t v) { return std::clamp(v, -3, 3); }

// `reswin.txt` (developer file): the item count, then `x y z` per item.
std::optional<std::vector<Vec3>> read_reswin_txt(const GameFiles& files, std::size_t count) {
  const std::optional<std::vector<std::uint8_t>> data = files.try_load("reswin.txt");
  if (!data) {
    return std::nullopt;
  }
  const std::string_view text(reinterpret_cast<const char*>(data->data()), data->size());
  std::size_t at = 0;
  const auto next_int = [&]() -> std::optional<std::int32_t> {
    while (at < text.size() && !(text[at] == '-' || (text[at] >= '0' && text[at] <= '9'))) {
      ++at;
    }
    std::int32_t v = 0;
    const auto [end, ec] = std::from_chars(text.data() + at, text.data() + text.size(), v);
    if (ec != std::errc()) {
      return std::nullopt;
    }
    at = static_cast<std::size_t>(end - text.data());
    return v;
  };
  const std::optional<std::int32_t> n = next_int();
  if (!n || static_cast<std::size_t>(*n) != count) {
    return std::vector<Vec3>{};  // a wrong count keeps the initial positions
  }
  std::vector<Vec3> out;
  for (std::size_t i = 0; i < count; ++i) {
    const auto x = next_int();
    const auto y = next_int();
    const auto z = next_int();
    if (!x || !y || !z) {
      break;
    }
    out.push_back({static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)});
  }
  return out;
}

}  // namespace

ResWnd::ResWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {
  for (std::size_t i = 0; i < kMaxItems; ++i) {
    pos_[i] = {0.0F, static_cast<float>(i), 0.0F};
  }
  camera_.set_projection(30.0F, 1.0F, 100000.0F, 320);
  camera_.position = {0.0F, 0.0F, -150.0F};
}

std::vector<Vec3> ResWnd::layout_tree(const std::vector<std::array<std::uint8_t, 5>>& prereqs) {
  const std::size_t count = prereqs.size();
  std::vector<std::uint8_t> level(count, 0xff);
  std::uint32_t levels = 0;
  bool done = false;
  while (!done) {
    done = true;
    for (std::size_t i = 0; i < count; ++i) {
      if (level[i] != 0xff) {
        continue;
      }
      std::uint32_t l = 0;
      for (const std::uint8_t pre : prereqs[i]) {
        if (pre == 0xff) {
          break;
        }
        const std::uint32_t pl = level.at(pre);
        if (pl == 0xff) {
          done = false;
          l = 0xff;
          break;
        }
        if (l <= pl) {
          l = pl + 1;
          if (levels <= l) {
            levels = pl + 2;
          }
        }
      }
      level[i] = static_cast<std::uint8_t>(l);
    }
  }
  std::array<std::uint32_t, 256> per_level{};
  for (const std::uint8_t l : level) {
    ++per_level[l];
  }
  std::vector<Vec3> pos(count);
  for (std::size_t i = 0; i < count; ++i) {
    pos[i] = {0.0F, static_cast<float>(i), 0.0F};
  }
  for (std::uint32_t l = 0; l < levels; ++l) {
    float radius = 4.0F;
    float angle = static_cast<float>(240 + 30 * l);
    std::uint32_t n = per_level[l];
    if (l % 2 == 0 || n < 3) {
      if (n > 1) {
        radius = 23.0F;
      }
    } else {
      --n;  // the first item sits on the axis
    }
    std::uint32_t k = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (level[i] != l) {
        continue;
      }
      Vec3 p{0.0F, static_cast<float>(30 * l + (k & 1) * 15), radius};
      game::rotate_y(p, angle);
      pos[i] = p;
      angle = game::f32(static_cast<double>(360 / static_cast<std::int32_t>(n)) + angle);
      ++k;
      radius = 23.0F;
    }
  }
  return pos;
}

game::Session* ResWnd::core() const { return wm_.services().game.core(); }

bool ResWnd::save_state(std::span<std::uint8_t> out) const {
  if (out.size() != Camera3d::kSize) {
    return false;
  }
  const std::array<std::uint8_t, Camera3d::kSize> bytes = camera_.serialize();
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return true;
}

void ResWnd::load_state(std::span<const std::uint8_t> in) {
  if (in.size() == Camera3d::kSize) {
    camera_.deserialize(in.first<Camera3d::kSize>());
  }
}

void ResWnd::new_game() { camera_.orientation = {1, 0, 0, 0, 1, 0, 0, 0, 1}; }

void ResWnd::sync_with_game() {
  game::Session* session = core();
  if (session == nullptr) {
    return;
  }
  const game::Research& research = session->game->research;
  if (!laid_out_) {
    const std::size_t count = std::min<std::size_t>(research.count, kMaxItems);
    std::optional<std::vector<Vec3>> fixed = read_reswin_txt(wm_.services().files, count);
    std::vector<Vec3> pos;
    if (fixed) {
      pos = std::move(*fixed);
    } else {
      std::vector<std::array<std::uint8_t, 5>> prereqs(count);
      for (std::size_t i = 0; i < count; ++i) {
        prereqs[i] = research.items[i].prereqs;
      }
      pos = layout_tree(prereqs);
    }
    std::copy(pos.begin(), pos.end(), pos_.begin());
    laid_out_ = true;
  }
}

std::uint16_t ResWnd::item_at(int screen_x, int screen_y) const {
  const game::Session* session = core();
  if (session == nullptr) {
    return kNone;
  }
  const game::Research& research = session->game->research;
  const int x = screen_x - pane_.x0;
  const int y = screen_y - pane_.y0;
  for (int k = static_cast<int>(research.count) - 1; k >= 0; --k) {
    const std::uint16_t item = order_[static_cast<std::size_t>(k)];
    if (!game::research_can_research(research, session->player_race, item)) {
      continue;
    }
    const ScreenPoint p = screen_[item];
    if (p.x - kHoverDistance < x && x < p.x + kHoverDistance && p.y - kHoverDistance < y && y < p.y + kHoverDistance) {
      return item;
    }
  }
  return kNone;
}

void ResWnd::press_areas(std::vector<PressArea>& out) const {
  out.push_back(PressArea{pane_, false});
  const game::Session* session = core();
  if (session == nullptr) {
    return;
  }
  const game::Research& research = session->game->research;
  for (std::size_t k = 0; k < research.count; ++k) {
    const std::uint16_t item = order_[k];
    if (game::research_can_research(research, session->player_race, item)) {
      // `item_at`'s open box around the item.
      const ScreenPoint p = screen_[item];
      const int x = pane_.x0 + p.x;
      const int y = pane_.y0 + p.y;
      out.push_back(PressArea{
          Pane{x - kHoverDistance + 1, y - kHoverDistance + 1, x + kHoverDistance - 1, y + kHoverDistance - 1}, false});
    }
  }
}

void ResWnd::update_hover() {
  const int x = wm_.input().x;
  const int y = wm_.input().y;
  const bool in_info = kInfoPane.contains(x, y);
  std::uint16_t found = item_at(x, y);
  if (found != kNone) {
    last_hovered_ = found;
  } else if (in_info) {
    found = last_hovered_;
  }
  if (!in_info && !contains(x, y)) {
    wm_.cancel_notify(id_, kEventMouseMove);
  }
  if (found != hovered_) {
    hovered_ = found;
    redraw_info_ = true;
    redraw_tree_ = true;
  }
}

void ResWnd::toggle_motion(int message, int button) {
  const auto toggle = [this](std::uint32_t on, std::uint32_t dir, bool towards) {
    // A click on the direction running stops; the other direction reverses.
    if ((motion_ & on) == 0) {
      motion_ = towards ? (motion_ | on | dir) : ((motion_ & ~dir) | on);
    } else if (((motion_ & dir) != 0) == towards) {
      motion_ &= ~on;
    } else {
      motion_ = towards ? (motion_ | dir) : (motion_ & ~dir);
    }
  };
  switch (message) {
    case kRotateLeft:
      toggle(kRotating, kRotatingRight, false);
      break;
    case kRotateRight:
      toggle(kRotating, kRotatingRight, true);
      break;
    case kMoveUp:
      toggle(kMoving, kMovingDown, false);
      break;
    case kMoveDown:
      toggle(kMoving, kMovingDown, true);
      break;
    default:
      break;
  }
  // Held with the left button: the motion stops at the next left button
  // change; a right click leaves it running.
  if (button == kEventLeftClick) {
    const int bits = message == kMoveUp || message == kMoveDown ? kMoving : kRotating;
    wm_.request_notify(id_, WinMgr::kNotifyLeft, kStopMotion, bits, 0);
  }
}

LongPress ResWnd::child_long_press(const GWindow& child, int /*x*/, int /*y*/) const {
  return child.message() >= kRotateLeft && child.message() <= kMoveDown ? LongPress::kLeftHold : LongPress::kHelp;
}

bool ResWnd::handle_event(int type, int a, int b) {
  game::Session* session = core();
  switch (type) {
    case kEventActivate:
      sync_with_game();
      motion_ = 0;
      redraw_title_ = true;
      redraw_info_ = true;
      hovered_ = kNone;
      last_hovered_ = kNone;
      GWindow::handle_event(type, a, b);
      wm_.set_timer(id_, kTimer, 0, 0, 0);
      return false;
    case kEventDeactivate:
      if (session != nullptr) {
        game::research_clear_unseen(session->game->research, session->player_race);
      }
      GWindow::handle_event(type, a, b);
      wm_.kill_timer(id_, -1);
      return false;
    case kEventLeftClick:
    case kEventRightClick:
      if (active_ && session != nullptr) {
        update_hover();
        game::Research& research = session->game->research;
        // Not the item the info column keeps.
        if (hovered_ != kNone && hovered_ == item_at(wm_.input().x, wm_.input().y) &&
            !knows(research, hovered_, session->player_race) &&
            game::research_can_research(research, session->player_race, hovered_)) {
          wm_.services().sound.play_sfx(0);
          redraw_tree_ = true;
          redraw_info_ = true;
          game::research_set_project(research, session->player_race, static_cast<std::uint8_t>(hovered_));
        }
      }
      return false;
    case kEventKey:
      switch (b) {
        case kScanMinus:
          if (static_cast<double>(wm_.rotation_speed()) > 0.1) {
            wm_.set_rotation_speed(game::f32(static_cast<double>(wm_.rotation_speed()) * 0.8));
            return false;
          }
          break;
        case kScanEquals:
          if (wm_.rotation_speed() < 10.0F) {
            wm_.set_rotation_speed(game::f32(static_cast<double>(wm_.rotation_speed()) * 1.25));
            return false;
          }
          break;
        case kScanM:
          if (session != nullptr) {
            game::research_toggle_self_managed(session->game->research);
          }
          wm_.draw_background(kScreenResearch);
          redraw_title_ = true;
          redraw_tree_ = true;
          break;
        case kScanHome:
        case kScanPgUp:
          camera_.position[1] = -100.0F;
          draw(kDrawNormal);
          break;
        case kScanEnd:
        case kScanPgDn:
          camera_.position[1] = 1000000.0F;
          draw(kDrawNormal);
          break;
        // The developer keys (only with `nougat.lf`), not supported here:
        // D learns, F forgets every item, C completes the project.
        case kScanD:
        case kScanF:
        case kScanC:
        default:
          break;
      }
      update_hover();
      return false;
    case kEventMouseLeave:
      update_hover();
      if (active_ && shown_ && mouse_focus_) {
        draw(kDrawNormal);
      }
      return true;
    case kEventMouseEnter:
      wm_.cancel_notify(id_, kEventMouseMove);
      wm_.request_notify(id_, WinMgr::kNotifyMove, kEventMouseMove, 0, 0);
      update_hover();
      if (active_ && shown_ && mouse_focus_) {
        draw_hilite(kDrawNormal);
      }
      return true;
    case kEventMouseMove:
      update_hover();
      return false;
    case kEventHelpClick:
      if (kInfoPane.contains(a, b)) {
        for (const InfoEntry& e : info_entries_) {
          if (e.y0 <= b - kInfoPane.y0 && b - kInfoPane.y0 < e.y1) {
            wm_.post_message(kMessageHelp, e.index, e.help);
            return true;
          }
        }
      }
      return GWindow::handle_event(type, a, b);
    case kRotateLeft:
    case kRotateRight:
    case kMoveUp:
    case kMoveDown:
      toggle_motion(type, a);
      return false;
    case kStopMotion:
      motion_ &= ~static_cast<std::uint32_t>(a);
      wm_.cancel_notify(id_, -1);
      return false;
    case kTimer:
      if ((motion_ & kRotating) != 0) {
        const float step = (motion_ & kRotatingRight) != 0 ? -3.0F : 3.0F;
        camera_.rotate_forward_y(game::f32(static_cast<double>(step) * wm_.rotation_speed()));
        redraw_tree_ = true;
      }
      if ((motion_ & kMoving) != 0) {
        const float step = (motion_ & kMovingDown) != 0 ? 2.0F : -2.0F;
        const float delta = game::f32(static_cast<double>(step) * wm_.rotation_speed());
        camera_.position[1] = game::f32(static_cast<double>(camera_.position[1]) + delta);
        redraw_tree_ = true;
      }
      if (redraw_tree_) {
        draw(kDrawNormal);
      }
      return false;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

bool ResWnd::takes_view_gesture(int x, int y, bool mouse) const {
  return core() != nullptr && contains(x, y) && (!mouse || item_at(x, y) == kNone);
}

void ResWnd::view_drag(int dx, int dy) {
  if (dx != 0) {
    camera_.rotate_forward_y(game::f32(static_cast<double>(-dx) * kDragDegrees));
  }
  if (dy != 0) {
    // The tree follows the finger at the depth of its axis.
    const double per_pixel = -static_cast<double>(camera_.position[2]) / camera_.focal;
    camera_.position[1] = game::f32(static_cast<double>(camera_.position[1]) - dy * per_pixel);
  }
  redraw_tree_ = true;
}

void ResWnd::view_wheel(int notches) {
  camera_.position[1] = game::f32(static_cast<double>(camera_.position[1]) - notches * kWheelStep);
  redraw_tree_ = true;
}

void ResWnd::project_items() {
  const game::Session& session = *core();
  const std::size_t count = session.game->research.count;
  camera_.update_view();
  for (std::size_t i = 0; i < count; ++i) {
    const Vec3 v = camera_.transform(pos_[i]);
    depth_[i] = v[2];
    screen_[i] = project_point(v, camera_.focal, kCenterX, kCenterY);
    order_[i] = static_cast<std::uint16_t>(i);
  }
  // `ResWnd_compare_depth` 0x432c4: the farthest first.
  watcom_qsort(std::span(order_.data(), count), [this](std::uint16_t a, std::uint16_t b) {
    const float da = depth_[a];
    const float db = depth_[b];
    return db < da ? -1 : da < db ? 1 : 0;
  });
}

void ResWnd::draw_tree() {
  const game::Session& session = *core();
  const game::Research& research = session.game->research;
  const std::uint8_t player = session.player_race;
  const std::size_t count = research.count;
  Image8& screen = wm_.screen();
  // The camera height is kept between the lowest and the highest visible
  // item (the first item's height as the start of both).
  float low = pos_[0][1];
  float high = pos_[0][1];
  for (std::size_t i = 0; i < count; ++i) {
    if (game::research_can_research(research, player, static_cast<std::uint16_t>(i))) {
      low = std::min(low, pos_[i][1]);
      high = std::max(high, pos_[i][1]);
    }
  }
  camera_.position[1] = std::clamp(camera_.position[1], low, high);
  project_items();
  const std::uint16_t project = project_of(session);
  const std::vector<HazeTable>& haze = wm_.services().gsystem.haze_tables();
  const ShapeTable& icons = wm_.shared_shapes(kResearchShapes);
  const ShapeTable& rings = wm_.shapes().get(kRingFile);
  const double cam_z = camera_.position[2];
  for (std::size_t k = 0; k < count; ++k) {
    const std::uint16_t item = order_[k];
    if (!game::research_can_research(research, player, item)) {
      continue;
    }
    const int shade = clamp3(game::chop((static_cast<double>(depth_[item]) + cam_z) * 0.25));
    const HazeTable& table = haze.at(static_cast<std::size_t>(shade + kDepthHazeBase));
    const bool known = knows(research, item, player);
    std::int32_t size = 0;
    if (known) {
      size = 20;
    } else if (project == item) {
      const std::int32_t cost = research.items[item].cost;
      size = std::clamp<std::int32_t>(research.progress.at(player) * 18 / cost + 2, -17, 17);
    }
    // The lines to the visible prerequisites, from the ring's top to their
    // ring's bottom.
    const ScreenPoint p = screen_[item];
    for (const std::uint8_t pre : research.items[item].prereqs) {
      if (pre == 0xff) {
        break;
      }
      if (!game::research_can_research(research, player, pre)) {
        continue;
      }
      const double mid = (static_cast<double>(depth_[item]) + depth_[pre]) * 0.5;
      const int line_shade = clamp3(game::chop((mid + cam_z) * 0.25));
      const std::uint8_t color = pre == hovered_ ? kTextColor : static_cast<std::uint8_t>(0x6b - line_shade);
      const ScreenPoint q = screen_[pre];
      vfx_line_draw(screen, pane_, p.x, p.y - kRingOffset, q.x, q.y + kRingOffset, color);
    }
    if (size != 0) {
      const std::int32_t scale = (size << 16) / 20;
      vfx_shape_transform_draw(screen, pane_, icons.at(item), p.x, p.y, scale, scale, &table);
    }
    int ring = kRingNormal;
    if (project == item && !known) {
      ring = kRingProject;
    }
    if (unseen(research, item, player)) {
      ring = kRingNew;
    }
    if (hovered_ == item) {
      ring = kRingHovered;
      vfx_shape_draw(screen, pane_, rings.at(static_cast<std::size_t>(ring)), p.x, p.y);
    } else {
      vfx_shape_translate_draw(screen, pane_, rings.at(static_cast<std::size_t>(ring)), p.x, p.y, table);
    }
  }
}

void ResWnd::draw_info() {
  const game::Session& session = *core();
  const game::Research& research = session.game->research;
  const std::uint8_t player = session.player_race;
  Image8& screen = wm_.screen();
  vfx_pane_wipe(screen, kInfoPane, 0);
  info_entries_.clear();
  const std::uint16_t project = project_of(session);
  const std::uint16_t shown = hovered_ != kNone ? hovered_ : project;
  if (shown == kNone) {
    return;
  }
  GameFont& font = wm_.large_font();
  font.pane() = kInfoPane;
  int color = kDefault;
  if (shown == project) {
    if (shown != hovered_) {
      color = race_text_color(session.game->cosmos.races.at(player).color);
    }
    font.draw_wrapped(screen, 0, 10, wm_.static_string(kStringCurrentProject), kFontCenterH, color, kDefault,
                      kInfoWidth);
  }
  // The icon and its ring, centred in the top of the column.
  Pane top = kInfoPane;
  top.y1 = kInfoIconBottom;
  const Shape& icon = wm_.shared_shapes(kResearchShapes).at(shown);
  const PolyPoint at = center_shape(top, icon);
  vfx_shape_draw(screen, top, icon, at.x, at.y);
  int ring = kRingNormal;
  const bool known = knows(research, shown, player);
  if (shown == project && !known) {
    ring = kRingProject;
  }
  if (unseen(research, shown, player)) {
    ring = kRingNew;
  }
  if (shown == hovered_) {
    ring = kRingHovered;
  }
  vfx_shape_draw(screen, top, wm_.shapes().get(kRingFile).at(static_cast<std::size_t>(ring)), at.x, at.y);
  int y = kInfoNameY;
  y += font.draw_wrapped(screen, 0, y, game::name_view(research.items[shown].name), kFontCenterH, color, kDefault,
                         kInfoWidth);
  if (!known) {
    const std::int32_t rate = game::research_race_rate(session, player);
    const std::int32_t days = game::research_days_left(research.items[shown].cost, research.progress.at(player), rate);
    std::string text;
    if (days == 0xffff) {
      text = wm_.static_string(kStringNoProgress);
    } else {
      text = c_format(wm_.static_string(kStringDays), days,
                      wm_.static_string(days == 1 ? kStringNoEnding : kStringPlural).c_str());
    }
    font.draw_wrapped(screen, 0, y, text, kFontCenterH, color, kDefault, kInfoWidth);
  }
  // "Allows": the planet items, the gizmos and the hulls of the item.
  y = kInfoAllowsY;
  const auto allows = [&] {
    if (y == kInfoAllowsY) {
      y +=
          font.draw_wrapped(screen, 0, y, wm_.static_string(kStringAllows), kFontCenterH, color, kDefault, kInfoWidth) +
          10;
    }
  };
  // The picture's pane below a name.
  const auto picture_pane = [&](int name_bottom) {
    Pane pane = kInfoPane;
    pane.y0 = name_bottom + 1;
    pane.y1 = name_bottom + kInfoPictureHeight;
    return pane;
  };
  const game::GameTables& tables = *session.tables;
  const ShapeTable& small_ship = wm_.shared_shapes(kSmallShips + player);
  for (std::size_t i = 0; i < game::PlanItemTable::kItems; ++i) {
    if (tables.planitems.items[i].research_requirement != shown) {
      continue;
    }
    allows();
    const bool ship = i == kItemShip;
    const int entry_y = y;
    y += font.draw_wrapped(screen, 0, y, ship ? wm_.static_string(kStringShips) : tables.planitems.items[i].name,
                           kFontCenterH, color, kDefault, kInfoWidth);
    const Pane pane = picture_pane(y);
    const Shape& shape = ship ? small_ship.at(0) : wm_.shared_shapes(kPlanItemShapes).at(i);
    const std::int32_t scale = ship ? 0x8000 : kScaleOne;
    const PolyPoint c = center_shape(pane, shape);
    vfx_shape_transform_draw(screen, pane, shape, c.x, c.y, scale, scale);
    y += kInfoPictureHeight;
    info_entries_.push_back(InfoEntry{entry_y, y, kHelpPlanet, static_cast<int>(i)});
  }
  for (std::size_t i = 0; i < game::kGizmoCount; ++i) {
    if (tables.gizmos[i].research_requirement != shown) {
      continue;
    }
    allows();
    const int entry_y = y;
    y += font.draw_wrapped(screen, 0, y, tables.gizmos[i].name, kFontCenterH, color, kDefault, kInfoWidth);
    const Pane pane = picture_pane(y);
    const Shape& shape = wm_.shared_shapes(kGizmoShapes).at(i);
    const PolyPoint c = center_shape(pane, shape);
    vfx_shape_draw(screen, pane, shape, c.x, c.y);
    y += kInfoPictureHeight;
    info_entries_.push_back(InfoEntry{entry_y, y, kHelpGizmo, static_cast<int>(i)});
  }
  if (shown == kResearchLargeHull || shown == kResearchGiganticHull) {
    allows();
    const bool gigantic = shown == kResearchGiganticHull;
    y += font.draw_wrapped(screen, 0, y, wm_.static_string(gigantic ? kStringGiganticHull : kStringLargeHull),
                           kFontCenterH, color, kDefault, kInfoWidth);
    const Pane pane = picture_pane(y);
    const Shape& shape = small_ship.at(gigantic ? 3 : 2);
    const PolyPoint c = center_shape(pane, shape);
    vfx_shape_transform_draw(screen, pane, shape, c.x, c.y, 0x8000, 0x8000);
  }
}

void ResWnd::draw_title() {
  const game::Session& session = *core();
  const game::Race& race = session.game->cosmos.races.at(session.player_race);
  wm_.draw_colored(kTitlePane, wm_.shared_shapes(kRaceFlags).at(race.species), 3, 3, race.color);
  const std::string& managed =
      session.game->research.self_managed == -1 ? wm_.static_string(kStringSelfManaged) : std::string();
  const std::string title = c_format(wm_.static_string(kStringTitle),
                                     wm_.static_string(kStringSpecies + race.species).c_str(), managed.c_str());
  GameFont& font = wm_.large_font();
  font.pane() = kTitlePane;
  font.draw_wrapped(wm_.screen(), 0, 0, title, kFontCenterH | kFontCenterV, race_text_color(race.color), 0xff, 0);
}

void ResWnd::draw(int /*mode*/) {
  vfx_pane_wipe(wm_.screen(), pane_, 0);
  if (core() == nullptr) {
    return;
  }
  draw_tree();
  if (redraw_info_) {
    draw_info();
    redraw_info_ = false;
  }
  if (redraw_title_) {
    draw_title();
    redraw_title_ = false;
  }
  redraw_tree_ = false;
}

void add_research_windows(WindowFactory& factory) {
  factory.add(9, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<ResWnd>(wm, record, shapes);
  });
}

}  // namespace oa
