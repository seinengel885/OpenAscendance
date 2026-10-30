#include "screens/options_wnd.h"

#include <algorithm>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "assets/gif.h"
#include "audio/mixer.h"
#include "audio/sound_system.h"
#include "core/error.h"
#include "core/language.h"
#include "platform/input.h"
#include "ui/remake_help.h"

namespace oa {

namespace {

constexpr std::string_view kOpeningGif = "DATA\\0OPENING.GIF";
constexpr std::string_view kBackground = "DATA\\10status.tmp";

// The stone frame of `0OPENING.GIF`'s menu panel around its buttons: its
// width, and the panel's rectangle.
constexpr int kFrame = 7;
constexpr Pane kMenuPanel{152, 193, 484, 477};

// The options screen: the title bar and the body of `10status.tmp`.
constexpr Pane kTitlePane{7, 7, 632, 37};
constexpr Pane kBodyPane{7, 46, 632, 472};
// The settings' rows: the first one's top, the rows' pitch and height, the
// labels' and the choices' x ranges, the space between two choices'
// frames.
constexpr int kRowTop = 72;
constexpr int kRowPitch = 60;
constexpr int kRowHeight = 28;
constexpr int kLabelX0 = 30;
constexpr int kLabelX1 = 240;
constexpr int kChoicesX0 = 258;
constexpr int kChoicesX1 = 610;
constexpr int kChoiceGap = 2 * kFrame + 8;
// The top of the OK and Cancel buttons' row.
constexpr int kButtonsTop = 424;

// The messages of the screen's buttons to the screen (p1 the row, p2 the
// choice).
constexpr int kMessagePick = 600;
constexpr int kMessageOk = 601;
constexpr int kMessageCancel = 602;
// The messages of the volume sliders to the screen (p1 the row): a new
// level (p2), the end of a change (p2 the level); and the screen's timer
// that follows the mute keys.
constexpr int kMessageLevel = 603;
constexpr int kMessageLevelDone = 604;
constexpr int kMessageSync = 605;
// A volume slider's step (a wheel notch, an arrow key), and the margin at
// each end of its bar where a press is 0 or 100.
constexpr int kLevelStep = 5;
constexpr int kSliderMargin = 6;
constexpr std::uint16_t kScanLeft = 0x4b;
constexpr std::uint16_t kScanRight = 0x4d;
// The click sound of the menu's buttons (`soundfx.txt` 0, SOUNDCHANNEL 0).
constexpr int kClickSound = 0;
constexpr std::uint16_t kEnterKey = '\r';

// Returns the pane of choice `c` of `n` in the row whose top is `y0`.
Pane choice_pane(int c, int n, int y0) {
  const int width = (kChoicesX1 - kChoicesX0 + 1 - (n - 1) * kChoiceGap) / n;
  const int x0 = kChoicesX0 + c * (width + kChoiceGap);
  return Pane{x0, y0, x0 + width - 1, y0 + kRowHeight - 1};
}

// Draws the stone frame of the opening menu's panel (`gif`) around
// `inner`: its corners, and its edges cut from the panel's (half from each
// end of the panel's edge, so that the bevels meet).
void draw_stone_frame(Image8& screen, const Image8& gif, const Pane& inner) {
  const Pane& src = kMenuPanel;
  const int src_w = (src.x1 - kFrame) - (src.x0 + kFrame) + 1;
  const int src_h = (src.y1 - kFrame) - (src.y0 + kFrame) + 1;
  // The source coordinate of offset `k` of a length `n` edge.
  const auto edge = [](int k, int n, int first, int last) { return k < n / 2 ? first + k : last - (n - 1 - k); };
  const int w = inner.x1 - inner.x0 + 1;
  const int h = inner.y1 - inner.y0 + 1;
  const auto copy = [&](int dx, int dy, int sx, int sy) {
    if (screen.contains(dx, dy) && gif.contains(sx, sy)) {
      screen.set(dx, dy, gif.at(sx, sy));
    }
  };
  for (int f = 0; f < kFrame; ++f) {
    for (int k = 0; k < w + 2 * kFrame; ++k) {
      // Top and bottom rows, corners included.
      const int dx = inner.x0 - kFrame + k;
      int sx = 0;
      if (k < kFrame) {
        sx = src.x0 + k;
      } else if (k >= w + kFrame) {
        sx = src.x1 - (w + 2 * kFrame - 1 - k);
      } else {
        sx = edge(std::min(k - kFrame, src_w - 1), std::min(w, src_w), src.x0 + kFrame, src.x1 - kFrame);
      }
      copy(dx, inner.y0 - kFrame + f, sx, src.y0 + f);
      copy(dx, inner.y1 + 1 + f, sx, src.y1 - kFrame + 1 + f);
    }
    for (int k = 0; k < h; ++k) {
      const int sy = edge(std::min(k, src_h - 1), std::min(h, src_h), src.y0 + kFrame, src.y1 - kFrame);
      copy(inner.x0 - kFrame + f, inner.y0 + k, src.x0 + f, sy);
      copy(inner.x1 + 1 + f, inner.y0 + k, src.x1 - kFrame + 1 + f, sy);
    }
  }
}

// A text button of the remake: the toolkit's text window (0xf2, hot 0x96),
// in `kSelectedColor` (hot `kSelectedHotColor`) while selected, whose help
// is a remake topic.
class RemakeButton final : public GWindow {
 public:
  RemakeButton(WinMgr& wm, std::string caption, const Pane& pane, std::string help_topic)
      : GWindow(wm, std::move(caption), pane) {
    mouse_focus_ = true;
    sound_ = kClickSound;
    help_ = std::move(help_topic);
  }

