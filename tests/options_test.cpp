// The remake's options (`screens/options_wnd.h`, `ui/options.h`): the
// opening menu's Options button and layout, the options screen and
// `options.cfg`, an Antagonizer new game against the original's (OA_DATA
// install; the comparisons with the original's runs need OA_ORACLE). With
// OA_SHOTS set, the screens are written there (`screen_shots.h`).

#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "assets/gif.h"
#include "core_session.h"
#include "game/save.h"
#include "game_data.h"
#include "platform/touch.h"
#include "screen_shots.h"
#include "screens/options_wnd.h"
#include "test_dirs.h"
#include "ui/game_core_session.h"
#include "ui/help_wnd.h"
#include "ui/options.h"
#include "ui_rig.h"

namespace oa {
namespace {

using test::TempDir;

// The original's menu buttons (`windows.txt`): 159..477, 31 rows from y
// 200 every 40.
std::vector<Pane> original_menu() {
  std::vector<Pane> panes;
  for (int i = 0; i < 7; ++i) {
    panes.push_back(Pane{159, 200 + 40 * i, 477, 230 + 40 * i});
  }
  return panes;
}

// The y of the middle of the Options button on the remake's menu.
int options_y() {
  const Pane pane = spread_menu_buttons(original_menu(), 8)[kOptionsButtonIndex];
  return (pane.y0 + pane.y1) / 2;
}

enum Row : int { kVariant, kDisplay, kScaling, kMusic, kSound };
// The options screen's buttons: choice `c` of row `row`, and OK and Cancel
// (`options_wnd.cpp`: rows from y 72 every 60; the choices' columns
// 258..422 and 445..609, those of the three variants 258..360, 383..485 and
// 508..610; OK and Cancel at y 424).
constexpr int choice_x0(int c, int row) { return row == kVariant ? 258 + 125 * c : c == 0 ? 258 : 445; }
constexpr int choice_x(int c, int row) { return row == kVariant ? 309 + 125 * c : c == 0 ? 340 : 527; }
constexpr int row_y(int row) { return 72 + 60 * row + 14; }
constexpr int kOkX = 340;
constexpr int kCancelX = 527;
constexpr int kButtonsY = 437;
// The volume sliders of rows Music and Sound Effects (258..610, a level
// from 0 at x 264 to 100 at x 604): the x of `level`, a multiple of 5.
constexpr int slider_x(int level) { return 264 + level * 34 / 10; }

class FakeDisplay final : public DisplayControl {
 public:
  bool fullscreen() const override { return fullscreen_; }
  void set_fullscreen(bool fullscreen) override { fullscreen_ = fullscreen; }
  Scaling scaling() const override { return scaling_; }
  void set_scaling(Scaling scaling) override { scaling_ = scaling; }

