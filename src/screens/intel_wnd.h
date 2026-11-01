// The intelligence screen (screen 14, `docs/11-screens.md` "Intelligence"):
// `IntelWnd` (`windows.txt` TYPE 12 INTELSCREEN, ctor 0x32cb8, vtable
// 0x85d84), the title bar, over the INTELLIST list box of the player and
// every met, living species, strongest first; and the special abilities
// window `AbilityWnd` (TYPE 22 ABILITYWIN, ctor 0x32f50, vtable 0x85d6c), a
// help window over the player's ability topic (the window manager opens it
// for message 5 kind 1, `WinMgr::show_help`).
#pragma once

#include <cstdint>

#include "game/address.h"
#include "game/state.h"
#include "ui/gwindow.h"
#include "ui/help_wnd.h"
#include "ui/list_box.h"
#include "ui/winmgr.h"

namespace oa {

class IntelWnd final : public GWindow {
 public:
  // The rows' height (`+0x8c9`).
  static constexpr int kRowHeight = 60;

  IntelWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

  // `IntelWnd_handle_event` (0x32d24): 1 sets the list up (once).
  bool handle_event(int type, int a, int b) override;
  // `IntelWnd_draw` (0x32d94): the player's flag and "<species>
  // Intelligence" over the background, then the list filled again: the
  // player and the met, living species by fleet + colonies, descending
  // (`ListItem_compare_value` 0x10a14 on `-(fleet + colonies)`).
  void draw(int mode) override;
  void draw_hilite(int mode) override { draw(mode); }

  // The race of each row, in list order.
  int row_count() const { return list_ != nullptr ? list_->count() : 0; }
  game::RaceIndex row_race(int row) const;

 private:
  // `IntelWnd_draw_race_row` (0x327fc).
  void draw_row(const Pane& row, game::RaceIndex race);
  // peCustom: the race's mood toward the player (`color` the label's).
  void draw_mood(const Pane& row, const game::Race& race, std::size_t player, int color);

  ListBox* list_ = nullptr;  // +0xab INTELLIST
};

class AbilityWnd final : public HelpWnd {
 public:
  // The player's portrait and ring in the window.
  static constexpr int kPortraitX = 0xfa;
  static constexpr int kPortraitY = 0x41;

  using HelpWnd::HelpWnd;

  // `AbilityWnd_handle_event` (0x32ffc): 1 checks the charge
  // (`Race_use_special_ability(player, 0)`): ready, the countdown line is
  // hidden; else "Use Ability" is hidden, the countdown filled with the days
  // left and the other button renamed "OK". 50 with p1 1 (Use Ability) fires
  // the ability and shows its result topic `abres%02d`.
  bool handle_event(int type, int a, int b) override;
  // `AbilityWnd_draw` (0x33188): the help window, then the player's
  // portrait in its ring.
  void draw(int mode) override;
};

// Adds `IntelWnd` (TYPE 12) and `AbilityWnd` (TYPE 22) to `factory`.
void add_intel_windows(WindowFactory& factory);

}  // namespace oa
