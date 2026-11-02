// The planet status list (screen 10, `status.cpp`, class `StatusWnd`,
// `windows.txt` TYPE 16 STATUSWND, ctor 0x4ca84, vtable 0x85fa8): the
// player's planets in the STATPLANLST list box, oldest colony first, each
// row with its star and planet pictures, names, project and outputs; a click
// opens the star system (left part of a row) or the planet screen
// (`docs/11-screens.md`, "Planet status list").
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

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
  // over the background, then the list. peCustom: "<species> (N)" and the
  // buttons of the planets' marks and filters (`PeButton`).
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
  // peCustom: the title bar's buttons, their captions and panes (screen),
  // and a click on one.
  enum PeButton : int { kAll, kSameStrategy, kSize, kLabel, kBell, kManagement, kPeButtons };
  bool pe_custom() const;
  std::vector<std::pair<std::string, Pane>> pe_buttons() const;
  void pe_click(int button);
  // The marked planets shown, all those shown when none is.
  std::vector<game::Planet*> marked_or_shown() const;
  // `StatusWnd_draw_planet_item` (0x4b940).
  void draw_row(const Pane& row, game::PlanetIndex planet, bool hilite);

  ListBox* list_ = nullptr;  // +0xab
  int top_ = 0;              // +0xaf, the list's top row kept between visits
  // peCustom: where each planet's management label was last drawn (a click
  // there opens its menu).
  std::map<game::PlanetIndex, Pane> management_areas_;
  // peCustom: where each row's mark box and label symbol were drawn, the
  // marked planets and the filters (size, label: -1 all; only the planets
  // with a bell).
  std::map<game::PlanetIndex, Pane> mark_areas_;
  std::map<game::PlanetIndex, Pane> label_areas_;
  std::set<game::PlanetIndex> marked_;
  int size_filter_ = -1;
  int label_filter_ = -1;
  bool bell_filter_ = false;
  // The title bar's background, kept at the first draw after activation.
  std::vector<std::uint8_t> title_bar_;
};

// Adds `StatusWnd` (TYPE 16) to `factory`.
void add_status_windows(WindowFactory& factory);

}  // namespace oa
