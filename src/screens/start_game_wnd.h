// The species history at the start of a game (`StartGameWnd`, `windows.txt`
// TYPE 30, ctor 0x2684c, vtable 0x85ad8: screen 12): the player's species
// title, its large portrait, the introduction (with the paragraph of the
// atmosphere) and the history text of `history.txt`, over the species'
// theme.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "assets/translation.h"
#include "ui/winmgr.h"

namespace oa {

// A species' page of `history.txt`.
struct SpeciesHistory {
  std::string title;  // "<name>: " and the POWER lines (line ends kept)
  std::string intro;  // INTRO, then PEACE / NEUTRAL / HOSTILE by atmosphere
  std::string text;   // TEXT
};

// Reads the page of `species` (SPECNUM) from `history.txt` as
// `StartGameWnd_draw` (0x2691c) does: comment lines skipped, the lines of a
// section joined with their line ends, the paragraph of `atmosphere` (0
// PEACE, 1 NEUTRAL, 2 HOSTILE) appended to the introduction; in the
// introduction and the text a line end or `_` becomes a space and `@` a
// line break. `name` starts the title. Each section is translated as read
// (its lines with their line ends, `translation` of `history.txt`). Throws
// `DataError` when the species is missing.
SpeciesHistory parse_species_history(std::span<const std::uint8_t> data, int species, int atmosphere,
                                     std::string_view name, const Translation& translation = Translation::none());

class StartGameWnd final : public GWindow {
 public:
  StartGameWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

  // `StartGameWnd_handle_event` (0x268a4): 1 plays the species' theme (tune
  // 8 + species); 2 posts the star map (message 1, screen 1, pushing).
  bool handle_event(int type, int a, int b) override;
  // `StartGameWnd_draw` (0x2691c).
  void draw(int mode) override;
  void draw_hilite(int mode) override { draw(mode); }
};

// Adds the class of this screen to `factory`.
void add_start_game_windows(WindowFactory& factory);

}  // namespace oa
