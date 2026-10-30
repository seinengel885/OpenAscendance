#include "assets/fnt.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/byte_reader.h"

namespace oa {

namespace {

// A glyph to change: its pixels by row.
struct Glyph {
  int width = 0;
  std::vector<std::vector<std::uint8_t>> rows;
};

Glyph glyph(const Font& font, char c) {
  const auto code = static_cast<std::uint8_t>(c);
  Glyph g{.width = font.widths[code], .rows = {}};
  for (int y = 0; y < font.height; ++y) {
    const auto row = font.glyphs[code].begin() + static_cast<std::ptrdiff_t>(y) * g.width;
    g.rows.emplace_back(row, row + g.width);
  }
  return g;
}

void set_glyph(Font& font, std::uint8_t code, const Glyph& g) {
  font.widths[code] = g.width;
  font.glyphs[code].clear();
  for (const std::vector<std::uint8_t>& row : g.rows) {
    font.glyphs[code].insert(font.glyphs[code].end(), row.begin(), row.end());
  }
}

// Returns the most frequent pixel of `g` that is not `background`.
std::uint8_t ink(const Glyph& g, std::uint32_t background) {
  std::map<std::uint8_t, int> count;
  for (const auto& row : g.rows) {
    for (const std::uint8_t p : row) {
      if (p != background) {
        ++count[p];
      }
    }
  }
  if (count.empty()) {
    return static_cast<std::uint8_t>(background);
  }
  return std::ranges::max_element(count, {}, [](const auto& e) { return e.second; })->first;
}

bool blank_row(const std::vector<std::uint8_t>& row, std::uint32_t background) {
  return std::ranges::all_of(row, [&](std::uint8_t p) { return p == background; });
}

// Returns `base` with two dots above: dots `size` square, `size` rows over
// the letter (`base` itself when it is too small for them). Without that room, rows repeating the one above them (the
// straight parts of the strokes) are left out, else rows from the middle.
Glyph with_dots(const Glyph& base, std::uint32_t background) {
  const int height = static_cast<int>(base.rows.size());
  const int size = std::max(1, height / 12);
  int left = base.width;
  int right = -1;
  for (const auto& row : base.rows) {
    for (int x = 0; x < base.width; ++x) {
      if (row[static_cast<std::size_t>(x)] != background) {
        left = std::min(left, x);
        right = std::max(right, x);
      }
    }
  }
  const int span = right - left + 1;
  // A letter too small for the dots stays without them.
  if (span < 2 * size + 1 || height < 2 * size + 2) {
    return base;
  }
  Glyph g = base;
  int top = 0;
  while (top < height && blank_row(g.rows[static_cast<std::size_t>(top)], background)) {
    ++top;
  }
  for (int missing = 2 * size - top; missing > 0; --missing) {
    std::size_t drop = 0;
    for (std::size_t y = static_cast<std::size_t>(top) + 1; y < g.rows.size() && drop == 0; ++y) {
      if (g.rows[y] == g.rows[y - 1]) {
        drop = y;
      }
    }
    if (drop == 0) {
      drop = g.rows.size() / 2;
    }
    g.rows.erase(g.rows.begin() + static_cast<std::ptrdiff_t>(drop));
    g.rows.insert(g.rows.begin(), std::vector<std::uint8_t>(static_cast<std::size_t>(g.width), static_cast<std::uint8_t>(background)));
    ++top;
  }
  const int gap = std::max(1, (span + 2) / 5);
  const int x0 = left + (span - 2 * size - gap) / 2;
  const std::uint8_t color = ink(base, background);
  for (int y = top - 2 * size; y < top - size; ++y) {
    for (int i = 0; i < size; ++i) {
      g.rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x0 + i)] = color;
      g.rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x0 + size + gap + i)] = color;
    }
  }
  return g;
}

// `ß` of the UI fonts (`smfont.fnt`, `bigfont.fnt`), by height; '3' ink.
constexpr std::array<std::string_view, 8> kSharpS8{
    "..33..", ".3..3.", ".3.3..", ".3..3.", ".3...3", ".3...3", ".3.33.", "......",
};
constexpr std::array<std::string_view, 11> kSharpS11{
    "..333.", ".3...3", ".3...3", ".3..3.", ".3.3..", ".3..3.",
    ".3...3", ".3...3", ".3...3", ".3.33.", "......",
};

Glyph sharp_s(const Font& font) {
  const Glyph s = glyph(font, 's');
  std::span<const std::string_view> pattern;
  if (font.height == 8) {
    pattern = kSharpS8;
  } else if (font.height == 11) {
    pattern = kSharpS11;
  }
  if (pattern.empty()) {
    Glyph ss = s;
    for (std::size_t y = 0; y < ss.rows.size(); ++y) {
      ss.rows[y].insert(ss.rows[y].end(), s.rows[y].begin(), s.rows[y].end());
    }
    ss.width *= 2;
    return ss;
  }
  const std::uint8_t color = ink(s, font.background);
  Glyph g{.width = static_cast<int>(pattern[0].size()), .rows = {}};
  for (const std::string_view line : pattern) {
    auto& row = g.rows.emplace_back();
    for (const char c : line) {
      row.push_back(c == '.' ? static_cast<std::uint8_t>(font.background) : color);
    }
  }
  return g;
}

}  // namespace

Font parse_font(std::span<const std::uint8_t> data, std::string_view context) {
  ByteReader r(data, context);
  const std::string version = r.fixed_string(4);
  if (!version.starts_with("1.")) {
    r.fail(std::format("not a VFX font (version '{}')", version));
  }
  const std::uint32_t count = r.u32();
  const std::uint32_t height = r.u32();
  Font font;
  font.background = r.u32();
  if (count > 256 || height > 256) {
    r.fail(std::format("bad font header ({} chars, height {})", count, height));
  }
  font.height = static_cast<int>(height);
  std::vector<std::uint32_t> offsets(count);
  for (std::uint32_t& o : offsets) {
    o = r.u32();
  }
  for (const std::uint32_t off : offsets) {
    r.seek(off);
    const std::uint32_t w = r.u32();
    if (w > 1024) {
      r.fail(std::format("glyph width {}", w));
    }
    const auto pixels = r.bytes(std::size_t{w} * height);
    font.widths.push_back(static_cast<int>(w));
    font.glyphs.emplace_back(pixels.begin(), pixels.end());
  }
  return font;
}

void add_german_letters(Font& font) {
  if (font.widths.size() < 128) {
    return;
  }
  font.widths.resize(std::max<std::size_t>(font.widths.size(), kCodeSharpS + 1));
  font.glyphs.resize(font.widths.size());
  for (const auto& [code, base] : {std::pair{kCodeAUmlaut, 'A'}, std::pair{kCodeOUmlaut, 'O'},
                                   std::pair{kCodeUUmlaut, 'U'}, std::pair{kCodeAUmlautSmall, 'a'},
                                   std::pair{kCodeOUmlautSmall, 'o'}, std::pair{kCodeUUmlautSmall, 'u'}}) {
    set_glyph(font, code, with_dots(glyph(font, base), font.background));
  }
  set_glyph(font, kCodeSharpS, sharp_s(font));
}

}  // namespace oa