  void set_selected(bool selected) { selected_ = selected; }

  bool handle_event(int type, int a, int b) override {
    if (type == kEventHelpClick) {
      if (!contains(a, b)) {
        return false;
      }
      show_remake_help(wm_, help_);
      return true;
    }
    return GWindow::handle_event(type, a, b);
  }

  void draw(int mode) override {
    if (!active_) {
      return;
    }
    if ((wm_.hot_window() == id_ || mode == kDrawHot) && mode != kDrawNotHot) {
      draw_hilite(mode);
    } else {
      draw_text(selected_ ? kSelectedColor : kBackColor);
    }
  }
  void draw_hilite(int /*mode*/) override { draw_text(selected_ ? kSelectedHotColor : kHotBackColor); }

 private:
  bool selected_ = false;
};

// A volume slider of the remake, as wide as a row's choices: a text window
// (0xf2, hot 0x96) filled from the left up to its level in `kSelectedColor`
// (hot `kSelectedHotColor`), the level as its caption ("Off" at 0). A
// click or a tap sets the level under the pointer, a drag of the mouse or
// a finger moves it, the wheel and the arrow keys (the pointer over it)
// step it. Sends `kMessageLevel` (row, level) to its parent at each
// change, `kMessageLevelDone` (row, level) at the end of a click, drag,
// notch or key.
class VolumeSlider final : public GWindow {
 public:
  VolumeSlider(WinMgr& wm, const Pane& pane, int row, std::string help_topic) : GWindow(wm, "", pane), row_(row) {
    mouse_focus_ = true;
    help_ = std::move(help_topic);
  }

  int level() const { return level_; }
  // Shows `level` (0..100), without a message.
  void set_level(int level) {
    level_ = std::clamp(level, 0, Mixer::kMaxLevel);
    draw(kDrawNormal);
  }

  bool handle_event(int type, int a, int b) override {
    switch (type) {
      case kEventLeftClick:
        if (!contains(a, b)) {
          return false;
        }
        change(level_at(a));
        done();
        return true;
      case kEventRightClick:
        return contains(a, b);
      case kEventHelpClick:
        if (!contains(a, b)) {
          return false;
        }
        show_remake_help(wm_, help_);
        return true;
      case kEventKey:
        if (wm_.hot_window() != id_ || (b != kScanLeft && b != kScanRight)) {
          return false;
        }
        change(level_ + (b == kScanRight ? kLevelStep : -kLevelStep));
        done();
        return true;
      default:
        return GWindow::handle_event(type, a, b);
    }
  }

