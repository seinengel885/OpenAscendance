// The help window (`helpwin.cpp`, class `HelpWnd`, `windows.txt` TYPE 20,
// ctor 0x2f370, vtable 0x85cd4): a topic of `help.txt` (or another help
// file) with its pictures, pages and HELPBUTTONn children, run modally by
// message 5 (`recon/notes/ui.md`, "Help system").
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "assets/help_txt.h"
#include "ui/gwindow.h"

namespace oa {

class HelpWnd : public GWindow {
 public:
  // `gwshare.txt` table of `help.shp`, the window's picture (frame 0).
  static constexpr int kHelpShapes = 37;
  // The window's size, and its x on the screens where it opens on the left
  // (0, 1, 7, 8, 16, 20) or elsewhere.
  static constexpr int kWidth = 0x14f;
  static constexpr int kTop = 0x78;
  static constexpr int kBottom = 0x177;
  static constexpr int kLeftX = 0x50;
  static constexpr int kOtherX = 0x98;

  HelpWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes);

  // `HelpWnd_open_topic` (0x2f3d0): loads `topic` of the help file `file`
  // (`HelpWnd_load_topic` 0x2f5d8: the buttons it names shown, the others
  // deactivated) and places the window for the current screen. The texts
  // and button titles are translated (`GameFiles::text` of `file`). Throws
  // `DataError` when the topic is missing.
  void open_topic(std::string_view file, std::string_view topic);
  // The same with the help file's content `data` (the remake's own help
  // texts), `file` naming it in the errors.
  void open_topic(std::string_view file, std::span<const std::uint8_t> data, std::string_view topic);
  const std::string& topic() const { return topic_; }
  const HelpTopic& content() const { return content_; }
  // The loaded topic, for the screens that fill its texts and pictures in
  // (the colony and invasion reports, `PlanWnd_handle_event` 0x35e98).
  HelpTopic& content() { return content_; }
  int page() const { return page_; }

  // `HelpWnd_handle_event` (0x2f488): a right click or help click closes it
  // (message 6, 202); a button's message 50 turns the page, or after the
  // last one closes it with the button's MSG.
  bool handle_event(int type, int a, int b) override;
  // The whole screen (its right and help clicks anywhere).
  void press_areas(std::vector<PressArea>& out) const override { out.push_back(PressArea{Pane{}, false}); }
  // `HelpWnd_draw` (0x2fe94): `help.shp`, the shown buttons spread along the
  // bottom, the current page's elements.
  void draw(int mode) override;

 protected:
  std::string file_;
  std::string topic_;
  HelpTopic content_;
  int page_ = 0;
};

}  // namespace oa
