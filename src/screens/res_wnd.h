// The research screen (screen 20, `reswin.cpp`): `ResWnd` (TYPE 9 RESWIN,
// size 0x9dd, vtable 0x85e98), the research tree as rings in 3D around a
// vertical axis, the info column of the hovered item (or of the current
// project; the remake keeps the last hovered item until another one is
// hovered or a click hits no item, so the column's planet items and gizmos
// can show their help) and the title bar; its children RESROTLEFT/RIGHT and
// RESMOVEUP/DOWN are plain buttons sending messages 50..53
// (`docs/11-screens.md`, "Research screen").
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "screens/camera3d.h"
#include "ui/gwindow.h"
#include "ui/winmgr.h"

namespace oa::game {
struct Session;
}  // namespace oa::game

namespace oa {

class ResWnd final : public GWindow {
 public:
  static constexpr std::size_t kMaxItems = 100;
  static constexpr std::uint16_t kNone = 0xffff;
  // The messages of the window: the buttons' (p1 = 4 left / 5 right
  // button), the end of a held button (p1 = the motion bits to clear) and
  // the timer.
  enum Message : int {
    kRotateLeft = 50,
    kRotateRight = 51,
    kMoveUp = 52,
    kMoveDown = 53,
    kStopMotion = 0x38,
    kTimer = 0x3a,
  };
  // The motion flags (`0xf64ac`).
  enum Motion : std::uint32_t { kMoving = 1, kMovingDown = 2, kRotating = 4, kRotatingRight = 8 };
  // The window-relative centre of the projection.
  static constexpr std::int32_t kCenterX = 235;
  static constexpr std::int32_t kCenterY = 240;
  // The remake's drag turn, in degrees per pixel (the star map's), and the
  // camera height of a wheel notch (half the rings' spacing).
  static constexpr double kDragDegrees = 0.5;
  static constexpr double kWheelStep = 15.0;

  // `ResWnd_ctor` 0x443a0 / `ResWnd_init` 0x4250c: the camera (30 degree
  // field of view, at (0, 0, -150)); the tree is laid out at the first use
  // of a game (`layout_tree`).
  ResWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes);

  // `ResWnd_handle_event` 0x42a84.
  bool handle_event(int type, int a, int b) override;
  // The tree and the items' hover boxes (a click acts on the hovered item
  // wherever the pointer is), not targets.
  void press_areas(std::vector<PressArea>& out) const override;
  // A long press holds the rotation and move buttons (they act while held).
  LongPress child_long_press(const GWindow& child, int x, int y) const override;
  // `ResWnd_draw` 0x4331c: the tree, then the info column and the title bar
  // when they are marked for redrawing.
  void draw(int mode) override;
  // `ResWnd_serialize` 0x44374: the camera (0x98 bytes) in the saves'
  // window state.
  bool save_state(std::span<std::uint8_t> out) const override;
  void load_state(std::span<const std::uint8_t> in) override;
  // "Begin New Game": the camera's orientation back to the identity.
  void new_game() override;

  // The remake's gestures on the tree (inside the window): a horizontal
  // drag turns it as the rotation buttons do (`kDragDegrees` per pixel), a
  // vertical one moves the camera up or down as the move buttons do, the
  // tree following the finger; each wheel notch moves it `kWheelStep` (away
  // from the player: up, towards Home / PgUp). The heights stay within the
  // visible tree (the draw's clamp). The mouse drags only from a point with
  // no item (a press on an item picks it).
  bool takes_view_gesture(int x, int y, bool mouse) const override;
  void view_drag(int dx, int dy) override;
  void view_wheel(int notches) override;

  // The tree layout of `ResWnd_init` without `reswin.txt`: the level of an
  // item is 0 without prerequisites, else 1 + the highest level of its
  // prerequisites; the items of a level go around the axis (see the doc).
  // `prereqs[i]` are item i's (0xff-terminated) prerequisites.
  static std::vector<Vec3> layout_tree(const std::vector<std::array<std::uint8_t, 5>>& prereqs);

  const Camera3d& camera() const { return camera_; }
  Camera3d& camera() { return camera_; }
  const std::array<Vec3, kMaxItems>& positions() const { return pos_; }
  // The projected positions (window-relative) and view depths of the last
  // draw, and the draw order (far first).
  ScreenPoint screen_position(std::size_t item) const { return screen_[item]; }
  float depth(std::size_t item) const { return depth_[item]; }
  const std::array<std::uint16_t, kMaxItems>& draw_order() const { return order_; }
  std::uint16_t hovered() const { return hovered_; }
  std::uint32_t motion() const { return motion_; }

 private:
  game::Session* core() const;
  // Lays the tree out when a game is there.
  void sync_with_game();
  // `ResWnd_update_hover` 0x428e8: the front-most visible item within 24
  // pixels of the pointer on both axes (and the last one found, which the
  // info column keeps showing). Stops the pointer's notifications once the
  // pointer is in neither the tree nor the column.
  void update_hover();
  // The item `update_hover` finds at screen point (x, y), `kNone` for none.
  std::uint16_t item_at(int screen_x, int screen_y) const;
  // `ResWnd_compare_depth` 0x432c4 + the projection loop of the draw.
  void project_items();
  void draw_tree();
  void draw_info();
  void draw_title();
  // A button message: toggles the motion bits (`docs/11-screens.md`).
  void toggle_motion(int message, int button);

  Camera3d camera_;
  std::array<Vec3, kMaxItems> pos_{};             // +0x143
  std::array<ScreenPoint, kMaxItems> screen_{};   // +0x5f3, +0x6bb
  std::array<float, kMaxItems> depth_{};          // +0x783
  std::array<std::uint16_t, kMaxItems> order_{};  // +0x913
  std::uint16_t hovered_ = kNone;                 // +0x9db
  std::uint16_t last_hovered_ = kNone;
  // The planet items and gizmos the info column shows (y relative to the
  // column), for their help.
  struct InfoEntry {
    int y0 = 0;
    int y1 = 0;
    HelpKind help = kHelpPlanet;
    int index = 0;
  };
  std::vector<InfoEntry> info_entries_;
  bool laid_out_ = false;
  // The module's globals: redraw the title (`0xf64a0`), the info column
  // (`0xf64a4`), the tree (`0xf64a8`); the motion flags (`0xf64ac`).
  bool redraw_title_ = false;
  bool redraw_info_ = false;
  bool redraw_tree_ = false;
  std::uint32_t motion_ = 0;
};

// Adds `ResWnd` (TYPE 9) to `factory`.
void add_research_windows(WindowFactory& factory);

}  // namespace oa
