#include "ui/options.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

#include "core/log.h"

namespace oa {

namespace {

// Returns `s` without its leading and trailing blanks.
std::string_view trim(std::string_view s) {
  const std::size_t begin = s.find_first_not_of(" \t\r");
  if (begin == std::string_view::npos) {
    return {};
  }
  return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}

std::optional<bool> parse_flag(std::string_view value) {
  if (value == "1") {
    return true;
  }
  if (value == "0") {
    return false;
  }
  return std::nullopt;
}

// Returns the volume `value`, a whole percent within 0..100.
std::optional<int> parse_volume(std::string_view value) {
  int v = 0;
  const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
  if (ec != std::errc{} || end != value.data() + value.size() || v < 0 || v > 100) {
    return std::nullopt;
  }
  return v;
}

std::optional<Scaling> parse_scaling(std::string_view value) {
  if (value == "smooth") {
    return Scaling::kSmooth;
  }
  if (value == "sharp") {
    return Scaling::kSharp;
  }
  return std::nullopt;
}

}  // namespace

RemakeOptions parse_options(std::string_view text) {
  RemakeOptions options;
  int line_number = 0;
  while (!text.empty()) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    std::string_view line = text.substr(0, end);
    text.remove_prefix(std::min(end + 1, text.size()));
    ++line_number;
    line = trim(line.substr(0, line.find('#')));
    if (line.empty()) {
      continue;
    }
    const std::size_t equal = line.find('=');
    const std::string_view key = trim(line.substr(0, equal));
    const std::string_view value = equal == std::string_view::npos ? std::string_view{} : trim(line.substr(equal + 1));
    bool known = true;
    if (key == "variant") {
      if (const std::optional<game::Variant> v = game::parse_variant(value)) {
        options.variant = *v;
      } else {
        known = false;
      }
    } else if (key == "fullscreen") {
      if (const std::optional<bool> v = parse_flag(value)) {
        options.fullscreen = *v;
      } else {
        known = false;
      }
    } else if (key == "scaling") {
      if (const std::optional<Scaling> v = parse_scaling(value)) {
        options.scaling = *v;
      } else {
        known = false;
      }
    } else if (key == "music_volume" || key == "sfx_volume") {
      if (const std::optional<int> v = parse_volume(value)) {
        (key == "music_volume" ? options.music_volume : options.sfx_volume) = *v;
      } else {
        known = false;
      }
    } else if (key == "language") {
      if (const std::optional<Language> v = parse_language(value)) {
        options.language = *v;
      } else {
        known = false;
      }
    } else {
      known = false;
    }
    if (!known) {
      log_warning("{} line {}: ignored '{}'", kOptionsFile, line_number, line);
    }
  }
  return options;
}

std::string format_options(const RemakeOptions& options) {
  return std::format("variant={}\nfullscreen={}\nscaling={}\nmusic_volume={}\nsfx_volume={}\nlanguage={}\n",
                     game::variant_info(options.variant).key, options.fullscreen ? 1 : 0,
                     options.scaling == Scaling::kSharp ? "sharp" : "smooth", options.music_volume, options.sfx_volume,
                     language_info(options.language).key);
}

RemakeOptions load_options(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    log_warning("cannot read {}", path.string());
    return {};
  }
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  return parse_options(text);
}

bool save_options(const std::filesystem::path& path, const RemakeOptions& options) {
  const std::string text = format_options(options);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    log_warning("cannot write {}", path.string());
    return false;
  }
  return true;
}

}  // namespace oa
