#include "gfx/font.h"

#include <cstdlib>
#include <vector>

namespace oa {

GameFont::GameFont(Font font, std::uint8_t background) : font_(std::move(font)), background_(background) {
  add_german_letters(font_);
  for (std::size_t i = 0; i < translate_.size(); ++i) {
    translate_[i] = static_cast<std::uint8_t>(i);
  }
}

std::size_t GameFont::next_segment(std::string_view text, std::size_t pos, std::string& out, bool apply_colors) {
  const std::string_view src = text.substr(pos);
  const std::size_t bar = src.find('|');
  if (bar == std::string_view::npos) {
    out = src;
    return 0;
  }
  if (bar > 0) {
    out = src.substr(0, bar);
    return bar;
  }
  out.clear();
  const std::size_t close = src.find('|', 1);
  if (close == std::string_view::npos) {
    return 1;  // a lone '|' is skipped
  }
  if (close == 1) {
    out = "|";
    return 2;
  }
  if (apply_colors) {
    // atoi of the (at most) 3 characters after the '|'.
    const std::string digits(src.substr(1, std::min<std::size_t>(3, close - 1)));
    translate_[kTextColor] = static_cast<std::uint8_t>(std::atoi(digits.c_str()));
  }
  return close + 1;
}

int GameFont::string_width(std::string_view text) {
  int w = 0;
  std::string seg;
  std::size_t pos = 0;
  for (;;) {
    const std::size_t n = next_segment(text, pos, seg, false);
    for (const char c : seg) {
      w += font_.char_width(static_cast<std::uint8_t>(c));
    }
    if (n == 0) {
      return w;
    }
    pos += n;
  }
}

void GameFont::draw_string(Image8& window, int x, int y, std::string_view text, int /*flags*/, int color,
                           int background) {
  const int bg = background == kDefault ? background_ : background;
  if (color == kDefault) {
    color = kTextColor;
  }
  if (color != kKeepColor) {
    translate_[kTextColor] = static_cast<std::uint8_t>(color);
  }
  Pane r = pane_;
  r.x0 += x;
  r.y0 += y;
  r.x1 = r.x0 + string_width(text);
  r.y1 = r.y0 + font_.height;
  if (r.x0 > pane_.x1 || r.x1 < pane_.x0 || r.y0 > pane_.y1 || r.y1 < pane_.y0) {
    return;
  }
  // The original clamps only one side of each axis.
  if (pane_.x0 > r.x0) {
    r.x0 = pane_.x0;
  } else if (r.x1 > pane_.x1) {
    r.x1 = pane_.x1;
  }
  if (pane_.y0 > r.y0) {
    r.y0 = pane_.y0;
  } else if (r.y1 > pane_.y1) {
    r.y1 = pane_.y1;
  }
  if (bg != 0xff) {
    vfx_pane_wipe(window, r, static_cast<std::uint8_t>(bg));
  }
  int cx = x;
  std::string seg;
  std::size_t pos = 0;
  for (;;) {
    const std::size_t n = next_segment(text, pos, seg, true);
    cx += vfx_string_draw(window, pane_, cx, y, font_, seg, translate_);
    if (n == 0) {
      return;
    }
    pos += n;
  }
}

int GameFont::draw_wrapped(Image8& window, int x, int y, std::string_view text, int flags, int color, int background,
                           int width) {
  if ((flags & kFontWipe) != 0 && background != 0xff) {
    vfx_pane_wipe(window, pane_, static_cast<std::uint8_t>(background == kDefault ? background_ : background));
  }
  translate_[kTextColor] = static_cast<std::uint8_t>(color == kDefault ? kTextColor : color);
  if (width < 1) {
    width = pane_.x1 - pane_.x0;
  }
  // The wrap, on a copy split at line ends and at the last space that fits.
  std::string buf(text);
  std::vector<std::size_t> starts{0};
  std::size_t ls = 0;
  std::size_t p = 0;
  std::size_t sp = 0;
  bool done = false;
  while (!done) {
    done = p >= buf.size() || buf[p] == '\0';
    const int w = string_width(std::string_view(buf).substr(ls, p - ls));
    const char c = p < buf.size() ? buf[p] : '\0';
    if (c == '\n') {
      buf[p] = '\0';
      ++p;
      ls = p;
      starts.push_back(ls);
    } else if (w > width && sp > ls) {
      buf[sp] = '\0';
      ls = p = sp + 1;
      starts.push_back(ls);
    } else {
      if (c == ' ') {
        sp = p;
      }
      ++p;
    }
  }
  const int lines = static_cast<int>(starts.size());
  const int lh = font_.height + 2;
  const int h = lh * lines;
  int y0 = y;
  if ((flags & kFontBottom) != 0) {
    y0 = y - h;
  } else if ((flags & kFontCenterV) != 0) {
    y0 = ((pane_.y1 - pane_.y0) - (h + 1)) / 2;
  } else if ((flags & kFontAboveV) != 0) {
    y0 = y - (h + 1) / 2;
  }
  for (const std::size_t start : starts) {
    const std::size_t end = buf.find('\0', start);
    const std::string_view line =
        std::string_view(buf).substr(start, end == std::string::npos ? std::string::npos : end - start);
    const int w = string_width(line);
    int lx = x;
    if ((flags & kFontRight) != 0) {
      lx = x - w;
    } else if ((flags & kFontCenterH) != 0) {
      lx = ((pane_.x1 - pane_.x0) - w) / 2;
    } else if ((flags & kFontAboveH) != 0) {
      lx = x - w / 2;
    }
    draw_string(window, lx, y0, line, flags, kKeepColor, background);
    y0 += lh;
  }
  return h;
}

}  // namespace oa
