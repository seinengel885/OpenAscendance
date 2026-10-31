#include "ui/help_wnd.h"

#include "ui/winmgr.h"

namespace oa {

namespace {

// The buttons' row and the text area, relative to the window.
constexpr int kButtonsX0 = 8;
constexpr int kButtonsX1 = 0x147;
constexpr int kButtonsY0 = 0xd9;
constexpr int kButtonsY1 = 0xf7;
constexpr int kTextX0 = 10;
constexpr int kTextY0 = 10;
constexpr int kTextX1 = 0x145;
constexpr int kTextY1 = 0xcf;
// BPOS: the buttons' corner boxes.
constexpr int kCornerInset = 0xf;
constexpr int kCornerWidth = 0x5a;
constexpr int kCornerHeight = 0x32;
// CENTERPOS: the height of the buttons' row.
constexpr int kButtonRow = 0x32;

}  // namespace

HelpWnd::HelpWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) : GWindow(wm, record, shapes) {}

void HelpWnd::open_topic(std::string_view file, std::string_view topic) {
  open_topic(file, wm_.services().files.load(file), topic);
}

void HelpWnd::open_topic(std::string_view file, std::span<const std::uint8_t> data, std::string_view topic) {
  file_ = file;
  topic_ = topic;
  page_ = 0;
  // HelpWnd_load_topic 0x2f5d8.
  broadcast_to_children(kEventDeactivate, 0, 0);
  shapes_ = &wm_.shared_shapes(kHelpShapes);
  frame_ = 0;
  // CENTERPOS and BPOS use the rectangle of the previous placement.
  content_ = parse_help_topic(data, topic_, file_, (pane_.x1 - pane_.x0) / 2, (pane_.y1 - kButtonRow - pane_.y0) / 2);
  const GameFiles& files = wm_.services().files;
  for (HelpElement& e : content_.elements) {
    if (e.kind == HelpElement::Kind::kText) {
      e.text = files.text(file_, e.text);
    }
  }
  for (std::size_t i = 0; i < content_.buttons.size() && i < children_.size(); ++i) {
    const HelpButton& b = content_.buttons[i];
    GWindow* child = children_[i];
    if (b.shown) {
      wm_.send_event(child->id(), kEventShow, 0, 0);
    }
    if (b.title) {
      child->set_name(files.text(file_, *b.title));
    }
    if (b.message) {
      child->set_message(child->message(), *b.message, 0);
    }
    if (b.pane) {
      child->set_pane(*b.pane);
    }
    if (b.corner) {
      const int x0 = *b.corner == 'L' ? pane_.x0 + kCornerInset : pane_.x1 - kCornerWidth;
      const int x1 = *b.corner == 'L' ? pane_.x0 + kCornerWidth : pane_.x1 - kCornerInset;
      child->set_pane(Pane{x0, pane_.y1 - kCornerHeight, x1, pane_.y1 - kCornerInset});
    }
  }
  const int screen = wm_.current_screen();
  const bool left = screen == 0 || screen == 1 || screen == 7 || screen == 8 || screen == 16 || screen == 20;
  const int x = left ? kLeftX : kOtherX;
  pane_ = Pane{x, kTop, x + kWidth, kBottom};
}

bool HelpWnd::handle_event(int type, int a, int b) {
  switch (type) {
    case kEventActivate:
      shown_ = true;
      active_ = true;
      draw(kDrawNormal);
      return false;
    case kEventRightClick:
    case kEventHelpClick:
      wm_.post_message(kMessageEndModal, kEventCancel, 0);
      return true;
    case kEventHelpButton:
      if (page_ < content_.pages) {
        ++page_;
        draw(kDrawNormal);
      } else {
        wm_.post_message(kMessageEndModal, a, b);
      }
      return false;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

void HelpWnd::draw(int /*mode*/) {
  Image8& screen = wm_.screen();
  vfx_pane_wipe(screen, pane_, kBackColor);
  if (const Shape* s = shape(frame_)) {
    vfx_shape_draw(screen, pane_, *s, 0, 0);
  }
  // The shown buttons share the bottom row.
  std::vector<GWindow*> shown;
  for (GWindow* child : children_) {
    if (child->shown()) {
      shown.push_back(child);
    }
  }
  int x = pane_.x0 + kButtonsX0;
  const int end = pane_.x0 + kButtonsX1;
  int left = static_cast<int>(shown.size());
  for (GWindow* child : shown) {
    const int x0 = x;
    x += (end - x) / left;
    x = std::min(x, end);
    --left;
    child->set_pane(Pane{x0, pane_.y0 + kButtonsY0, x, pane_.y0 + kButtonsY1});
    ++x;
  }
  // The current page: the elements after the page's start, up to its end.
  const Pane text{pane_.x0 + kTextX0, pane_.y0 + kTextY0, pane_.x0 + kTextX1, pane_.y0 + kTextY1};
  int index = 0;
  for (int ends = 0; ends < page_ && index < content_.element_count; ++index) {
    if (content_.elements[static_cast<std::size_t>(index)].page_end) {
      ++ends;
    }
  }
  for (; index < content_.element_count; ++index) {
    const HelpElement& e = content_.elements[static_cast<std::size_t>(index)];
    if (e.kind == HelpElement::Kind::kNone) {
      // Hidden.
    } else if (e.kind == HelpElement::Kind::kShape) {
      const ShapeTable& table = wm_.shared_shapes(e.shape_index);
      vfx_shape_draw(screen, text, table.at(static_cast<std::size_t>(e.frame)), e.x, e.y);
    } else {
      GameFont& font = e.small_font ? wm_.small_font() : wm_.large_font();
      font.pane() = text;
      const int width = (e.flags & kFontCenterH) != 0 ? 0 : (text.x1 - text.x0) - e.x;
      font.draw_wrapped(screen, e.x, e.y, e.text, e.flags, e.color, 0xff, width);
    }
    if (e.page_end) {
      break;
    }
  }
  draw_children();
}

}  // namespace oa