  void press_areas(std::vector<PressArea>& out) const override { out.push_back(PressArea{pane_, true}); }
  bool takes_view_gesture(int x, int y, bool /*mouse*/) const override { return contains(x, y); }
  void view_drag(int dx, int /*dy*/) override {
    // The gesture's drags add up from where it started (the press).
    if (const ViewGesture& g = wm_.input().gesture; g.started) {
      drag_x_ = g.x;
    }
    drag_x_ += dx;
    dragging_ = true;
    change(level_at(drag_x_));
  }
  void view_wheel(int notches) override {
    change(level_ + notches * kLevelStep);
    done();
  }
  void view_end() override {
    if (dragging_) {
      dragging_ = false;
      done();
    }
  }

  void draw(int mode) override {
    if (active_) {
      draw_bar((wm_.hot_window() == id_ || mode == kDrawHot) && mode != kDrawNotHot);
    }
  }
  void draw_hilite(int /*mode*/) override { draw_bar(true); }

 private:
  int inner_width() const { return pane_.x1 - pane_.x0 - 2 * kSliderMargin; }

  // The level at screen x `x`.
  int level_at(int x) const {
    const int w = inner_width();
    const int offset = std::clamp(x - pane_.x0 - kSliderMargin, 0, w);
    return (offset * Mixer::kMaxLevel + w / 2) / w;
  }

  // The last column of the filled part (`pane_.x0 - 1` for none).
  int fill_end() const {
    if (level_ == 0) {
      return pane_.x0 - 1;
    }
    if (level_ == Mixer::kMaxLevel) {
      return pane_.x1;
    }
    return pane_.x0 + kSliderMargin + (level_ * inner_width() + Mixer::kMaxLevel / 2) / Mixer::kMaxLevel;
  }

  void draw_bar(bool hot) {
    Image8& screen = wm_.screen();
    const int end = fill_end();
    if (end >= pane_.x0) {
      vfx_pane_wipe(screen, Pane{pane_.x0, pane_.y0, end, pane_.y1}, hot ? kSelectedHotColor : kSelectedColor);
    }
    if (end < pane_.x1) {
      vfx_pane_wipe(screen, Pane{end + 1, pane_.y0, pane_.x1, pane_.y1}, hot ? kHotBackColor : kBackColor);
    }
    GameFont& font = wm_.large_font();
    font.pane() = pane_;
    const std::string caption = level_ == 0 ? wm_.remake_text("Off") : std::format("{} %", level_);
    font.draw_wrapped(screen, 0, 0, caption, kFontCenterH | kFontCenterV, kDefault, 0xff, 0);
  }

  void change(int level) {
    level = std::clamp(level, 0, Mixer::kMaxLevel);
    if (level == level_) {
      return;
    }
    set_level(level);
    wm_.send_event(parent_, kMessageLevel, row_, level_);
  }
  void done() { wm_.send_event(parent_, kMessageLevelDone, row_, level_); }

