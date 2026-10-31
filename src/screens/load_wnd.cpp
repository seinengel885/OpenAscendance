#include "screens/load_wnd.h"

#include <array>
#include <cstdio>
#include <format>
#include <optional>

#include "assets/text_lines.h"
#include "core/error.h"

namespace oa {

namespace {

constexpr int kScreenStarMap = 1;
constexpr int kScreenSave = 2;
constexpr int kScreenLoad = 4;

// The static strings of the screen.
constexpr int kStringEmpty = 0x10;  // "<EMPTY>"
constexpr int kStringDay = 0x11;    // "Day %d"
constexpr int kStringAm = 0x12;
constexpr int kStringPm = 0x13;
constexpr int kStringFleet = 0x14;
constexpr int kStringColonies = 0x15;
constexpr int kStringDiscoveries = 0x16;
constexpr int kStringLoadGame = 0x17;
constexpr int kStringSaveGame = 0x18;
constexpr int kStringTutorial = 0x19;
constexpr int kStringCannotOpen = 0xaa;  // the save cannot be opened
// The species names (static strings 101..121, `Race_ctor` 0x39a50).
constexpr int kStringSpecies = 101;

// `gwshare.txt` tables.
constexpr int kRaceFlags = 32;    // raceflag.shp, frame = species
constexpr int kIntelShapes = 41;  // 14intel.shp
constexpr int kSmallRaces = 42;   // smraces.shp, frame = species

// 14intel.shp frames: the portrait's ring, the pips, the war and alliance
// marks.
constexpr int kRingFrame = 3;
constexpr int kFleetPip = 5;
constexpr int kColonyPip = 6;
constexpr int kDiscoveryPip = 7;
constexpr int kWarMark = 8;
constexpr int kAllianceMark = 9;

// The text colours of a row: an unused row, the hovered one.
constexpr int kDimColor = 9;
constexpr int kHotColor = 0xf3;

// The title bar's background: load, save.
constexpr std::uint8_t kLoadTitleColor = 0x10;
constexpr std::uint8_t kSaveTitleColor = 0x73;

// The colour index of a race colour's brightest entry.
int race_text_color(int color) { return color * 4 + 0x13; }

}  // namespace

void LoadWnd::read_headers() {
  list_->clear();
  slots_ = wm_.services().game.list_saves();
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    const int row = list_->add_item(std::format("{:02d}.sav", slots_[static_cast<std::size_t>(i)].slot));
    list_->set_item_data(row, i);
  }
}

void LoadWnd::load_tutindex() {
  list_->clear();
  tutorials_.clear();
  const std::vector<std::uint8_t> data = wm_.services().files.load("tutindex.txt");
  TextLineReader r(data);
  const std::optional<std::string> count_line = r.content_line();
  const int count = count_line ? std::stoi(*count_line) : 0;
  if (count > kMaxTutorials) {
    throw DataError(std::format("tutindex.txt: {} tutorials", count));
  }
  for (int i = 0; i < count; ++i) {
    const std::optional<std::string> name = r.content_line();
    const std::optional<std::string> description = r.content_line();
    if (!name || !description) {
      throw DataError("tutindex.txt: short file");
    }
    tutorials_.push_back(
        Tutorial{name->substr(0, name->find_first_of(" \t")), wm_.services().files.text("tutindex.txt", *description), i + 1});
    const int row = list_->add_item(tutorials_.back().name);
    list_->set_item_data(row, i);
  }
}

