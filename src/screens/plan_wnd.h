// The planet screen (screen 17, `docs/11-screens.md` "Planet screen"):
// `PlanWnd` (`planwin.cpp`, TYPE 7 PLANETALLOC: the isometric surface, the
// orbital squares, the build list), its five top panels `PlanStatButton`
// (TYPE 29 PLRES, PLIND, PLPRO, PLPOP, PLBUILD) and the square / info dialog
// `PlSquareWnd` (`psqwin.cpp`, TYPE 14 PLSQUARE with its buttons
// PLSQBUT0..5). The planet shown is the cosmos' selected planet.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gfx/vfx_poly.h"
#include "ui/gwindow.h"
#include "ui/list_box.h"
#include "ui/winmgr.h"

namespace oa {

namespace game {
struct Planet;
struct Session;
struct Ship;
}  // namespace game

class ShipDesWnd;

// The screen geometry (`PlanWnd_draw` 0x36de0, `plan_square_at_point`
// 0x35d10).
namespace planet_geometry {

// The 68x34 diamonds: the top vertex of surface square (`row`, `column`).
constexpr PolyPoint diamond_top(int row, int column) {
  return {410 - 34 * row + 34 * column, 110 + 17 * row + 17 * column};
}
// The four vertices of the diamond under `top`: top, right, bottom, left.
constexpr std::array<PolyPoint, 4> diamond(PolyPoint top) {
  return {{{top.x, top.y}, {top.x + 34, top.y + 17}, {top.x, top.y + 34}, {top.x - 34, top.y + 17}}};
}
// The top-left corner of the 70x70 cell of orbital square `index` (0..9).
constexpr PolyPoint orbital_cell(int index) { return {13 + 70 * (index / 5), 119 + 70 * (index % 5)}; }
// `plan_screen_to_rowcol` (0x35cb0): the surface row and column under
// (x, y) (C divisions, toward zero).
struct RowColumn {
  int row = 0;
  int column = 0;
};
constexpr RowColumn screen_to_rowcol(int x, int y) {
  return {(17 * x - 34 * y - 3230) / -1156, static_cast<std::int16_t>((17 * x + 34 * y - 10710) / 1156)};
}
// Whether (x, y) falls in the orbital columns rather than on the surface.
constexpr bool in_orbit_area(int x, int y) { return x >= 14 && x <= 152 && y >= 120 && y <= 468; }

}  // namespace planet_geometry

// A shape of the screen: a `gwshare.txt` table and a frame.
struct PlanShape {
  int table = 0;
  int frame = 0;
};

class PlanWnd final : public GWindow {
 public:
  // No item selected / no square (`+0xab`, `+0xac`..`+0xb0`).
  static constexpr std::uint8_t kNoSelection = 0xff;
  static constexpr std::uint16_t kNoSquare = 0xffff;
  // The display flags (`+0x18b`, saved): coloured squares (key S, on by
  // default) and the planet picture (on, nothing toggles it).
  static constexpr std::uint8_t kColoredSquares = 1;
  static constexpr std::uint8_t kPlanetPicture = 4;

  // The screen's own events.
  enum Event : int {
    // From the panels (message 50, p1 = dialog mode 0..2, 4), a square
    // click (mode 3) and the dialog (p1 = 10 close, 11 close and open the
    // build list, 12 close and start the Colony Base placement).
    kEventDialog = 0x32,
    // PLBUILD (message 51, p1 1 = planet-wide) or the dialog's Build (p1 0,
    // the dialog's square).
    kEventBuildList = 0x33,
    // (square, refit): the ship design of a new ship or a refit.
    kEventShipDesign = 0x34,
  };

  PlanWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

  // `PlanWnd_handle_event` (0x35e98).
  bool handle_event(int type, int a, int b) override;
  // The whole screen (its left click anywhere), not a target.
  void press_areas(std::vector<PressArea>& out) const override { out.push_back(PressArea{Pane{}, false}); }
  // A long press while an item is being placed: the right button, which
  // drops it.
  LongPress long_press(int /*x*/, int /*y*/) const override {
    return selection_ != kNoSelection ? LongPress::kRight : LongPress::kHelp;
  }
  // `PlanWnd_draw` (0x36de0).
  void draw(int mode) override;
  // `PlanWnd_serialize` (0x37ef4): the display flags byte.
  bool save_state(std::span<std::uint8_t> out) const override;
  void load_state(std::span<const std::uint8_t> in) override;

  std::uint8_t selection() const { return selection_; }
  std::uint16_t hovered() const { return hovered_; }
  std::uint16_t square() const { return square_; }
  std::uint8_t flags() const { return flags_; }
  // The items of the build list, in its order (`+0xb2`).
  const std::vector<std::uint8_t>& build_items() const { return build_items_; }

