// Miles VFX bitmap fonts (`.fnt`), `docs/02-graphics-formats.md`.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace oa {

// A decoded VFX font. Layout (little-endian): "1.\0\0", `u32 char_count`,
// `u32 char_height`, `u32 font_background` (pixels of that value are not
// drawn), `u32` glyph offsets from the file start, each glyph `u32 width`
// then `width * height` row-major pixels.
struct Font {
  int height = 0;
  std::uint32_t background = 0;
  std::vector<int> widths;                        // per character code
  std::vector<std::vector<std::uint8_t>> glyphs;  // per character code

  // Returns the width of character `c` (0 for codes beyond the font), as
  // `vfx_character_width` (0x5ba63).
  int char_width(std::uint8_t c) const { return c < widths.size() ? widths[c] : 0; }
};

// Returns the font in `data`; throws `DataError` naming `context` when it is
// malformed.
Font parse_font(std::span<const std::uint8_t> data, std::string_view context = "font");

// The cp437 codes of the German letters, which the original's fonts (128
// characters) lack.
inline constexpr std::uint8_t kCodeAUmlaut = 0x8e, kCodeOUmlaut = 0x99, kCodeUUmlaut = 0x9a;
inline constexpr std::uint8_t kCodeAUmlautSmall = 0x84, kCodeOUmlautSmall = 0x94, kCodeUUmlautSmall = 0x81;
inline constexpr std::uint8_t kCodeSharpS = 0xe1;

// Adds the German letters to `font` (the remake's own glyphs, for the
// translations): the vowels with two dots above (a capital squeezed when the
// font has no room above it), `ß` drawn for the UI fonts' heights and `ss`
// for others. A font without the ASCII letters is left alone.
void add_german_letters(Font& font);

}  // namespace oa