bool LoadWnd::handle_event(int type, int a, int b) {
  switch (type) {
    case kEventActivate: {
      std::string help;
      const int screen = wm_.current_screen();
      if (screen == kScreenSave) {
        mode_ = Mode::kSave;
        help = "savescr";
      } else if (screen == kScreenLoad) {
        mode_ = Mode::kLoad;
        help = "loadscr";
      } else {
        mode_ = Mode::kTutorial;
        help = "tutscr";
      }
      list_ = dynamic_cast<ListBox*>(wm_.find_window("LOADLIST"));
      if (list_ == nullptr) {
        throw DataError("windows.txt: no LOADLIST list box");
      }
      if (mode_ == Mode::kTutorial) {
        list_->set_line_height(kTutorialRowHeight);
        list_->set_draw_item([this](const Pane& row, const ListBox::Item& item, bool hilite) {
          draw_tutorial_entry(row, item.data, hilite);
        });
        load_tutindex();
      } else {
        list_->set_line_height(kSaveRowHeight);
        list_->set_draw_item([this](const Pane& row, const ListBox::Item& item, bool hilite) {
          draw_save_entry(row, item.data, hilite);
        });
        read_headers();
      }
      set_help(help);
      list_->set_help(help);
      GWindow::handle_event(type, a, b);
      return false;
    }
    case kEventKey:
      if (a > '0' && a <= '9') {
        const int digit = a - '0';
        if (digit < (mode_ == Mode::kSave ? 10 : 9)) {
          handle_event(kEventListPick, digit, 0);
          return true;
        }
      }
      return false;
    case kEventListPick:
      pick(a);
      return true;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

void LoadWnd::pick(int row) {
  if (list_ == nullptr || row < 0 || row >= list_->count()) {
    return;
  }
  GameSession& game = wm_.services().game;
  const int index = list_->item(row).data;
  switch (mode_) {
    case Mode::kSave:
      if (!game.save(slots_[static_cast<std::size_t>(index)].slot)) {
        // Game_save 0x4f458: the file cannot be opened.
        wm_.message_box(wm_.static_string(kStringCannotOpen));
      }
      wm_.post_message(kMessageBack, 0, 0);
      break;
    case Mode::kLoad: {
      const SaveSlot& slot = slots_[static_cast<std::size_t>(index)];
      if (slot.header) {
        if (game.load(slot.slot)) {
          // Esc from the star map then comes back here, then to the menu.
          wm_.reset_history_to_opening();
          wm_.post_message(kMessageChangeScreen, kScreenStarMap, 1);
        } else {
          wm_.message_box(wm_.static_string(kStringCannotOpen));
        }
      }
      break;
    }
    case Mode::kTutorial:
      wm_.start_tutorial(tutorials_[static_cast<std::size_t>(index)].name);
      break;
  }
}

void LoadWnd::draw(int /*mode*/) {
  int title = kStringLoadGame;
  if (mode_ == Mode::kSave) {
    title = kStringSaveGame;
    vfx_pane_wipe(wm_.screen(), pane_, kSaveTitleColor);
  } else if (mode_ == Mode::kTutorial) {
    title = kStringTutorial;
  } else {
    vfx_pane_wipe(wm_.screen(), pane_, kLoadTitleColor);
  }
  GameFont& font = wm_.large_font();
  font.pane() = pane_;
  font.draw_wrapped(wm_.screen(), 0, 4, wm_.static_string(title), kFontCenterH | kFontCenterV, kDefault, 0xff, 0);
  draw_children();
}

void LoadWnd::draw_save_entry(const Pane& row, int index, bool hilite) {
  const SaveSlot& slot = slots_[static_cast<std::size_t>(index)];
  const std::optional<SaveHeader>& h = slot.header;
  int color = kDimColor;
  if (hilite && (mode_ == Mode::kSave || h)) {
    color = kHotColor;
  } else if (h) {
    color = race_text_color(h->colors[static_cast<std::size_t>(h->player_race)]);
  }
  Image8& screen = wm_.screen();
  GameFont& large = wm_.large_font();
  large.pane() = row;
  large.draw_wrapped(screen, 5, 5, std::format("{}.", slot.slot), 0, color, 0xff, 0);
  if (!h) {
    large.draw_wrapped(screen, 0x1e, 5, wm_.static_string(kStringEmpty), 0, color, 0xff, 0);
    return;
  }
  const auto player = static_cast<std::size_t>(h->player_race);
  const int species = h->species[player];
  const int player_color = h->colors[player];
  wm_.draw_colored(row, wm_.shared_shapes(kRaceFlags).at(static_cast<std::size_t>(species)), 0x19, 5, player_color);
  large.draw_wrapped(screen, 0x2d, 5, wm_.static_string(kStringSpecies + species), 0, color, 0xff, 0);
  // The static string is a printf format ("Day %d").
  std::array<char, 64> day{};
  std::snprintf(day.data(), day.size(), wm_.static_string(kStringDay).c_str(), h->day);
  large.draw_wrapped(screen, 0x2d, 0x14, day.data(), 0, color, 0xff, 0);
  // The DOS file date and time: the year counts from 1900.
  large.draw_wrapped(screen, 0x2d, 0x32, std::format("{}/{}/{}", slot.month, slot.day, slot.year - 1900), 0, color,
                     0xff, 0);
  int hour = slot.hour;
  std::string_view ampm = wm_.static_string(kStringAm);
  if (hour == 0) {
    hour = 12;
  } else if (hour == 12) {
    ampm = wm_.static_string(kStringPm);
  } else if (hour > 12) {
    ampm = wm_.static_string(kStringPm);
    hour -= 12;
  }
  large.draw_wrapped(screen, 0x2d, 0x41, std::format("{} : {:02d} {}", hour, slot.minute, ampm), 0, color, 0xff, 0);

  const ShapeTable& intel = wm_.shared_shapes(kIntelShapes);
  vfx_shape_draw_half(screen, row, wm_.shared_shapes(kSmallRaces).at(static_cast<std::size_t>(species)), 0x96, 0x1e);
  wm_.draw_colored(row, intel.at(kRingFrame), 0x96, 0x1e, player_color);

  // The fleet, colonies and discoveries (/ 4) as rows of 9 pips.
  GameFont& small = wm_.small_font();
  small.pane() = row;
  const auto pips = [&](int label, int x0, int count, int frame) {
    small.draw_wrapped(screen, x0, 5, wm_.static_string(label), 0, color, 0xff, 0);
    int x = x0;
    int y = 0x14;
    for (int i = 0; i < count; ++i) {
      wm_.draw_colored(row, intel.at(static_cast<std::size_t>(frame)), x, y, player_color);
      x += 8;
      if (i % 9 == 8) {
        y += 8;
        x = x0;
      }
    }
  };
  pips(kStringFleet, 200, h->fleet, kFleetPip);
  pips(kStringColonies, 0x118, h->colonies, kColonyPip);
  pips(kStringDiscoveries, 0x168, h->discoveries >> 2, kDiscoveryPip);

  // The species the player has met, with their war / alliance mark.
  const ShapeTable& flags = wm_.shared_shapes(kRaceFlags);
  int x = 0x1cc;
  int y = 5;
  for (int i = 0; i < h->race_count; ++i) {
    const auto r = static_cast<std::size_t>(i);
    if (i == h->player_race || h->relations[r] == 0) {
      continue;
    }
    wm_.draw_colored(row, flags.at(static_cast<std::size_t>(h->species[r])), x, y, h->colors[r]);
    const int mark = h->relations[r] == 3 ? kAllianceMark : h->relations[r] == 2 ? kWarMark : -1;
    if (mark != -1) {
      vfx_shape_draw(screen, row, intel.at(static_cast<std::size_t>(mark)), x, y);
    }
    x += 0x39;
    if (x > 600) {
      y += 0x1b;
      x = 0x1cc;
    }
  }
}

void LoadWnd::draw_tutorial_entry(const Pane& row, int index, bool hilite) {
  const Tutorial& t = tutorials_[static_cast<std::size_t>(index)];
  const int color = hilite ? kHotColor : kDimColor;
  GameFont& font = wm_.large_font();
  font.pane() = row;
  font.draw_wrapped(wm_.screen(), 5, 5, std::format("{}.", t.number), 0, color, 0xff, 0);
  font.draw_wrapped(wm_.screen(), 0x1e, 5, t.description, 0, color, 0xff, 0x226);
}

void add_load_windows(WindowFactory& factory) {
  factory.add(11, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<LoadWnd>(wm, record, shapes);
  });
}

}  // namespace oa
