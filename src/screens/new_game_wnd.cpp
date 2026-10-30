#include "screens/new_game_wnd.h"

#include <format>
#include <optional>
#include <vector>

#include "assets/text_lines.h"
#include "core/error.h"
#include "game/session.h"
#include "screens/cos_wnd.h"
#include "ui/list_box.h"

namespace oa {

namespace {

// `gwshare.txt` tables.
constexpr int kRaceFlags = 32;    // raceflag.shp
constexpr int kRaceRings = 33;    // racering.shp
constexpr int kIntelShapes = 41;  // 14intel.shp
constexpr int kSmallRaces = 42;   // smraces.shp

// Static strings.
constexpr int kStringPlayerSpecies = 0x99;
constexpr int kStringBlurb = 0x9a;        // "The %s %s"
constexpr int kStringDensities = 0x9b;    // 5
constexpr int kStringAtmospheres = 0xa0;  // 3
constexpr int kStringCounts = 0xa3;       // 5, "Three".. "Seven"
constexpr int kStringSummary = 0xa8;
constexpr int kStringBegin = 0xa9;
constexpr int kStringSpeciesNames = 0x65;  // the 21 species (`Race_ctor`)

// The RACELIST rows: the list's line height (`+0x8c9`) and each row's.
constexpr int kListLineHeight = 0x6d;
constexpr int kListRowHeight = 0x73;

// The setting buttons' frames in `3cfgnew.shp`: density + 0, species count
// + 2, atmosphere + 10.
constexpr int kCountFrame = 2;
constexpr int kAtmosphereFrame = 10;


// The click sound (`soundfx.txt` 0).
constexpr int kClickSound = 0;

// The screen of the species history.
constexpr int kScreenStartGame = 12;

// The brightest entry of colour `c`'s ramp (`0x865f9`): 0x13 + 4 c.
std::uint8_t color_text(int c) { return static_cast<std::uint8_t>(0x13 + 4 * c); }

// The `printf` of a static string's `%s` fields.
std::string fill(std::string_view format, std::initializer_list<std::string_view> args) {
  std::string out;
  auto it = args.begin();
  for (std::size_t i = 0; i < format.size(); ++i) {
    if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == 's' && it != args.end()) {
      out += *it++;
      ++i;
    } else {
      out += format[i];
    }
  }
  return out;
}

}  // namespace

NewGameWnd::NewGameWnd(WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes)
    : GWindow(wm, record, shapes) {}

void NewGameWnd::press_areas(std::vector<PressArea>& out) const {
  out.push_back(PressArea{pane_, false});
  // The open boxes of the clicks' tests.
  for (const Pane& box : {kColorBar, kBeginBox}) {
    out.push_back(PressArea{Pane{box.x0 + 1, box.y0 + 1, box.x1 - 1, box.y1 - 1}, true});
  }
}

bool NewGameWnd::in_begin_box(int x, int y) const {
  return x > kBeginBox.x0 && x < kBeginBox.x1 && y > kBeginBox.y0 && y < kBeginBox.y1;
}

void NewGameWnd::set_button_frame(int index, int frame, bool redraw) {
  if (static_cast<std::size_t>(index) >= children_.size()) {
    return;
  }
  GWindow* button = children_[static_cast<std::size_t>(index)];
  button->set_item_frame(0, frame);
  if (redraw) {
    button->draw(kDrawNormal);
  }
}

