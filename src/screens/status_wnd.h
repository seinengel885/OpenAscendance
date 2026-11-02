// The planet status list (screen 10, `status.cpp`, class `StatusWnd`,
// `windows.txt` TYPE 16 STATUSWND, ctor 0x4ca84, vtable 0x85fa8): the
// player's planets in the STATPLANLST list box, oldest colony first, each
// row with its star and planet pictures, names, project and outputs; a click
// opens the star system (left part of a row) or the planet screen
// (`docs/11-screens.md`, "Planet status list").
#pragma once

#include <cstdint>
#include <map>

#include "game/state.h"
#include "ui/gwindow.h"
#include "ui/list_box.h"
#include "ui/winmgr.h"

namespace oa {

class StatusWnd final : public GWindow {
 public:
  // The rows' height (`ListBox_add_item` height 0x96) and the list's line
  // height (`+0x8c9`, which only limits scrolling down).
  static constexpr int kRowHeight = 0x96;
  static constexpr int kLineHeight = 0x8d;
  // A click left of this screen x opens the star system, else the planet;
  // the hovered row's star (left) or planet (right) name is highlighted.
  static constexpr int kStarClickLimit = 0x97;
  static constexpr int kStarHiliteLimit = 0x96;

  StatusWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

  // `StatusWnd_handle_event` (0x4cca4): 1 sets the list up and fills it
  // with the player's planets sorted by `acquired_day` (Watcom `qsort`,
  // ties in its order), scrolled to the row kept at the last deactivation
  // (2); 0x1c01 (a row picked) selects the planet and pushes screen 17, or
  // selects its star and pushes screen 16 when the pointer is left of
  // `kStarClickLimit`. peCustom: a click on a row's management label opens
  // its menu instead.
  bool handle_event(int type, int a, int b) override;
  // `StatusWnd_draw` (0x4caf0): the title "<species>  (N Planets
  // Occupied, M Systems Controlled)" in the player's colour with its flag,
  // over the background, then the list.
  void draw(int mode) override;
  void draw_hilite(int mode) override { draw(mode); }

  // The planet of each row, in list order.
  int row_count() const { return list_ != nullptr ? list_->count() : 0; }
  game::PlanetIndex row_planet(int row) const { return static_cast<game::PlanetIndex>(list_->item(row).data); }

  // The systems the player controls as the title counts them: the stars
  // whose owner mask is the player's bit alone (the home star excluded),
  // plus the home star while the player has a colony there.
  static int controlled_systems(const game::Cosmos& cosmos, std::uint8_t player);

 private:
  void fill();
  // `StatusWnd_draw_planet_item` (0x4b940).
  void draw_row(const Pane& row, game::PlanetIndex planet, bool hilite);

  ListBox* list_ = nullptr;  // +0xab
  int top_ = 0;              // +0xaf, the list's top row kept between visits
  // peCustom: where each planet's management label was last drawn (a click
  // there opens its menu).
  std::map<game::PlanetIndex, Pane> management_areas_;
};

// Adds `StatusWnd` (TYPE 16) to `factory`.
void add_status_windows(WindowFactory& factory);

}  // namespace oa
