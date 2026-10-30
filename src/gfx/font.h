// The game's font objects (`font.cpp`, 0x2a9e0): a VFX font with its
// "FONT TRANSLATE" table and the pane it draws into, the string drawers with
// the `|nnn|` colour escapes, centring and word wrapping
// (`docs/02-graphics-formats.md`, "Fonts").
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "assets/fnt.h"
#include "assets/pal.h"
#include "core/image.h"
#include "gfx/vfx.h"

namespace oa {

// The colour index that the translate table remaps: the glyph pixels of the
// UI fonts, white in `game.pal`.
inline constexpr std::uint8_t kTextColor = 0xf3;

// `font_draw_wrapped` / `font_draw_string` flags.
enum FontFlags : int {
  kFontCenterV = 0x01,  // centred vertically in the pane
  kFontCenterH = 0x02,  // centred horizontally in the pane
  kFontAboveV = 0x04,   // centred vertically on y
  kFontAboveH = 0x08,   // centred horizontally on x
  kFontBottom = 0x10,   // the text ends at y
  kFontRight = 0x20,    // each line ends at x
  kFontWipe = 0x80,     // wipe the pane with the background first
};

// The colour argument meaning "keep the translate table" (`-2`).
inline constexpr int kKeepColor = -2;
// The colour / background argument meaning "the default" (`-1`).
inline constexpr int kDefault = -1;

// A font object (0x20 bytes in the original: data, translate table, PANE,
// default background).
class GameFont {
 public:
  // `font_init` (0x2aa1c): an identity translate table, the full screen pane
  // and background `background` (0xff = none). The remake adds the German
  // letters (`add_german_letters`).
  explicit GameFont(Font font, std::uint8_t background = 0xff);

  const Font& font() const { return font_; }
  HazeTable& translate() { return translate_; }
  Pane& pane() { return pane_; }
  const Pane& pane() const { return pane_; }
  std::uint8_t background() const { return background_; }
  void set_background(std::uint8_t background) { background_ = background; }

  // `font_next_segment` (0x2ab14): splits the next segment off `text` (from
  // `pos`) into `out` and returns the number of characters consumed (0 when
  // the rest is copied whole). With `apply_colors`, a `|nnn|` escape sets the
  // translate table's text colour to `nnn`.
  std::size_t next_segment(std::string_view text, std::size_t pos, std::string& out, bool apply_colors);

  // 0x2ac14: the width of `text`, escapes left out.
  int string_width(std::string_view text);

  // `font_draw_string` (0x2ad30): draws one line at (x, y) of the pane in
  // `color` (`kDefault` = 0xf3, `kKeepColor` keeps the table), on
  // `background` (`kDefault` = the font's; 0xff = none) wiped under it.
  void draw_string(Image8& window, int x, int y, std::string_view text, int flags, int color, int background);

  // `font_draw_wrapped` (0x2afc8): draws `text` word-wrapped to `width` (the
  // pane's when < 1), lines `height + 2` apart, placed after `flags`;
  // returns the text's height.
  int draw_wrapped(Image8& window, int x, int y, std::string_view text, int flags, int color, int background,
                   int width);

 private:
  Font font_;
  HazeTable translate_{};
  Pane pane_;
  std::uint8_t background_;
};

}  // namespace oa