  int row_;
  int level_ = 0;
  // The pointer's x in a drag (the start plus the drags so far).
  int drag_x_ = 0;
  bool dragging_ = false;
};

// A row of the options screen: choice buttons, or a volume slider.
struct Setting {
  std::string_view label;
  std::string_view note;  // under the label, in the small font
  std::string_view help;
  std::vector<std::string_view> choices;  // none for a slider
};

// The rows, in order (their indices are `OptionsWnd`'s).
enum SettingRow : std::size_t { kRowVariant, kRowDisplay, kRowScaling, kRowMusic, kRowSound, kRowLanguage };

std::vector<Setting> settings() {
  std::vector<std::string_view> variants;
  for (const game::VariantInfo& v : game::kVariants) {
    variants.push_back(v.label);
  }
  std::vector<std::string_view> languages;
  for (const LanguageInfo& l : kLanguages) {
    languages.push_back(l.label);
  }
  return {
      {"Game Variant", "For the next new game", "oa_variant", std::move(variants)},
      {"Display", "", "oa_display", {"Window", "Full Screen"}},
      {"Scaling", "", "oa_scaling", {"Smooth", "Sharp"}},
      {"Music", "", "oa_music", {}},
      {"Sound Effects", "", "oa_sound", {}},
      {"Language", "From the next start", "oa_language", std::move(languages)},
  };
}

// The options screen: the settings' rows on the body of `10status.tmp`,
// OK and Cancel. Its buttons are its children.
class OptionsWnd final : public GWindow {
 public:
  OptionsWnd(WinMgr& wm, OptionsContext context, std::shared_ptr<const Image8> gif)
      : GWindow(wm, "Options", kBodyPane), context_(std::move(context)), gif_(std::move(gif)), settings_(settings()) {
    selected_.assign(settings_.size(), 0);
    sliders_.assign(settings_.size(), nullptr);
  }

  // Creates the buttons (registered as children, on the options screen).
  void add_buttons() {
    for (std::size_t row = 0; row < settings_.size(); ++row) {
      const Setting& s = settings_[row];
      const int n = static_cast<int>(s.choices.size());
      const int y0 = kRowTop + static_cast<int>(row) * kRowPitch;
      std::vector<RemakeButton*>& buttons = rows_.emplace_back();
      if (n == 0) {
        sliders_[row] =
            add(std::make_unique<VolumeSlider>(wm_, choice_pane(0, 1, y0), static_cast<int>(row), std::string(s.help)));
        continue;
      }
      for (int c = 0; c < n; ++c) {
        auto button = std::make_unique<RemakeButton>(wm_, wm_.remake_text(s.choices[static_cast<std::size_t>(c)]),
                                                     choice_pane(c, n, y0), std::string(s.help));
        button->set_message(kMessagePick, static_cast<int>(row), c);
        buttons.push_back(add(std::move(button)));
      }
    }
    auto ok =
        std::make_unique<RemakeButton>(wm_, wm_.static_string(kOkString), choice_pane(0, 2, kButtonsTop), "oa_ok");
    ok->set_message(kMessageOk, 0, 0);
    add(std::move(ok));
    auto cancel =
        std::make_unique<RemakeButton>(wm_, wm_.remake_text("Cancel"), choice_pane(1, 2, kButtonsTop), "oa_cancel");
    cancel->set_message(kMessageCancel, 0, 0);
    add(std::move(cancel));
  }

  // `FullscreenToggled`: saved at once; on this screen, kept by Cancel too,
  // the other settings' picks still pending.
  void fullscreen_toggled(bool fullscreen) {
    context_.options->fullscreen = fullscreen;
    if (open_) {
      saved_options_.fullscreen = fullscreen;
      saved_display_.fullscreen = fullscreen;
    }
    if (!context_.file.empty()) {
      save_options(context_.file, open_ ? saved_options_ : *context_.options);
    }
  }

  bool handle_event(int type, int a, int b) override {
    switch (type) {
      case kEventActivate:
        read_settings();
        cancelled_ = false;
        open_ = true;
        wm_.set_timer(id_, kMessageSync, 0, 0, 0);
        return GWindow::handle_event(type, a, b);
      case kEventDeactivate:
        open_ = false;
        if (cancelled_) {
          restore();
        } else {
          save();
        }
        return GWindow::handle_event(type, a, b);
      case kEventHelpClick:
        if (!contains(a, b)) {
          return false;
        }
        show_remake_help(wm_, help_for(b));
        return true;
      case kEventKey:
        if (a != kEnterKey) {
          return false;
        }
        wm_.services().sound.play_sfx(kClickSound);
        wm_.go_back();
        return true;
      case kMessagePick:
        pick(static_cast<std::size_t>(a), b);
        return true;
      case kMessageOk:
        wm_.go_back();
        return true;
      case kMessageCancel:
        cancelled_ = true;
        wm_.go_back();
        return true;
      case kMessageLevel:
        set_level(static_cast<std::size_t>(a), b);
        return true;
      case kMessageLevelDone:
        if (a == kRowSound && b > 0) {
          wm_.services().sound.play_sfx(kClickSound);
        }
        return true;
      case kMessageSync:
        sync_sliders();
        sync_display();
        return true;
      default:
        return GWindow::handle_event(type, a, b);
    }
  }