bool NewGameWnd::handle_event(int type, int a, int b) {
  GameSession& game = wm_.services().game;
  switch (type) {
    case kEventActivate: {
      game.save_resume();
      // Going back to the opening menu reloads resume.gam (0x86a4c).
      wm_.set_reload_resume(true);
      if (list_ == nullptr) {
        list_ = dynamic_cast<ListBox*>(wm_.find_window("RACELIST"));
        if (list_ == nullptr) {
          throw DataError("windows.txt: no RACELIST list box");
        }
        list_->set_line_height(kListLineHeight);
        list_->set_draw_item([this](const Pane& row, const ListBox::Item& item, bool hilite) {
          draw_species_item(row, item.data, hilite);
        });
        list_->set_style(0);
        list_->set_colors(0x31, kBackColor, kBackColor);
      }
      list_->clear();
      for (int s = 0; s < kSpecies; ++s) {
        list_->set_item_data(list_->add_item({}, kListRowHeight), s);
      }
      settings_ = NewGameSettings{};
      set_button_frame(0, settings_.star_density, false);
      set_button_frame(1, settings_.species_count + kCountFrame, false);
      set_button_frame(2, settings_.atmosphere + kAtmosphereFrame, false);
      game.open_new_game(settings_);
      vfx_pane_wipe(wm_.screen(), kColorBar, kBackColor);
      // NewGame_load_blurbs 0x4d584: 21 lines of newgame.txt (fgets, 199).
      const std::vector<std::uint8_t> data = wm_.services().files.load("newgame.txt");
      TextLineReader lines(data);
      for (std::string& blurb : blurbs_) {
        const std::optional<std::string> line = lines.line();
        blurb = line ? wm_.services().files.text("newgame.txt", *line) + "\n" : std::string();
      }
      return GWindow::handle_event(type, a, b);
    }
    case kEventLeftClick:
    case kEventRightClick:
      if (a > kColorBar.x0 && a < kColorBar.x1 && b > kColorBar.y0 && b < kColorBar.y1) {
        wm_.services().sound.play_sfx(kClickSound);
        settings_.color = (a - kColorBar.x0) / 16;
        game.assign_colors(settings_.color);
        draw(kDrawNormal);
        return true;
      }
      if (!in_begin_box(wm_.input().x, wm_.input().y)) {
        return false;
      }
      wm_.services().sound.play_sfx(kClickSound);
      game.begin_game(settings_);
      wm_.new_game_windows();
      wm_.set_reload_resume(false);
      wm_.post_message(kMessageChangeScreen, kScreenStartGame, 0);
      return true;
    case kEventHelpClick:
      if (a > kColorBar.x0 && a < kColorBar.x1 && b > kColorBar.y0 && b < kColorBar.y1) {
        wm_.post_message(kMessageHelp, 0, kHelpTopic, "pcolor");
        return true;
      }
      if (in_begin_box(wm_.input().x, wm_.input().y)) {
        wm_.post_message(kMessageHelp, 0, kHelpTopic, "beghelp");
        return true;
      }
      return GWindow::handle_event(type, a, b);
    case kMessageDensity:
      if (++settings_.star_density == 5) {
        settings_.star_density = 0;
      }
      game.generate_galaxy(GameSession::kDensityStars[static_cast<std::size_t>(settings_.star_density)],
                           settings_.species_count, settings_.species);
      rebuild_map();
      draw(kDrawNormal);
      set_button_frame(0, settings_.star_density, true);
      return true;
    case kMessageSpeciesCount:
      if (++settings_.species_count == 8) {
        settings_.species_count = 3;
      }
      game.new_galaxy(settings_.species_count, settings_.species);
      rebuild_map();
      draw(kDrawNormal);
      set_button_frame(1, settings_.species_count + kCountFrame, true);
      return true;
    case kMessageAtmosphere:
      if (++settings_.atmosphere == 3) {
        settings_.atmosphere = 0;
      }
      draw(kDrawNormal);
      set_button_frame(2, settings_.atmosphere + kAtmosphereFrame, true);
      return true;
    case kEventListPick:
      if (a != settings_.species) {
        settings_.species = a;
        game.new_galaxy(settings_.species_count, settings_.species);
        draw(kDrawNormal);
      }
      return true;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

void NewGameWnd::rebuild_map() {
  if (auto* map = dynamic_cast<CosWnd*>(wm_.find_window("COSMOSWnd"))) {
    map->build_items();
  }
}

void NewGameWnd::draw_species_item(const Pane& row, int species, bool hilite) {
  Image8& screen = wm_.screen();
  if (hilite) {
    vfx_pane_wipe(screen, row, kHotBackColor);
  }
  vfx_shape_draw(screen, row, wm_.shared_shapes(kSmallRaces).at(static_cast<std::size_t>(species)), 0x38, 0x37);
  vfx_shape_draw(screen, row, wm_.shared_shapes(kRaceRings).at(7), 0x38, 0x37);
}

void NewGameWnd::draw_begin_box() {
  Image8& screen = wm_.screen();
  const InputState& in = wm_.input();
  vfx_pane_wipe(screen, kBeginBox, in_begin_box(in.x, in.y) ? kHotBackColor : kBackColor);
  // The star map's stars, a fifth of their size, in its drawing order.
  if (const auto* map = dynamic_cast<const CosWnd*>(wm_.find_window("COSMOSWnd"))) {
    const game::Session* core = wm_.services().game.core();
    for (const std::int32_t k : map->draw_list()) {
      const CosWnd::Item& it = map->items()[static_cast<std::size_t>(k)];
      if (it.kind != CosWnd::ItemKind::kStar || core == nullptr) {
        continue;
      }
      const std::int32_t d = game::chop(static_cast<double>(it.depth) + static_cast<double>(-CosWnd::kDistance));
      const int shade = d > 220 ? 0 : d > 0 ? 1 : d > -220 ? 2 : 3;
      const int type = core->game->cosmos.stars[static_cast<std::size_t>(it.index)].type;
      vfx_shape_draw(screen, kBeginBox, map->star_shapes().at(static_cast<std::size_t>(type * 4 + shade)), it.x / 5 + 5,
                     it.y / 5 + 5);
    }
  }
  vfx_shape_draw_half(screen, kBeginBox, wm_.shared_shapes(kSmallRaces).at(static_cast<std::size_t>(settings_.species)),
                      0x62, 0x24);
  const GameSession& game = wm_.services().game;
  wm_.draw_colored(kBeginBox, wm_.shared_shapes(kIntelShapes).at(3), 0x62, 0x24, game.race_color(0));
  GameFont& font = wm_.large_font();
  font.pane() = kBeginBox;
  font.draw_wrapped(screen, 0x41, 0x50, wm_.static_string(kStringBegin), kFontAboveV | kFontAboveH, kDefault, 0xff, 0);
}

void NewGameWnd::draw(int mode) {
  Image8& screen = wm_.screen();
  draw_begin_box();
  GameFont& large = wm_.large_font();
  large.pane() = Pane{0x1f6, 7, 0x278, 0x24};
  large.draw_wrapped(screen, 0, 0, wm_.static_string(kStringPlayerSpecies), kFontCenterH | kFontCenterV, kTextColor,
                     0xff, 0x6e);
  if (mode != kDrawNormal) {
    return;
  }
  vfx_pane_wipe(screen, pane_, 0);
  const std::string& name = wm_.static_string(kStringSpeciesNames + settings_.species);
  const std::uint8_t text_color = color_text(settings_.color);
  large.pane() = Pane{pane_.x0, pane_.y0, pane_.x1, 0x23};
  large.draw_wrapped(screen, 0, 0, name, kFontCenterH | kFontCenterV, text_color, 0xff, 0);
  const auto species = static_cast<std::size_t>(settings_.species);
  vfx_shape_draw(screen, pane_, wm_.shared_shapes(kSmallRaces).at(species), 0x4b, 0x55);
  vfx_shape_draw(screen, pane_, wm_.shared_shapes(kRaceRings).at(static_cast<std::size_t>(settings_.color)), 0x4b,
                 0x55);
  vfx_shape_draw(screen, pane_, wm_.shapes().get("DATA\\smhome.shp").at(species), 0x48, 0xcd);
  const GameSession& game = wm_.services().game;
  const int player = game.player_race();
  wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(static_cast<std::size_t>(game.race_species(player))), 0x6b,
                   0xc1, game.race_color(player));
  GameFont& small = wm_.small_font();
  small.pane() = Pane{pane_.x0, 0x109, pane_.x1, pane_.y1};
  small.draw_wrapped(screen, 0, 0, fill(wm_.static_string(kStringBlurb), {name, blurbs_[species]}), 0, text_color, 0xff,
                     0x96);

  large.pane() = Pane{0x11, 0x145, 0x144, 0x172};
  const std::string summary =
      fill(wm_.static_string(kStringSummary), {wm_.static_string(kStringDensities + settings_.star_density),
                                               wm_.static_string(kStringCounts + settings_.species_count - 3),
                                               wm_.static_string(kStringAtmospheres + settings_.atmosphere)});
  large.draw_wrapped(screen, 0, 0, summary, kFontWipe | kFontCenterH, kDefault, 0, 0xfa);

  // The colour bar: the player's flag in each colour, the chosen one framed.
  Pane flag{0x17d, 0x1b1, 0x18b, 0x1cf};
  for (int c = 0; c < kColors; ++c) {
    wm_.draw_colored(flag, wm_.shared_shapes(kRaceFlags).at(species), 0, 0, c);
    if (c == settings_.color) {
      // draw_rect_frame 0x2b294: (0, 0)-(14, 24).
      vfx_line_draw(screen, flag, 0, 0, 14, 0, kTextColor);
      vfx_line_draw(screen, flag, 0, 24, 14, 24, kTextColor);
      vfx_line_draw(screen, flag, 0, 0, 0, 24, kTextColor);
      vfx_line_draw(screen, flag, 14, 0, 14, 24, kTextColor);
    }
    flag.x0 += 16;
    flag.x1 += 16;
  }
}

void add_new_game_windows(WindowFactory& factory) {
  factory.add(18, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<NewGameWnd>(wm, record, shapes);
  });
}

}  // namespace oa