 private:
  bool fullscreen_ = false;
  Scaling scaling_ = Scaling::kSmooth;
};

// Installs the options on `rig`, the volumes of its settings applied as
// `main` does, and enters the opening menu again (the rig starts on it with
// the original layout); returns the handler of Alt+Enter.
FullscreenToggled install(test::UiRig& rig, OptionsContext context) {
  rig.sound.set_music_level(context.options->music_volume);
  rig.sound.set_sfx_level(context.options->sfx_volume);
  FullscreenToggled toggled = install_options(rig.wm, std::move(context));
  rig.wm.set_screen(kScreenOptions, false);
  rig.wm.set_screen(0, false);
  return toggled;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST_CASE("the opening menu's buttons spread over the original's span", "[ui]") {
  const std::vector<Pane> original = original_menu();
  const std::vector<Pane> eight = spread_menu_buttons(original, 8);
  REQUIRE(eight.size() == 8);
  CHECK(eight.front().y0 == 200);
  CHECK(eight.back().y1 == 470);
  for (std::size_t i = 0; i < eight.size(); ++i) {
    CHECK(eight[i].x0 == 159);
    CHECK(eight[i].x1 == 477);
    // 271 rows: 8 buttons of 26 and 7 gaps of 9, the original's gap.
    CHECK(eight[i].y1 - eight[i].y0 + 1 == 26);
    if (i > 0) {
      CHECK(eight[i].y0 - eight[i - 1].y1 - 1 == 9);
    }
  }
  // Rows that do not divide go to the first buttons.
  const std::vector<Pane> six = spread_menu_buttons(original, 6);
  CHECK(six.front().y1 - six.front().y0 + 1 == (271 - 5 * 9) / 6 + 1);
  CHECK(six.back().y1 == 470);
  CHECK(spread_menu_buttons(original, 7) == original);
}

TEST_CASE("the opening menu with the Options button", "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  test::UiRig rig(files);
  RemakeOptions options;
  install(rig, OptionsContext{.options = &options, .file = {}, .display = nullptr});

  const std::vector<Pane> eight = spread_menu_buttons(original_menu(), 8);
  std::size_t slot = 0;
  for (std::size_t i = 0; i < kMenuButtons.size(); ++i, ++slot) {
    if (i == kOptionsButtonIndex) {
      ++slot;
    }
    CHECK(rig.wm.find_window(kMenuButtons[i])->pane() == eight[slot]);
  }
  GWindow* button = rig.wm.find_window("Options");
  REQUIRE(button != nullptr);
  CHECK(button->pane() == eight[kOptionsButtonIndex]);
  CHECK(button->shown());

  // Everything but the panel's rows between the buttons' top and bottom is
  // the original's picture.
  const Image8 gif = decode_gif(files.load("DATA\\0OPENING.GIF"), "0OPENING.GIF");
  int differing = 0;
  for (int y = 0; y < gif.height(); ++y) {
    for (int x = 0; x < gif.width(); ++x) {
      const bool panel = x >= 152 && x <= 484 && y >= 200 && y <= 470;
      if (!panel && rig.wm.screen().at(x, y) != gif.at(x, y)) {
        ++differing;
      }
    }
  }
  CHECK(differing == 0);
  rig.move(318, options_y());
  CHECK(rig.wm.hot_window() == button->id());
  test::write_shot(rig, "options_menu");

  // Options opens the options screen, Esc goes back.
  rig.click(318, options_y());
  CHECK(rig.wm.current_screen() == kScreenOptions);
  rig.key(SDLK_ESCAPE);
  CHECK(rig.wm.current_screen() == 0);
  // The other buttons keep their messages: New Game.
  rig.click(318, 282);
  CHECK(rig.wm.current_screen() == 3);
}

TEST_CASE("a tutorial replays on the original opening menu", "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  GameCoreSession session(files, {}, false);
  session.start(0, nullptr);
  test::UiRig rig(files, &session);
  RemakeOptions options;
  install(rig, OptionsContext{.options = &options, .file = {}, .display = nullptr});
  rig.wm.set_screen(15, true);
  REQUIRE(rig.wm.start_tutorial("beginnew"));
  rig.pass();
  REQUIRE(rig.wm.current_screen() == 0);
  const std::vector<Pane> original = original_menu();
  for (std::size_t i = 0; i < kMenuButtons.size(); ++i) {
    CHECK(rig.wm.find_window(kMenuButtons[i])->pane() == original[i]);
  }
  CHECK_FALSE(rig.wm.find_window("Options")->shown());
  const Image8 gif = decode_gif(files.load("DATA\\0OPENING.GIF"), "0OPENING.GIF");
  CHECK(rig.wm.screen().at(155, 235) == gif.at(155, 235));
}

TEST_CASE("the options screen: the selected choices stand out from the others, hot or not", "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  test::UiRig rig(files);
  RemakeOptions options;
  install(rig, OptionsContext{.options = &options, .file = {}, .display = nullptr});
  rig.click(318, options_y());
  REQUIRE(rig.wm.current_screen() == kScreenOptions);
  // A background pixel of choice `c` of row `row` (the rows 28 high from
  // y 72), off its caption.
  const auto background = [&](int c, int row) { return rig.wm.screen().at(choice_x0(c, row) + 3, 72 + 60 * row + 3); };
  // Standard (the default variant) selected, Antagonizer not.
  CHECK(background(0, kVariant) == kSelectedColor);
  CHECK(background(1, kVariant) == GWindow::kBackColor);
  rig.move(choice_x(0, kVariant), row_y(kVariant));
  CHECK(background(0, kVariant) == kSelectedHotColor);
  test::write_shot(rig, "options_screen_selected_hot");
  rig.move(choice_x(1, kVariant), row_y(kVariant));
  CHECK(background(0, kVariant) == kSelectedColor);
  CHECK(background(1, kVariant) == GWindow::kHotBackColor);
  // The third variant, peCustom.
  rig.click(choice_x(2, kVariant), row_y(kVariant));
  CHECK(rig.game.new_game_variant() == game::Variant::kPeCustom);
  CHECK(background(2, kVariant) == kSelectedHotColor);
  CHECK(background(0, kVariant) == GWindow::kBackColor);

  // In the game's palette, far from the unselected colours (the toggles'
  // "on" colours 0x50 / 0xca were within 80 of 0xf2), dark enough for the
  // white captions.
  const Palette palette = Palette::from_vga(rig.gs.shadow());
  const auto distance = [&](std::uint8_t a, std::uint8_t b) {
    const Rgb& p = palette.colors.at(a);
    const Rgb& q = palette.colors.at(b);
    const auto d = [](int u, int v) { return (u - v) * (u - v); };
    return std::sqrt(static_cast<double>(d(p.r, q.r) + d(p.g, q.g) + d(p.b, q.b)));
  };
  CHECK(distance(0x50, GWindow::kBackColor) < 80);
  for (const std::uint8_t selected : {kSelectedColor, kSelectedHotColor}) {
    for (const std::uint8_t other : {GWindow::kBackColor, GWindow::kHotBackColor}) {
      CHECK(distance(selected, other) > 150);
    }
    const Rgb& c = palette.colors.at(selected);
    CHECK(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b < 100);
  }
}

TEST_CASE("options.cfg: the settings' text round-trips; bad lines are ignored", "[ui]") {
  const RemakeOptions defaults;
  CHECK(parse_options("") == defaults);
  // The full screen by default; a file's window kept.
  CHECK(defaults.fullscreen);
  CHECK_FALSE(parse_options("fullscreen=0\n").fullscreen);
  // The music at the original's volume, the sound effects under it.
  CHECK(defaults.music_volume == 100);
  CHECK(defaults.sfx_volume == 60);
  const RemakeOptions set{.variant = game::Variant::kAntagonizer,
                          .fullscreen = false,
                          .scaling = Scaling::kSharp,
                          .music_volume = 0,
                          .sfx_volume = 35,
                          .language = Language::kGerman};
  CHECK(format_options(set) ==
        "variant=antagonizer\nfullscreen=0\nscaling=sharp\nmusic_volume=0\nsfx_volume=35\nlanguage=de\n");
  CHECK(parse_options(format_options(set)) == set);
  CHECK(parse_options(format_options(defaults)) == defaults);
  CHECK(parse_options("# comment\r\n variant = antagonizer # Antagonizer\r\nwide=1\nfullscreen=yes\nscaling\n") ==
        RemakeOptions{.variant = game::Variant::kAntagonizer, .fullscreen = true, .scaling = Scaling::kSmooth});
  // A file of an earlier version (no volumes): the defaults' volumes (the
  // mute flags stay those of ascend.cfg).
  CHECK(parse_options("variant=standard\nfullscreen=0\nscaling=sharp\n") ==
        RemakeOptions{.fullscreen = false, .scaling = Scaling::kSharp, .music_volume = 100, .sfx_volume = 60});
  // Volumes are whole percents.
  CHECK(parse_options("music_volume=101\nsfx_volume=-1\n") == defaults);
  CHECK(parse_options("music_volume=50%\nsfx_volume=0x10\nmusic_volume=\n") == defaults);
  CHECK(parse_options("music_volume = 7\nsfx_volume=100\n") == RemakeOptions{.music_volume = 7, .sfx_volume = 100});

  TempDir dir("oa_options");
  const std::filesystem::path file = dir.path / kOptionsFile;
  CHECK(load_options(file) == defaults);
  REQUIRE(save_options(file, set));
  CHECK(load_options(file) == set);
}

TEST_CASE("the options screen: a pick applies at once, Cancel reverts, OK and Esc keep and save",
          "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  TempDir dir("oa_options");
  test::UiRig rig(files, nullptr, dir.path);
  RemakeOptions options;
  FakeDisplay display;
  const std::filesystem::path file = dir.path / kOptionsFile;
  install(rig, OptionsContext{.options = &options, .file = file, .display = &display});
  rig.sound.set_music_muted(false);
  rig.sound.set_sfx_muted(true);

  rig.click(318, options_y());
  REQUIRE(rig.wm.current_screen() == kScreenOptions);
  rig.move(choice_x(1, kVariant), row_y(kVariant));
  test::write_shot(rig, "options_screen");
  for (const Row row : {kVariant, kDisplay, kScaling}) {
    rig.click(choice_x(1, row), row_y(row));
  }
  // In force at once.
  CHECK(rig.game.new_game_variant() == game::Variant::kAntagonizer);
  CHECK(display.fullscreen());
  CHECK(display.scaling() == Scaling::kSharp);
  rig.click(kCancelX, kButtonsY);
  CHECK(rig.wm.current_screen() == 0);
  CHECK(options == RemakeOptions{});
  CHECK(rig.game.new_game_variant() == game::Variant::kStandard);
  CHECK_FALSE(display.fullscreen());
  CHECK(display.scaling() == Scaling::kSmooth);
  CHECK_FALSE(rig.sound.music_muted());
  CHECK(rig.sound.sfx_muted());
  CHECK_FALSE(std::filesystem::exists(file));

  // Opened again: the picks were reverted. Esc (as Back) keeps and saves.
  rig.click(318, options_y());
  rig.click(choice_x(1, kVariant), row_y(kVariant));
  rig.key(SDLK_ESCAPE);
  CHECK(rig.wm.current_screen() == 0);
  CHECK(rig.game.new_game_variant() == game::Variant::kAntagonizer);
  CHECK(load_options(file).variant == game::Variant::kAntagonizer);

  rig.click(318, options_y());
  for (const Row row : {kVariant, kDisplay, kScaling}) {
    rig.click(choice_x(1, row), row_y(row));
  }
  rig.click(kOkX, kButtonsY);
  CHECK(rig.wm.current_screen() == 0);
  const RemakeOptions expected{.variant = game::Variant::kAntagonizer, .fullscreen = true, .scaling = Scaling::kSharp};
  CHECK(options == expected);
  CHECK(load_options(file) == expected);
  CHECK(rig.game.new_game_variant() == game::Variant::kAntagonizer);
  CHECK(display.fullscreen());
  CHECK(display.scaling() == Scaling::kSharp);
  CHECK_FALSE(rig.sound.music_muted());
  CHECK(rig.sound.sfx_muted());
  // ascend.cfg: the original's 12 bytes, the mute flags (effects, music) as
  // 0 / -1.
  const std::string cfg = read_text(dir.path / "ascend.cfg");
  REQUIRE(cfg.size() == 12);
  CHECK(cfg.substr(0, 8) == std::string("\xff\xff\xff\xff\0\0\0\0", 8));

  // Enter is OK; the screen shows the settings in force (the window's), and
  // leaving it unchanged changes nothing.
  display.set_fullscreen(false);
  rig.click(318, options_y());
  rig.key(SDLK_RETURN);
  CHECK(rig.wm.current_screen() == 0);
  CHECK(options == expected);
  CHECK_FALSE(display.fullscreen());
}

// The level a volume slider of `row` shows (`options_wnd.cpp`: the bar
// 258..610 filled with the selected colour, hot or not, up to 264 + level *
// 340 / 100), read on its top rows, off the caption.
int shown_level(test::UiRig& rig, Row row) {
  const int y = 72 + 60 * row + 2;
  const auto filled = [&](int x) {
    const std::uint8_t c = rig.wm.screen().at(x, y);
    return c == kSelectedColor || c == kSelectedHotColor;
  };
  if (!filled(258)) {
    return 0;
  }
  if (filled(610)) {
    return 100;
  }
  int end = 258;
  while (filled(end + 1)) {
    ++end;
  }
  return ((end - 264) * 100 + 170) / 340;
}

TEST_CASE("Alt+Enter saves the fullscreen setting at once, also on the options screen", "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  TempDir dir("oa_options");
  test::UiRig rig(files, nullptr, dir.path);
  RemakeOptions options{.fullscreen = false};
  FakeDisplay display;
  const std::filesystem::path file = dir.path / kOptionsFile;
  const FullscreenToggled toggled = install(rig, OptionsContext{.options = &options, .file = file, .display = &display});
  // What `Display` does on Alt+Enter.
  const auto alt_enter = [&] {
    display.set_fullscreen(!display.fullscreen());
    toggled(display.fullscreen());
  };
  // A background pixel of choice `c` of the Display row (as in the
  // selected choices' test), off its caption.
  const auto display_choice = [&](int c) { return rig.wm.screen().at(c == 0 ? 261 : 448, 72 + 60 * kDisplay + 3); };

  // On the opening menu.
  alt_enter();
  CHECK(options.fullscreen);
  CHECK(load_options(file) == RemakeOptions{.fullscreen = true});

  // On the options screen, a pick pending: the Display row follows, Cancel
  // reverts the pick and keeps the window's state.
  rig.click(318, options_y());
  REQUIRE(rig.wm.current_screen() == kScreenOptions);
  rig.click(choice_x(1, kVariant), row_y(kVariant));
  CHECK(display_choice(1) == kSelectedColor);
  alt_enter();
  CHECK(load_options(file) == RemakeOptions{.fullscreen = false});
  // Shown at the next pass.
  rig.pass();
  CHECK(display_choice(0) == kSelectedColor);
  CHECK(display_choice(1) == GWindow::kBackColor);
  // Another pick puts the shown rows in force: the window stays as toggled.
  rig.click(choice_x(1, kScaling), row_y(kScaling));
  CHECK_FALSE(display.fullscreen());
  rig.click(kCancelX, kButtonsY);
  CHECK(rig.wm.current_screen() == 0);
  CHECK_FALSE(display.fullscreen());
  CHECK(display.scaling() == Scaling::kSmooth);
  CHECK(rig.game.new_game_variant() == game::Variant::kStandard);
  CHECK(options == RemakeOptions{.fullscreen = false});
  CHECK(load_options(file) == RemakeOptions{.fullscreen = false});

  // OK keeps the picks with the toggled state.
  rig.click(318, options_y());
  rig.click(choice_x(1, kVariant), row_y(kVariant));
  alt_enter();
  rig.pass();
  rig.click(kOkX, kButtonsY);
  const RemakeOptions expected{.variant = game::Variant::kAntagonizer, .fullscreen = true};
  CHECK(options == expected);
  CHECK(load_options(file) == expected);
}

TEST_CASE("the options screen's volume sliders: click, drag, wheel and keys, heard at once", "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  TempDir dir("oa_options");
  test::UiRig rig(files, nullptr, dir.path);
  RemakeOptions options;
  const std::filesystem::path file = dir.path / kOptionsFile;
  install(rig, OptionsContext{.options = &options, .file = file, .display = nullptr});
  rig.sound.set_sfx_muted(true);

  rig.click(318, options_y());
  REQUIRE(rig.wm.current_screen() == kScreenOptions);
  // The music at 100 %, the muted sound effects at 0.
  CHECK(shown_level(rig, kMusic) == 100);
  CHECK(shown_level(rig, kSound) == 0);

  // A click sets the level under the pointer, heard at once.
  rig.click(slider_x(40), row_y(kMusic));
  CHECK(shown_level(rig, kMusic) == 40);
  CHECK(rig.sound.music_level() == 40);
  CHECK(rig.mixer.music_level() == 40);
  CHECK_FALSE(rig.sound.music_muted());
  // The sound effects unmuted at the new level, one played to hear it.
  rig.mixer.stop_all_sfx();
  rig.click(slider_x(80), row_y(kSound));
  CHECK(shown_level(rig, kSound) == 80);
  CHECK_FALSE(rig.sound.sfx_muted());
  CHECK(rig.mixer.sfx_level() == 80);
  CHECK(rig.mixer.sfx_playing(0));
  // The ends of the bar are 0 and 100.
  rig.click(611 - 3, row_y(kMusic));
  CHECK(rig.sound.music_level() == 100);
  rig.move(100, 400);
  test::write_shot(rig, "options_volume");

  // A drag of the mouse moves the level with the pointer, past the press.
  rig.pass(
      InputEvent{.type = EventType::kMouseDown, .x = slider_x(60), .y = row_y(kMusic), .button = MouseButton::kLeft});
  CHECK(rig.sound.music_level() == 60);
  for (const int level : {50, 30, 20}) {
    rig.move(slider_x(level), row_y(kMusic) + 20);
    CHECK(rig.sound.music_level() == level);
  }
  rig.pass(InputEvent{
      .type = EventType::kMouseUp, .x = slider_x(20), .y = row_y(kMusic) + 20, .button = MouseButton::kLeft});
  CHECK(shown_level(rig, kMusic) == 20);
  // Dragged to the far left: muted, the level kept for Alt+M.
  rig.pass(
      InputEvent{.type = EventType::kMouseDown, .x = slider_x(20), .y = row_y(kMusic), .button = MouseButton::kLeft});
  rig.move(100, row_y(kMusic));
  rig.pass(InputEvent{.type = EventType::kMouseUp, .x = 100, .y = row_y(kMusic), .button = MouseButton::kLeft});
  CHECK(shown_level(rig, kMusic) == 0);
  CHECK(rig.sound.music_muted());
  CHECK(rig.sound.music_level() == 20);

  // A finger: a tap sets the level, a drag moves it from where it landed.
  TouchMapper touch(rig.input);
  const auto finger = [&](EventType type, int x, std::uint64_t ms) {
    touch.feed(InputEvent{.type = type, .x = x, .y = row_y(kSound), .finger = 1, .time_ms = ms});
    rig.pass();
  };
  finger(EventType::kFingerDown, slider_x(30), 0);
  finger(EventType::kFingerUp, slider_x(30), 50);
  rig.pass();
  CHECK(rig.sound.sfx_level() == 30);
  finger(EventType::kFingerDown, slider_x(30), 1000);
  finger(EventType::kFingerMove, slider_x(45), 1050);
  CHECK(rig.sound.sfx_level() == 45);
  finger(EventType::kFingerMove, slider_x(70), 1100);
  finger(EventType::kFingerUp, slider_x(70), 1150);
  CHECK(rig.sound.sfx_level() == 70);
  CHECK(shown_level(rig, kSound) == 70);

  // The wheel and the arrow keys step the slider under the pointer by 5.
  rig.pass(InputEvent{.type = EventType::kMouseWheel, .x = slider_x(50), .y = row_y(kSound), .wheel = 1});
  CHECK(rig.sound.sfx_level() == 75);
  rig.key(SDLK_LEFT);
  rig.key(SDLK_LEFT);
  CHECK(rig.sound.sfx_level() == 65);
  CHECK(shown_level(rig, kSound) == 65);

  // Cancel: the volumes heard before.
  rig.click(kCancelX, kButtonsY);
  CHECK(rig.wm.current_screen() == 0);
  CHECK_FALSE(rig.sound.music_muted());
  CHECK(rig.sound.music_level() == 100);
  CHECK(rig.mixer.music_level() == 100);
  CHECK(rig.sound.sfx_muted());
  CHECK(rig.sound.sfx_level() == 60);
  CHECK(rig.mixer.sfx_level() == 60);
  CHECK(options == RemakeOptions{});
  CHECK_FALSE(std::filesystem::exists(file));

  // Esc keeps them.
  rig.click(318, options_y());
  rig.click(slider_x(25), row_y(kMusic));
  rig.key(SDLK_ESCAPE);
  CHECK(rig.wm.current_screen() == 0);
  CHECK(rig.sound.music_level() == 25);
  CHECK(load_options(file).music_volume == 25);

  // OK keeps them: the levels in options.cfg, a muted music in ascend.cfg.
  rig.click(318, options_y());
  rig.click(slider_x(85), row_y(kSound));
  rig.click(slider_x(55), row_y(kMusic));
  rig.pass(
      InputEvent{.type = EventType::kMouseDown, .x = slider_x(55), .y = row_y(kMusic), .button = MouseButton::kLeft});
  rig.move(100, row_y(kMusic));
  rig.pass(InputEvent{.type = EventType::kMouseUp, .x = 100, .y = row_y(kMusic), .button = MouseButton::kLeft});
  CHECK(rig.sound.music_muted());
  rig.click(kOkX, kButtonsY);
  CHECK(rig.wm.current_screen() == 0);
  CHECK(options.music_volume == 55);
  CHECK(options.sfx_volume == 85);
  CHECK(load_options(file) == options);
  CHECK(rig.sound.music_muted());
  CHECK_FALSE(rig.sound.sfx_muted());
  const std::string cfg = read_text(dir.path / "ascend.cfg");
  REQUIRE(cfg.size() == 12);
  CHECK(cfg.substr(0, 8) == std::string("\0\0\0\0\xff\xff\xff\xff", 8));
}

TEST_CASE("Alt+M and Alt+N mute and unmute at the sliders' volumes", "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  test::UiRig rig(files);
  RemakeOptions options{.music_volume = 70, .sfx_volume = 45};
  install(rig, OptionsContext{.options = &options, .file = {}, .display = nullptr});
  CHECK(rig.mixer.music_level() == 70);
  CHECK(rig.mixer.sfx_level() == 45);
  rig.key(SDLK_N, SDL_KMOD_LALT);
  CHECK(rig.sound.sfx_muted());
  CHECK(rig.sound.sfx_level() == 45);
  rig.click(318, options_y());
  REQUIRE(rig.wm.current_screen() == kScreenOptions);
  CHECK(shown_level(rig, kMusic) == 70);
  CHECK(shown_level(rig, kSound) == 0);
  // On the options screen, the sliders follow the keys.
  rig.key(SDLK_N, SDL_KMOD_LALT);
  rig.pass();
  CHECK_FALSE(rig.sound.sfx_muted());
  CHECK(shown_level(rig, kSound) == 45);
  rig.key(SDLK_M, SDL_KMOD_LALT);
  rig.pass();
  CHECK(rig.sound.music_muted());
  CHECK(shown_level(rig, kMusic) == 0);
  // A muted music unmuted by its slider, at the slider's level.
  rig.click(slider_x(35), row_y(kMusic));
  CHECK_FALSE(rig.sound.music_muted());
  CHECK(rig.sound.music_level() == 35);
  rig.key(SDLK_RETURN);
  CHECK(options.music_volume == 35);
  CHECK(options.sfx_volume == 45);
  rig.key(SDLK_M, SDL_KMOD_LALT);
  rig.key(SDLK_M, SDL_KMOD_LALT);
  CHECK_FALSE(rig.sound.music_muted());
  CHECK(rig.mixer.music_level() == 35);
}

TEST_CASE("the options screen's help: Shift and a click", "[screens][content]") {
  OA_REQUIRE_GAME_FILES(files);
  test::UiRig rig(files);
  RemakeOptions options;
  install(rig, OptionsContext{.options = &options, .file = {}, .display = nullptr});
  const auto help_click = [&](int x, int y) {
    rig.pending.push_back(InputEvent{.type = EventType::kKeyDown, .key = SDLK_LSHIFT, .mods = SDL_KMOD_LSHIFT});
    rig.pending.push_back(InputEvent{.type = EventType::kMouseMove, .x = x, .y = y});
    rig.queue_click(x, y);
    rig.pending.push_back(InputEvent{.type = EventType::kKeyUp, .key = SDLK_LSHIFT, .mods = 0});
    // The help window's modal loop takes the rest: its OK closes it.
    rig.queue_click(320, 350);
    rig.drain();
  };
  auto& help = dynamic_cast<HelpWnd&>(*rig.wm.find_window("HELPWINDOW"));
  help_click(318, options_y());
  CHECK(help.topic() == "oa_options");
  rig.click(318, options_y());
  REQUIRE(rig.wm.current_screen() == kScreenOptions);
  help_click(choice_x(0, kScaling), row_y(kScaling));
  CHECK(help.topic() == "oa_scaling");
  help_click(slider_x(50), row_y(kSound));
  CHECK(help.topic() == "oa_sound");
  CHECK(rig.sound.sfx_level() == 60);
  help_click(100, row_y(kVariant));
  CHECK(help.topic() == "oa_variant");
  help_click(100, 412);
  CHECK(help.topic() == "oa_screen");
  CHECK(rig.wm.current_screen() == kScreenOptions);
}

// Clears the stale stack bytes of a save header: the per-race entries past
// the race count and the last 8 bytes.
void clear_stale_header(std::vector<std::uint8_t>& save) {
  const int races = save[4];
  for (const std::size_t table : {0x0cU, 0x28U, 0x44U}) {
    for (int r = races; r < 7; ++r) {
      std::fill_n(save.begin() + static_cast<std::ptrdiff_t>(table + 4 * static_cast<std::size_t>(r)), 4, 0);
    }
  }
  std::fill_n(save.begin() + 0x6c, 8, 0);
}

TEST_CASE("an Antagonizer new game from the options is the original Antagonizer's", "[screens][content][oracle]") {
  OA_REQUIRE_GAME_FILES(files);
  int found = 0;
  for (const int seed : {1, 2, 3, 12345}) {
    const std::string run = std::format("corpus/day0_antag_s{}/out/", seed);
    const auto new_game_shot = test::oracle_path(run + "SCR000.GIF");
    const auto save = test::oracle_path(run + "02.SAV");
    if (!new_game_shot || !save) {
      continue;
    }
    ++found;
    INFO(run);
    TempDir dir("oa_options");
    // `main` with options.cfg's variant: the start-up galaxy under its rules.
    GameCoreSession session(files, dir.path, false);
    session.set_new_game_variant(game::Variant::kAntagonizer);
    const game::CosmosTxt txt{.stars = 50, .races = 7, .seed = seed, .species = {}};
    session.start(0, &txt);
    test::UiRig rig(files, &session, dir.path);
    RemakeOptions options{.variant = game::Variant::kAntagonizer};
    install(rig, OptionsContext{.options = &options, .file = {}, .display = nullptr});

    // The steps of the oracle's `day0_default.txt` (`star_map_test.cpp`),
    // New Game on its button of the remake's menu.
    const auto wait = [&](int passes) {
      for (int i = 0; i < passes; ++i) {
        rig.pass();
      }
    };
    wait(40);
    rig.move(318, 282);
    rig.click(318, 282);
    wait(60);
    REQUIRE(rig.wm.current_screen() == 3);
    // The original's screen (with its pointer): the variant shows nowhere.
    const Image8 original = decode_gif(test::read_file(*new_game_shot), new_game_shot->string());
    int differing = 0;
    for (int y = 0; y < original.height(); ++y) {
      for (int x = 0; x < original.width(); ++x) {
        // The original's pointer, on its New Game button.
        const bool pointer = x >= 318 - 16 && x <= 318 + 16 && y >= 295 - 16 && y <= 295 + 16;
        if (pointer || original.at(x, y) == rig.wm.screen().at(x, y)) {
          continue;
        }
        ++differing;
      }
    }
    CHECK(differing == 0);
    if (seed == 1) {
      test::write_shot(rig, "options_new_game_antagonizer");
    }
    rig.pass();
    wait(2);
    rig.move(594, 452);
    rig.click(594, 452);  // Begin New Game
    wait(60);
    rig.move(320, 240);
    rig.click(320, 240);  // the species history
    wait(60);
    REQUIRE(rig.wm.current_screen() == 1);
    REQUIRE(session.save(2));
    std::vector<std::uint8_t> ours = test::read_file(dir.path / "02.SAV");
    std::vector<std::uint8_t> theirs = test::read_file(*save);
    // The remake's variant trailer (`game/save.h`) after the original's bytes.
    REQUIRE(ours.size() == theirs.size() + 8);
    CHECK(game::parse_save(ours).variant == game::Variant::kAntagonizer);
    ours.resize(theirs.size());
    clear_stale_header(ours);
    clear_stale_header(theirs);
    CHECK(ours == theirs);

    // Loaded back by a Standard session, the game is the Antagonizer's.
    GameCoreSession loader(files, dir.path, false);
    REQUIRE(loader.load(2));
    CHECK(loader.variant() == game::Variant::kAntagonizer);
  }
  if (found == 0) {
    SKIP("OA_ORACLE is not set to work/oracle with the day-0 corpus");
  }
}

}  // namespace
}  // namespace oa