  // `plan_square_at_point` (0x35d10): the square under (x, y), `kNoSquare`
  // for none.
  std::uint16_t square_at_point(int x, int y) const;
  // `plan_item_shape` (0x37cc0) / `plan_square_shape` (0x37d24): the shape
  // of `item` (a Ship: the planet owner's small ship, frame 0) and of the
  // item on `square` (the ruins frame 39 on an empty ruins square, a ship
  // its owner's small ship of its hull size).
  PlanShape item_shape(std::uint8_t item) const;
  PlanShape square_shape(std::uint16_t square) const;

 private:
  game::Session* session() const;
  game::Planet* planet() const;
  // The mouse move (event 8): the hovered square, then the redraw.
  void hover(int x, int y);
  void left_click();
  // peCustom: the screen area of the management label (empty when not
  // shown), and whether (x, y) is on it while nothing else is open.
  Pane management_pane() const;
  bool management_hot(int x, int y) const;
  // peCustom: the bottom left line "<name> <label symbol> <labels>", the
  // symbol's (and the labels') area a click on which opens the planet's
  // labels, and whether (x, y) is on it while nothing else is open.
  Pane label_pane() const;
  bool label_hot(int x, int y) const;
  // The remake greys the panels that do nothing on another species' planet
  // (all but PLPOP's planet dialog): on entering the screen and after the
  // planet may have changed hands (a colony founded, an invasion).
  void grey_panels();
  // The Colony Base placement of the selected item 5 on `square_`.
  void colonize();
  void open_dialog(int mode);
  void open_build_list(int from);
  void pick_build_item(int row);
  void ship_design(std::uint16_t square, bool refit);
  // `PlanWnd_finish_ship_design` (0x37dec).
  void finish_ship_design();
  ShipDesWnd& design_window() const;
  void key(int scan);
  void draw_surface(const game::Session& s, const game::Planet& p);
  void draw_orbit(const game::Session& s, const game::Planet& p);
  // `plan_draw_build_list_item` (0x35d8c).
  void draw_build_row(const Pane& row, const ListBox::Item& item, bool hilite);
  ListBox& list() const;
  GWindow& dialog() const;

  std::uint8_t selection_ = kNoSelection;                  // +0xab
  std::uint16_t hovered_ = kNoSquare;                      // +0xac
  std::uint16_t square_ = kNoSquare;                       // +0xae
  std::uint16_t design_square_ = kNoSquare;                // +0xb0
  std::vector<std::uint8_t> build_items_;                  // +0xb2
  std::uint8_t flags_ = kColoredSquares | kPlanetPicture;  // +0x18b
  // The ship that the input box names at the activation (`0xf4508`), with
  // its title's static string (`0xf450c`).
  game::Ship* name_ship_ = nullptr;
  int name_title_ = 0;
};

// The top panels (`planwin.cpp`, TYPE 29, ctor 0x37f1c).
class PlanStatButton final : public GWindow {
 public:
  PlanStatButton(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

  // `PlanStatButton_handle_event` (0x37f74): no input while the build list
  // or the dialog is open (only shows and hides).
  bool handle_event(int type, int a, int b) override;
  // `PlanStatButton_draw` (0x38020).
  void draw(int mode) override;
  void draw_hilite(int mode) override { draw(mode); }
};

// The square / info dialog (`psqwin.cpp`, TYPE 14, ctor 0x386b0).
class PlSquareWnd final : public GWindow {
 public:
  // The dialog's modes (`0xf4510`).
  enum Mode : int { kResearch = 0, kIndustry = 1, kProsperity = 2, kSquare = 3, kPlanet = 4 };
  // The button commands, in the order the buttons are laid out (statics
  // 57..64 by command - 0x33).
  enum Command : int {
    kCancel = 0x33,
    kCreateColony = 0x34,
    kAbandon = 0x35,
    kBuild = 0x36,
    kLeaveOrbit = 0x37,
    kRefit = 0x38,
    kInvade = 0x39,
    kAutomate = 0x3a,
  };

  PlSquareWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

  // `PlSquareWnd_handle_event` (0x3876c).
  bool handle_event(int type, int a, int b) override;
  // The whole screen (it swallows every click).
  void press_areas(std::vector<PressArea>& out) const override { out.push_back(PressArea{Pane{}, false}); }
  // `PlSquareWnd_draw` (0x38fec).
  void draw(int mode) override;
  void draw_hilite(int mode) override { draw(mode); }

  int mode() const { return mode_; }
  std::uint16_t square() const { return square_; }
  // The commands of the shown buttons, in order.
  const std::vector<int>& commands() const { return commands_; }

 private:
  // The activation's button layout.
  void layout_buttons();
  void command(int id);
  void invade();
  void draw_square(const game::Session& s, const game::Planet& p, GameFont& font);

  int mode_ = kSquare;             // 0xf4510
  std::uint16_t square_ = 0xffff;  // +0xab
  std::vector<int> commands_;
};

// Adds the screen's window classes to `factory`.
void add_planet_windows(WindowFactory& factory);

}  // namespace oa
