// The languages of the game's texts: the game files' own (English, or
// whatever language the installed files are in), or a translation of the
// remake's (`assets/translation.h`), chosen on the options screen. Saves do
// not depend on it.
#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace oa {

enum class Language { kOriginal, kGerman };

struct LanguageInfo {
  Language language;
  // The name in the settings file and of the translation's folder
  // (`src/lang/<key>`).
  std::string_view key;
  // The name shown to the player, in that language ("Original": the files'
  // own texts; its key stays "en", as in the settings files written so far).
  std::string_view label;
};

// Every language, in the order the options screen lists them.
inline constexpr std::array<LanguageInfo, 2> kLanguages{{
    {Language::kOriginal, "en", "Original"},
    {Language::kGerman, "de", "Deutsch"},
}};

constexpr const LanguageInfo& language_info(Language language) {
  for (const LanguageInfo& info : kLanguages) {
    if (info.language == language) {
      return info;
    }
  }
  return kLanguages[0];
}

// Returns the language of settings key `key`, nothing for an unknown one.
constexpr std::optional<Language> parse_language(std::string_view key) {
  for (const LanguageInfo& info : kLanguages) {
    if (info.key == key) {
      return info.language;
    }
  }
  return std::nullopt;
}

}  // namespace oa