  void draw(int /*mode*/) override {
    Image8& screen = wm_.screen();
    GameFont& large = wm_.large_font();
    large.pane() = kTitlePane;
    large.draw_wrapped(screen, 0, 4, wm_.remake_text(name_), kFontCenterH | kFontCenterV, kDefault, 0xff, 0);
    vfx_pane_wipe(screen, pane_, kBackColor);
    GameFont& small = wm_.small_font();
    for (std::size_t row = 0; row < settings_.size(); ++row) {
      const Setting& s = settings_[row];
      const int y0 = kRowTop + static_cast<int>(row) * kRowPitch;
      const Pane label{kLabelX0, y0, kLabelX1, y0 + kRowHeight - 1};
      large.pane() = label;
      if (s.note.empty()) {
        large.draw_wrapped(screen, 0, 0, wm_.remake_text(s.label), kFontCenterV, kDefault, 0xff, 0);
      } else {
        large.draw_wrapped(screen, 0, 1, wm_.remake_text(s.label), 0, kDefault, 0xff, 0);
        small.pane() = label;
        small.draw_wrapped(screen, 0, large.font().height + 4, wm_.remake_text(s.note), 0, kDefault, 0xff, 0);
      }
    }
    for (GWindow* child : children_) {
      draw_stone_frame(screen, *gif_, child->pane());
    }
    draw_children();
  }
  void draw_hilite(int mode) override { draw(mode); }

 private:
  // The static string "OK".
  static constexpr int kOkString = 0x1e;

  template <typename T>
  T* add(std::unique_ptr<T> button) {
    T* raw = button.get();
    raw->set_parent(id_);
    const int id = wm_.register_window(std::move(button));
    add_child(raw);
    wm_.add_to_screen(id, kScreenOptions);
    return raw;
  }

  // The help topic of a click at height `y` of the body: its row's, else
  // the screen's.
  std::string_view help_for(int y) const {
    for (std::size_t row = 0; row < settings_.size(); ++row) {
      const int y0 = kRowTop + static_cast<int>(row) * kRowPitch;
      if (y >= y0 - kFrame && y <= y0 + kRowHeight - 1 + kFrame) {
        return settings_[row].help;
      }
    }
    return "oa_screen";
  }

  void select(std::size_t row, int choice) {
    selected_[row] = choice;
    for (std::size_t c = 0; c < rows_[row].size(); ++c) {
      rows_[row][c]->set_selected(static_cast<int>(c) == choice);
    }
  }

  // The settings in force.
  void read_settings() {
    const RemakeOptions& o = *context_.options;
    const game::Variant variant = wm_.services().game.new_game_variant();
    for (std::size_t i = 0; i < game::kVariants.size(); ++i) {
      if (game::kVariants[i].variant == variant) {
        select(kRowVariant, static_cast<int>(i));
      }
    }
    const bool fullscreen = context_.display != nullptr ? context_.display->fullscreen() : o.fullscreen;
    const Scaling scaling = context_.display != nullptr ? context_.display->scaling() : o.scaling;
    select(kRowDisplay, fullscreen ? 1 : 0);
    select(kRowScaling, scaling == Scaling::kSharp ? 1 : 0);
    for (std::size_t i = 0; i < kLanguages.size(); ++i) {
      if (kLanguages[i].language == o.language) {
        select(kRowLanguage, static_cast<int>(i));
      }
    }
    saved_options_ = o;
    saved_display_ = DisplayState{.variant = variant, .fullscreen = fullscreen, .scaling = scaling};
    const SoundSystem& sound = wm_.services().sound;
    saved_sound_ = SoundState{.music_muted = sound.music_muted(),
                              .sfx_muted = sound.sfx_muted(),
                              .music_level = sound.music_level(),
                              .sfx_level = sound.sfx_level()};
    sync_sliders();
  }

