// The end-of-game screen (screen 9, `eventwnd.cpp`, class `EndGameWnd`,
// `windows.txt` TYPE 24 ENDGAME, ctor 0x27318, vtable 0x85ac0): entered
// (message 1, pushed) when `Cosmos_check_game_over` ends the game. It shows
// the ending's text (`end-xtnc.txt`, `end-dest.txt`, `end-capt.txt`,
// `end-unit.txt`, `end-ctrl.txt` by `Cosmos::ending` 1..5), the rank
// picture, the score and its title, the player's portrait, the species met
// with their relations, and the player's ships, planets and discoveries as
// small scattered pictures (`docs/07-ui-framework.md`, "Screens").
//
// Nothing on it leaves by itself: Esc or an exit corner goes back to the
// star map (the screen history), where the game goes on.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "assets/translation.h"
#include "ui/gwindow.h"
#include "ui/winmgr.h"

namespace oa {

// The texts of an ending file for a rank, converted as drawn (line ends and
// `_` as spaces, `@` as line breaks).
struct EndingText {
  std::string description;      // ENDDESC
  std::string congratulations;  // CONGRAT
  std::string rank;             // line `rank` (0..9) of RANKS
};

// Reads the sections ENDDESC, CONGRAT and RANKS of an ending file `data`
// (named `name`) up to its `END` line (`EndGameWnd_draw` 0x27438), each
// section and the rank translated as read (with their line ends,
// `translation` of `name`). Throws `DataError` when it ends before `END`.
EndingText parse_ending(std::span<const std::uint8_t> data, int rank, std::string_view name,
                        const Translation& translation = Translation::none());

class EndGameWnd final : public GWindow {
 public:
  // The tunes: `victory`, and the first species theme (ending 1, the
  // player extinct, plays the player's).
  static constexpr int kVictoryTune = 5;
  static constexpr int kFirstSpeciesTheme = 8;

  EndGameWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

  // `EndGameWnd_handle_event` (0x27370): on activation the palette fades to
  // black (64 steps), the window manager's fade back in is armed, the tune
  // starts, then the activation (the window drawn).
  bool handle_event(int type, int a, int b) override;
  // `EndGameWnd_draw` (0x27438): the ending's screen over the background.
  // Each of the player's ships and planets is placed at random (2 `rand()`
  // of the game's generator each), so every draw changes the game's
  // random sequence.
  void draw(int mode) override;

  // The rank (0..9) of a score: score / 10 - 1, clamped.
  static int rank_of(int score);
};

// Adds `EndGameWnd` (TYPE 24) to `factory`.
void add_end_game_windows(WindowFactory& factory);

}  // namespace oa
