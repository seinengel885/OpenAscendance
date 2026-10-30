#include "assets/translation.h"

#include <format>

#include "core/error.h"

namespace oa {

namespace {

// The cp437 codes of the German letters (the fonts' added glyphs, `fnt.h`).
struct Letter {
  char32_t code;
  char cp437;
};
constexpr Letter kLetters[] = {
    {U'ä', '\x84'}, {U'ö', '\x94'}, {U'ü', '\x81'}, {U'Ä', '\x8e'}, {U'Ö', '\x99'}, {U'Ü', '\x9a'}, {U'ß', '\xe1'},
};

// Returns a table column with each `\n` made a line break (some strings of
// `static.txt` hold line breaks).
std::string line_breaks(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
      out += '\n';
      ++i;
    } else {
      out += text[i];
    }
  }
  return out;
}

}  // namespace

std::string utf8_to_cp437(std::string_view text, std::string_view context) {
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    // The German letters are all two-byte sequences.
    char32_t code = 0;
    if ((c & 0xe0) == 0xc0 && i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xc0) == 0x80) {
      code = static_cast<char32_t>(((c & 0x1f) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3f));
    }
    const Letter* letter = nullptr;
    for (const Letter& l : kLetters) {
      if (l.code == code) {
        letter = &l;
      }
    }
    if (letter == nullptr) {
      throw DataError(std::format("{}: character not in the fonts in '{}'", context, text));
    }
    out += letter->cp437;
    i += 2;
  }
  return out;
}

Translation::Translation(Language language) {
  const std::string_view key = language_info(language).key;
  for (const TranslationTable& table : built_in_translations()) {
    if (table.language != key) {
      continue;
    }
    auto& texts = tables_[std::string(table.file)];
    std::string_view rest = table.text;
    int line_number = 0;
    while (!rest.empty()) {
      const std::size_t end = std::min(rest.find('\n'), rest.size());
      std::string_view line = rest.substr(0, end);
      rest.remove_prefix(std::min(end + 1, rest.size()));
      ++line_number;
      if (line.ends_with('\r')) {
        line.remove_suffix(1);
      }
      if (line.empty() || line.starts_with('#')) {
        continue;
      }
      const std::string context = std::format("lang/{}/{} line {}", key, table.file, line_number);
      const std::size_t tab1 = line.find('\t');
      const std::size_t tab2 = tab1 == std::string_view::npos ? tab1 : line.find('\t', tab1 + 1);
      if (tab2 == std::string_view::npos || line.find('\t', tab2 + 1) != std::string_view::npos) {
        throw DataError(context + ": not number, English and translation");
      }
      const std::string_view translated = line.substr(tab2 + 1);
      if (translated.empty()) {
        continue;
      }
      const std::string english = utf8_to_cp437(line_breaks(line.substr(tab1 + 1, tab2 - tab1 - 1)), context);
      if (!texts.emplace(english, utf8_to_cp437(line_breaks(translated), context)).second) {
        throw DataError(std::format("{}: '{}' translated twice", context, english));
      }
    }
  }
}

std::string Translation::text(std::string_view file, std::string_view text) const {
  if (const auto table = tables_.find(file); table != tables_.end()) {
    if (const auto it = table->second.find(text); it != table->second.end()) {
      return it->second;
    }
  }
  return std::string(text);
}

}  // namespace oa
