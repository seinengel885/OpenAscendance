#include "ui/flic.h"

#include <format>

#include "core/error.h"

namespace oa {

namespace {

// The preload's palette fade: 64 chunks, one fade step each.
constexpr int kPreloadSteps = 64;
// The first frame's blocking fade-in steps (BRUN), the rest following one per
// frame up to step 63.
constexpr int kBrunFadeSteps = 32;
constexpr int kLastPaletteStep = 0x40;

}  // namespace

Flic::Flic(UiServices& services, std::string_view file)
    : services_(services), script_(parse_flic_script(services.files.load(kScriptFile))) {
  font_ = std::make_unique<GameFont>(parse_font(services.files.load(script_.font_file), script_.font_file),
                                     static_cast<std::uint8_t>(script_.back_color));
  font_->pane() = {script_.title_x0, script_.title_y0, script_.title_x1, script_.title_y1};
  font_palette_ = services.files.load(script_.palette_file);
  // `font_center_offset`: centred in the title box (inclusive coordinates).
  for (const std::string& subtitle : script_.subtitles) {
    const std::string text = services.files.text(kScriptFile, subtitle);
    const int width = font_->string_width(text);
    if (width > script_.title_x1 - script_.title_x0) {
      throw DataError(std::format("{}: subtitle '{}' wider than its box", kScriptFile, text));
    }
    subtitles_.push_back({text, ((script_.title_x1 - script_.title_x0) - width) / 2,
                          ((script_.title_y1 - script_.title_y0) - font_->font().height) / 2});
  }
  flc_ = std::make_unique<FlcFile>(services.files.load(file), file);
  x_ = (kScreenWidth - flc_->header().width) / 2;
  y_ = (kScreenHeight - flc_->header().height) / 2;
}

void Flic::preload() {
  cursor_ = 0;
  fade_mode_ = 0;
  palette_step_ = 0;
  frame_ = -1;
  GSystem& gs = services_.gsystem;
  for (int i = 0; i < kPreloadSteps; ++i) {
    gs.fade_to_color_step(i);
    gs.wait_vblank();
  }
  vfx_pane_wipe(gs.screen(), GSystem::kScreenPane, 0);
}

void Flic::set_frame_rate(int hz) {
  rate_ = hz;
  timer_running_ = hz > 0;
  if (timer_running_) {
    next_tick_us_ = services_.now_us() + 1'000'000 / static_cast<std::uint64_t>(hz);
  }
}

void Flic::stop_timer() { timer_running_ = false; }

void Flic::poll_timer() {
  if (!timer_running_) {
    return;
  }
  const std::uint64_t now = services_.now_us();
  if (now < next_tick_us_) {
    return;
  }
  frame_due_ = true;
  const std::uint64_t period = 1'000'000 / static_cast<std::uint64_t>(rate_);
  next_tick_us_ += ((now - next_tick_us_) / period + 1) * period;
}

std::optional<bool> Flic::play_frame() {
  poll_timer();
  if (!frame_due_) {
    return std::nullopt;
  }
  frame_due_ = false;
  GSystem& gs = services_.gsystem;
  ++frame_;
  if (frame_ == 0) {
    vfx_pane_wipe(gs.screen(), GSystem::kScreenPane, static_cast<std::uint8_t>(script_.back_color));
  }
  if (frame_ >= flc_->header().frames) {
    return false;
  }
  const auto colors = [&](int index, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    gs.set_dac(index, r >> 2, g >> 2, b >> 2);
  };
  // Flic_decode_color256 0x2938c resets the palette fade before its entries.
  const FlcFrameChunks chunks =
      flc_->decode_frame(frame_, gs.screen(), x_, y_, colors, script_.palette_start, script_.palette_count);
  if (chunks.color256) {
    palette_step_ = 0;
  }
  if (chunks.brun) {
    // Flic_decode_brun 0x29474: the frame's palette becomes the fade target.
    gs.read_dac_into_shadow(0, 256);
    gs.set_dac_range(0, 256);
    for (palette_step_ = 0; palette_step_ < kBrunFadeSteps; ++palette_step_) {
      gs.fade_to_palette_step(palette_step_);
      gs.wait_vblank();
    }
    ++palette_step_;
  }
  if (fade_mode_ == 1) {
    gs.fade_in_range_step(script_.palette_start, script_.palette_count);
  } else if (fade_mode_ == 2) {
    gs.fade_out_range_step(script_.palette_start, script_.palette_count);
  }
  if (cursor_ < script_.events.size() && script_.events[cursor_].frame == frame_) {
    run_script_events();
  }
  if (palette_step_ != 0) {
    gs.fade_to_palette_step(palette_step_ - 1);
    if (++palette_step_ > kLastPaletteStep) {
      palette_step_ = 0;
    }
  }
  return true;
}

void Flic::run_script_events() {
  GSystem& gs = services_.gsystem;
  SoundSystem& sound = services_.sound;
  while (cursor_ < script_.events.size() && script_.events[cursor_].frame == frame_) {
    const FlicEvent& e = script_.events[cursor_];
    switch (e.type) {
      case FlicEvent::kShowSubtitle: {
        const Subtitle& s = subtitles_.at(e.a1);
        gs.set_palette_entries(script_.palette_start, script_.palette_count, font_palette_);
        gs.set_dac_range(script_.palette_start, script_.palette_count);
        font_->draw_wrapped(gs.screen(), s.dx, s.dy, s.text, kFontWipe, kDefault, kDefault, 0);
        fade_mode_ = 1;
        break;
      }
      case FlicEvent::kHideSubtitle:
        fade_mode_ = 2;
        break;
      case FlicEvent::kFrameRate:
        // AIL_set_timer_frequency, or AIL_stop_timer for 0.
        set_frame_rate(static_cast<int>(e.a1));
        break;
      case FlicEvent::kMusicVolume:
        if (sound.music_ready()) {
          sound.set_music_volume(static_cast<std::int8_t>(e.a2));
        }
        break;
      case FlicEvent::kPlayTune:
        sound.play_tune(static_cast<int>(e.a1), false);
        break;
      case FlicEvent::kStopTune:
        sound.stop_tune();
        break;
      default:
        break;
    }
    ++cursor_;
  }
}

}  // namespace oa
