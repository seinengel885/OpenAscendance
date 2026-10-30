// The remake's own settings, set on its options screen
// (`screens/options_wnd.h`) and kept in `options.cfg` in the user folder,
// apart from the original's `ascend.cfg` (the mute flags and the rotation
// speed, kept byte-compatible by `WinMgr`: a muted sound keeps its volume
// here).
//
// `options.cfg` is text, one `key=value` per line (`#` starts a comment):
//
//     variant=standard        # or antagonizer, pecustom: the variant of new games
//     fullscreen=1            # or 0: a window
//     scaling=smooth          # or sharp
//     music_volume=100        # 0..100, percent of the original's volume
//     sfx_volume=60           # 0..100, likewise for the sound effects
//     language=en             # or de: the texts' language, from the next start
//
// Unknown keys and bad values are ignored with a warning, so that a file of
// a later version stays readable.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/language.h"
#include "game/variant.h"
#include "platform/display.h"

namespace oa {

inline constexpr std::string_view kOptionsFile = "options.cfg";

// The volumes' defaults, in percent: the music at the original's level, the
// sound effects under it.
inline constexpr int kDefaultMusicVolume = 100;
inline constexpr int kDefaultSfxVolume = 60;

struct RemakeOptions {
  game::Variant variant = game::Variant::kStandard;
  bool fullscreen = true;
  Scaling scaling = Scaling::kSmooth;
  // The levels of `SoundSystem::set_music_level` / `set_sfx_level`.
  int music_volume = kDefaultMusicVolume;
  int sfx_volume = kDefaultSfxVolume;
  // The texts' language (`GameFiles`), read at the program's start.
  Language language = Language::kOriginal;

  bool operator==(const RemakeOptions&) const = default;
};

// Returns the options of the `options.cfg` text `text`, the defaults for
// the keys it lacks.
RemakeOptions parse_options(std::string_view text);
// Returns the `options.cfg` text of `options` (every key).
std::string format_options(const RemakeOptions& options);
// Returns the options of the file `path`, the defaults when it does not
// exist or cannot be read.
RemakeOptions load_options(const std::filesystem::path& path);
// Writes `options` to `path`; returns false (with a warning) when it cannot
// be written.
bool save_options(const std::filesystem::path& path, const RemakeOptions& options);

// The window settings the options screen changes (the SDL display of the
// program; the tests' fake).
class DisplayControl {
 public:
  virtual ~DisplayControl() = default;
  virtual bool fullscreen() const = 0;
  virtual void set_fullscreen(bool fullscreen) = 0;
  virtual Scaling scaling() const = 0;
  virtual void set_scaling(Scaling scaling) = 0;
};

}  // namespace oa