  // The Display row shows the window's state (Alt+Enter too).
  void sync_display() {
    if (context_.display == nullptr) {
      return;
    }
    const int choice = context_.display->fullscreen() ? 1 : 0;
    if (selected_[kRowDisplay] != choice) {
      select(kRowDisplay, choice);
      for (RemakeButton* button : rows_[kRowDisplay]) {
        button->draw(kDrawNormal);
      }
    }
  }

  // The sliders show the volumes heard: 0 while muted (Alt+M / Alt+N too).
  void sync_sliders() {
    const SoundSystem& sound = wm_.services().sound;
    const auto show = [](VolumeSlider* slider, bool muted, int level) {
      if (const int heard = muted ? 0 : level; slider->level() != heard) {
        slider->set_level(heard);
      }
    };
    show(sliders_[kRowMusic], sound.music_muted(), sound.music_level());
    show(sliders_[kRowSound], sound.sfx_muted(), sound.sfx_level());
  }

  // A slider's level heard at once: 0 mutes (the level kept for Alt+M /
  // Alt+N), another one unmutes at that level.
  void set_level(std::size_t row, int level) {
    SoundSystem& sound = wm_.services().sound;
    if (row == kRowMusic) {
      sound.set_music_muted(level == 0);
      if (level > 0) {
        sound.set_music_level(level);
      }
    } else if (row == kRowSound) {
      sound.set_sfx_muted(level == 0);
      if (level > 0) {
        sound.set_sfx_level(level);
      }
    }
  }

  // A choice applied at once.
  void pick(std::size_t row, int choice) {
    if (row >= rows_.size() || selected_[row] == choice) {
      return;
    }
    select(row, choice);
    for (RemakeButton* button : rows_[row]) {
      button->draw(kDrawNormal);
    }
    RemakeOptions& o = *context_.options;
    o.variant = game::kVariants.at(static_cast<std::size_t>(selected_[kRowVariant])).variant;
    o.fullscreen = selected_[kRowDisplay] == 1;
    o.scaling = selected_[kRowScaling] == 1 ? Scaling::kSharp : Scaling::kSmooth;
    o.language = kLanguages.at(static_cast<std::size_t>(selected_[kRowLanguage])).language;
    apply(DisplayState{.variant = o.variant, .fullscreen = o.fullscreen, .scaling = o.scaling});
  }

  // The settings the choice rows put in force (the sound is the sliders').
  struct DisplayState {
    game::Variant variant = game::Variant::kStandard;
    bool fullscreen = false;
    Scaling scaling = Scaling::kSmooth;
  };

  void apply(const DisplayState& d) {
    wm_.services().game.set_new_game_variant(d.variant);
    if (context_.display != nullptr) {
      if (context_.display->fullscreen() != d.fullscreen) {
        context_.display->set_fullscreen(d.fullscreen);
      }
      context_.display->set_scaling(d.scaling);
    }
  }

  // Leaving by OK, Enter, Esc or Back: the settings in force saved, when
  // they changed.
  void save() {
    RemakeOptions& o = *context_.options;
    const SoundSystem& sound = wm_.services().sound;
    o.music_volume = sound.music_level();
    o.sfx_volume = sound.sfx_level();
    const bool mutes_changed =
        sound.music_muted() != saved_sound_.music_muted || sound.sfx_muted() != saved_sound_.sfx_muted;
    if (o == saved_options_ && !mutes_changed) {
      return;
    }
    if (!context_.file.empty()) {
      save_options(context_.file, o);
    }
    wm_.save_config();
  }

  // Cancel: the settings and the sound of the screen's entry back in force.
  void restore() {
    *context_.options = saved_options_;
    apply(saved_display_);
    SoundSystem& sound = wm_.services().sound;
    sound.set_music_muted(saved_sound_.music_muted);
    sound.set_sfx_muted(saved_sound_.sfx_muted);
    sound.set_music_level(saved_sound_.music_level);
    sound.set_sfx_level(saved_sound_.sfx_level);
  }

