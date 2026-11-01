// The generic window (`gwindow.cpp`, class `GWindow`, ctor 0x2bf50, init
// 0x2c004, vtable 0x85ca0): buttons and static panes from `windows.txt`, the
// base of every window class (`docs/07-ui-framework.md`, "Window classes").
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "assets/shp.h"
#include "assets/windows_txt.h"
#include "gfx/vfx.h"
#include "platform/input_event.h"

namespace oa {

class WinMgr;

// Window event types (`docs/07-ui-framework.md`, "Event types").
enum WindowEvent : int {
  kEventActivate = 1,
  kEventDeactivate = 2,
  kEventKey = 3,
  kEventLeftClick = 4,
  kEventRightClick = 5,
  kEventMouseLeave = 6,
  kEventMouseEnter = 7,
  kEventMouseMove = 8,
  kEventSetControl = 9,  // ButtonGroup: a control id (-1 all), its flags
  kEventTick = 10,
  kEventHelpClick = 0xc,
  kEventRefresh = 0xd,
  kEventShow = 0xe,
  kEventHide = 0xf,
  kEventModalOpen = 200,
  kEventOk = 201,
  kEventCancel = 202,
  // Control messages between windows.
  kEventHelpButton = 50,   // the help window's buttons
  kEventScrollUp = 0x191,  // list box arrows
  kEventScrollDown = 0x192,
  kEventScrollStop = 0x193,
  kEventListPick = 0x1c01,  // a list row picked: (row, 4/5) to the parent
  kEventListHelp = 0x1c02,  // a list row clicked in help mode: (row, 0xc)
};

// A rectangle where a shown window takes the presses (the left, right and
// help clicks) that the original delivers to it (`GWindow::press_areas`).
struct PressArea {
  Pane pane;
  // Whether a press there acts (a button, a list's rows, a control), as
  // opposed to one the window takes without acting (a background, a view's
  // empty space, a dialog swallowing every click).
  bool target = false;
};

// The `draw` modes: 0 normal, 2 not hot, 3 hot.
enum DrawMode : int { kDrawNormal = 0, kDrawNotHot = 2, kDrawHot = 3 };

// The value of SENDMESSAGE / SENDPARAM1 / SENDPARAM2 replaced by the event
// type (4 left, 5 right).
inline constexpr int kParamMouseButton = 999;
// SENDMESSAGE of a child window posting (p1, p2) to the window manager
// instead of sending it to its parent.
inline constexpr int kMessageToWinMgr = 998;

class GWindow {
 public:
  // The colours of a text window's background: normal and hot (0xf2 and
  // 0x96, also the WMGR_MAPFOCUS remap).
  static constexpr std::uint8_t kBackColor = 0xf2;
  static constexpr std::uint8_t kHotBackColor = 0x96;
  // The most children of a window ("CHILD WINDOWS", 20 pointers).
  static constexpr std::size_t kMaxChildren = 20;

  // `GWindow_init` (0x2c004) from `record`: the shape table is the one of the
  // record's first screen (`shapes`, may be null for none).
  GWindow(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes);
  // A window made by the code (dialogs): `GWindow_init`'s defaults, `name`
  // (the caption) and the rectangle `pane`.
  GWindow(WinMgr& wm, std::string name, const Pane& pane);
  virtual ~GWindow() = default;
  GWindow(const GWindow&) = delete;
  GWindow& operator=(const GWindow&) = delete;

  // vtable slot 1, `GWindow_move` (0x2c198).
  virtual void move(int dx, int dy);
  // vtable slot 2, `GWindow_handle_event` (0x2eb40): returns true when the
  // event is consumed (-1 in the original).
  virtual bool handle_event(int type, int a, int b);
  // vtable slot 3, `GWindow_draw` (0x2c444).
  virtual void draw(int mode);
  // vtable slot 4, `GWindow_draw_hilite` (0x2c648): the hot look.
  virtual void draw_hilite(int mode);
  // vtable slot 5, `GWindow_serialize` (0x2cb80, nothing for the base): the
  // window's part of the saves' window state, `WinMgr::state_size` bytes
  // (`WinMgr_serialize_windows` 0x556a4). `save_state` fills `out` and
  // returns true, or returns false when the class leaves its part to the
  // window manager (the bytes saved before are kept); `load_state` reads it
  // back after a load.
  virtual bool save_state(std::span<std::uint8_t> /*out*/) const { return false; }
  virtual void load_state(std::span<const std::uint8_t> /*in*/) {}
  // The reset of the window's state by "Begin New Game"
  // (`NewGameWnd_handle_event` 0x4dd5c): nothing for most windows.
  virtual void new_game() {}

  // The remake's view gestures (`ViewGesture`) and mouse wheel, not in the
  // original: whether a drag or pinch starting at screen point (x, y) turns,
  // zooms or scrolls this window's view (none for most windows; `mouse`: a
  // drag of the mouse, whose press already was the original's click), then
  // the drags (in screen pixels) and pinches (the ratio of the fingers'
  // distances, above 1 apart) of that gesture. The wheel's notches (positive
  // away from the player) go to the window that would take a finger's
  // gesture at the pointer.
  virtual bool takes_view_gesture(int /*x*/, int /*y*/, bool /*mouse*/) const { return false; }
  virtual void view_drag(int /*dx*/, int /*dy*/) {}
  virtual void view_pinch(float /*ratio*/) {}
  virtual void view_wheel(int /*notches*/) {}
  // The end of the gesture this window took.
  virtual void view_end() {}
  // The zoom of one wheel notch on the 3D views, as a pinch's ratio.
  static constexpr float kWheelZoom = 1.1F;
  // Whether the window plays an animation that Space skips (a remake
  // addition: a finger put down then skips it, `WinMgr::skippable_animation`).
  virtual bool plays_skippable_animation() const { return false; }