  OptionsContext context_;
  // `0OPENING.GIF`, where the buttons' frames come from.
  std::shared_ptr<const Image8> gif_;
  std::vector<Setting> settings_;
  std::vector<int> selected_;
  std::vector<std::vector<RemakeButton*>> rows_;
  // The rows' sliders (null for the choice rows).
  std::vector<VolumeSlider*> sliders_;

  // The settings and the sound at the screen's entry, back on Cancel.
  RemakeOptions saved_options_;
  DisplayState saved_display_;
  struct SoundState {
    bool music_muted = false;
    bool sfx_muted = false;
    int music_level = Mixer::kMaxLevel;
    int sfx_level = Mixer::kMaxLevel;
  };
  SoundState saved_sound_;
  bool cancelled_ = false;
  // Between the screen's entry and its exit.
  bool open_ = false;
};

// The opening menu's two layouts, applied when the menu is entered.
class MenuLayout {
 public:
  MenuLayout(WinMgr& wm, std::shared_ptr<const Image8> gif) : wm_(wm), gif_(std::move(gif)) {
    for (const std::string_view name : kMenuButtons) {
      GWindow* w = wm.find_window(name);
      if (w == nullptr) {
        throw DataError(std::format("windows.txt: no {} on the opening menu", name));
      }
      buttons_.push_back(w);
      panes_.push_back(w->pane());
      items_.push_back(w->items());
    }
    spread_ = spread_menu_buttons(panes_, static_cast<int>(panes_.size()) + 1);
    auto options =
        std::make_unique<RemakeButton>(wm, wm.remake_text("Options"), spread_[kOptionsButtonIndex], "oa_options");
    options->set_message(kMessageChangeScreen, kScreenOptions, 1);
    options_id_ = wm.register_window(std::move(options));
  }

  // The screen's entry: the background and the buttons' layout, the
  // original one while a trace is replayed.
  void enter() {
    const bool original = wm_.replaying();
    if (original == on_menu_) {
      if (original) {
        wm_.remove_from_screen(options_id_, 0);
      } else {
        wm_.add_to_screen(options_id_, 0);
      }
      on_menu_ = !original;
    }
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
      const std::size_t slot = i < kOptionsButtonIndex ? i : i + 1;
      buttons_[i]->set_pane(original ? panes_[i] : spread_[slot]);
      buttons_[i]->set_items(original ? items_[i] : scaled_items(i, spread_[slot]));
    }
    Image8& screen = wm_.screen();
    for (int y = 0; y < std::min(gif_->height(), screen.height()); ++y) {
      for (int x = 0; x < std::min(gif_->width(), screen.width()); ++x) {
        screen.set(x, y, gif_->at(x, y));
      }
    }
    if (!original) {
      compose_panel();
    }
  }

 private:
  // The original button of the menu's button `slot` in the remake's
  // layout (the Options button looks like the one above it).
  static std::size_t source_button(std::size_t slot) {
    return slot <= kOptionsButtonIndex ? std::min(slot, kOptionsButtonIndex - 1) : slot - 1;
  }

  // The items of button `i` in `pane`: the lines placed from the top
  // (`y` != 0) closer in proportion to the height, centred. On Android,
  // without the keyboard shortcuts' lines ("Alt-L").
  std::vector<WindowItem> scaled_items(std::size_t i, const Pane& pane) const {
    std::vector<WindowItem> items = items_[i];
#ifdef __ANDROID__
    std::erase_if(items, [](const WindowItem& item) { return item.kind == 0 && item.text.starts_with("Alt-"); });
#endif
    int top = 0;
    int bottom = 0;
    for (const WindowItem& item : items) {
      if (item.y != 0) {
        top = top == 0 ? item.y : std::min(top, item.y);
        bottom = std::max(bottom, item.y);
      }
    }
    if (top == 0) {
      return items;
    }
    // The lines' spacing in proportion, the block centred.
    const int h = panes_[i].y1 - panes_[i].y0 + 1;
    const int new_h = pane.y1 - pane.y0 + 1;
    const int spacing = ((bottom - top) * new_h + h / 2) / h;
    const int new_top = (new_h - spacing - wm_.large_font().font().height + 1) / 2;
    for (WindowItem& item : items) {
      if (item.y != 0) {
        item.y = new_top + ((item.y - top) * new_h + h / 2) / h;
      }
    }
    return items;
  }

  // Redraws the panel's rows over the buttons' span for the remake's
  // layout: each button's and gap's rows from those of an original one
  // (half from its top, half from its bottom, so that the bevels meet).
  void compose_panel() {
    Image8& screen = wm_.screen();
    const auto copy_row = [&](int dst, int src) {
      for (int x = kMenuPanel.x0; x <= kMenuPanel.x1; ++x) {
        screen.set(x, dst, gif_->at(x, src));
      }
    };
    const auto rows = [&](int dst0, int n, int src0, int src1) {
      for (int k = 0; k < n; ++k) {
        copy_row(dst0 + k, k < n / 2 ? src0 + k : src1 - (n - 1 - k));
      }
    };
    for (std::size_t slot = 0; slot < spread_.size(); ++slot) {
      const Pane& to = spread_[slot];
      const std::size_t from = source_button(slot);
      rows(to.y0, to.y1 - to.y0 + 1, panes_[from].y0, panes_[from].y1);
      if (slot + 1 < spread_.size()) {
        // The gap below, from the gap below the source (or above the last).
        const std::size_t gap = std::min(from, panes_.size() - 2);
        rows(to.y1 + 1, spread_[slot + 1].y0 - to.y1 - 1, panes_[gap].y1 + 1, panes_[gap + 1].y0 - 1);
      }
    }
  }

  WinMgr& wm_;
  std::shared_ptr<const Image8> gif_;
  std::vector<GWindow*> buttons_;
  std::vector<Pane> panes_;
  std::vector<std::vector<WindowItem>> items_;
  std::vector<Pane> spread_;
  int options_id_ = 0;
  bool on_menu_ = false;
};

}  // namespace

std::vector<Pane> spread_menu_buttons(std::span<const Pane> original, int count) {
  if (original.size() < 2 || count < 1) {
    throw std::invalid_argument("spread_menu_buttons: needs two panes and a count");
  }
  const int top = original.front().y0;
  const int span = original.back().y1 - top + 1;
  const int gap = original[1].y0 - original[0].y1 - 1;
  const int rows = span - (count - 1) * gap;
  std::vector<Pane> out;
  int y = top;
  for (int i = 0; i < count; ++i) {
    const int h = rows / count + (i < rows % count ? 1 : 0);
    out.push_back(Pane{original.front().x0, y, original.front().x1, y + h - 1});
    y += h + gap;
  }
  return out;
}

FullscreenToggled install_options(WinMgr& wm, OptionsContext context) {
  if (context.options == nullptr) {
    throw std::invalid_argument("install_options: no settings");
  }
  const auto gif =
      std::make_shared<const Image8>(decode_gif(wm.services().files.load(kOpeningGif), std::string(kOpeningGif)));
  auto layout = std::make_shared<MenuLayout>(wm, gif);
  wm.set_enter_handler(0, [layout](int /*screen*/) { layout->enter(); });

  auto options = std::make_unique<OptionsWnd>(wm, std::move(context), gif);
  OptionsWnd& options_ref = *options;
  const int id = wm.register_window(std::move(options));
  wm.add_to_screen(id, kScreenOptions);
  options_ref.add_buttons();
  wm.set_enter_handler(kScreenOptions, [&wm](int /*screen*/) {
    vfx_shape_draw(wm.screen(), GSystem::kScreenPane, wm.shapes().get(kBackground).at(0), 0, 0);
  });
  return [&options_ref](bool fullscreen) { options_ref.fullscreen_toggled(fullscreen); };
}

}  // namespace oa