  // The remake's enlarged click targets (`WinMgr`, not in the original):
  // appends to `out` the rectangles where this window, while shown, takes
  // the presses, its click targets marked. The default: the window's
  // rectangle, a target when a click sends a message. A class taking
  // presses elsewhere (outside its rectangle, everywhere) or whose parts
  // are the targets overrides it.
  virtual void press_areas(std::vector<PressArea>& out) const;

  // What a finger's long press at (x, y) on the window is (a remake
  // addition, `WinMgr::long_press`). The default: the parent's
  // `child_long_press`, a help click without a parent.
  virtual LongPress long_press(int x, int y) const;
  // What a long press at (x, y) on the child window `child` is: a help
  // click by default.
  virtual LongPress child_long_press(const GWindow& /*child*/, int /*x*/, int /*y*/) const { return LongPress::kHelp; }
  // Esc or Back on the window's screen (a remake addition,
  // `WinMgr::cancel_action`): cancels the window's action in progress;
  // returns whether there was one (the screen then stays).
  virtual bool cancel_action() { return false; }
  // The remake's greyed look of a button whose action is unavailable (not
  // in the original; never while a trace is replayed): the window's own
  // `draw` shows it with `grey_out`, the left and right clicks do nothing,
  // the help click still works. `set_greyed` redraws a shown window whose
  // look changes.
  bool greyed() const;
  void set_greyed(bool greyed);

  // The `windows.txt` TYPE (-1 for a window made by the code).
  int type() const { return type_; }
  int id() const { return id_; }
  void set_id(int id) { id_ = id; }
  const std::string& name() const { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }
  const Pane& pane() const { return pane_; }
  void set_pane(const Pane& pane) { pane_ = pane; }
  bool shown() const { return shown_; }
  bool active() const { return active_; }
  // Sets both the shown (`+0x35`) and active (`+0x39`) flags, as windows
  // enabling or disabling their controls do (no event, no draw).
  void set_enabled(bool on) {
    shown_ = on;
    active_ = on;
  }
  bool mouse_focus() const { return mouse_focus_; }
  void set_mouse_focus(bool focus) { mouse_focus_ = focus; }
  int parent() const { return parent_; }
  void set_parent(int id) { parent_ = id; }
  const std::vector<GWindow*>& children() const { return children_; }
  // `GWindow_add_child` (0x2c0b0) / `GWindow_remove_child` (0x2c12c).
  void add_child(GWindow* child);
  void remove_child(GWindow* child);
  const std::string& help() const { return help_; }
  void set_help(std::string help) { help_ = std::move(help); }
  int message() const { return message_; }
  // SENDMESSAGE, SENDPARAM1, SENDPARAM2.
  void set_message(int message, int p1, int p2);
  // Sets the frame of WINITEMS entry `index` (a SHAPEITEM's frame, a
  // TEXTITEM's font flags); nothing when there is no such item.
  void set_item_frame(std::size_t index, int frame) {
    if (index < items_.size()) {
      items_[index].flags = frame;
    }
  }
  // The WINITEMS entries (the remake's opening menu moves them).
  const std::vector<WindowItem>& items() const { return items_; }
  void set_items(std::vector<WindowItem> items) { items_ = std::move(items); }
  int frame() const { return frame_; }
  void set_frame(int frame) { frame_ = frame; }
  const ShapeTable* shape_table() const { return shapes_; }
  void set_shape_table(const ShapeTable* shapes) { shapes_ = shapes; }
  // The caption's position in the pane (-1: centred on that axis) and the
  // border (`+0x5b`, `+0x5d`, `+0x5f`).
  void set_text_pos(int x, int y) {
    text_x_ = x;
    text_y_ = y;
  }
  void set_border(bool border) { border_ = border; }
  // Whether (x, y) is inside the window's rectangle.
  bool contains(int x, int y) const { return pane_.contains(x, y); }

 protected:
  // Sends `type` to the children (`GWindow_broadcast_event` 0x2c978).
  void broadcast_to_children(int type, int a, int b);
  // The click path of `handle_event` (4, 5 inside the rectangle): the sound,
  // then the message to the window manager or the parent.
  void send_click(int type);
  // `GWindow_draw_items` (0x2c814).
  void draw_items(bool hot);
  // `GWindow_draw_children` (0x2c938).
  void draw_children();
  // The text path of `draw` / `draw_hilite`, on `background`.
  void draw_text(std::uint8_t background);
  // Draws the 4-line border in `color` (the text windows' `+0x5f`).
  void draw_border(std::uint8_t color);
  // The window's shape (`shapes_` frame `frame`), null when it has none.
  const Shape* shape(int frame) const;
  // Turns the pane's colours other than `background` into greys faded
  // toward it (the greyed look).
  void grey_out(std::uint8_t background);

  WinMgr& wm_;
  int type_ = -1;
  Pane pane_;
  const ShapeTable* shapes_;
  int frame_ = -1;
  std::string name_;
  bool shown_ = false;
  bool active_ = false;
  bool mouse_focus_ = false;
  int id_ = 0;
  int message_ = 0;
  int param1_ = 0;
  int param2_ = 0;
  std::string help_ = "0";
  int sound_ = -1;
  int text_x_ = -1;
  int text_y_ = -1;
  bool border_ = false;
  bool greyed_ = false;
  int parent_ = 0;  // 0: none
  std::vector<GWindow*> children_;
  std::vector<WindowItem> items_;
};

}  // namespace oa
