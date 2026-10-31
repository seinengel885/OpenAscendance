// The remake's translations of the game's texts, built in from
// `src/lang/<language>/<file>.tsv`: per data file of the original, named
// by its stem in lower case (`restree.tsv` for `RESTREE.TXT`, `race00.tsv`
// for `RACE00.DIP`), one text per line,
//
//     T01<TAB>Orbital Structures<TAB>Orbitale Strukturen
//
// a number for the reviewers, the English text as the game reads it (`^`
// already a space, but kept in `windows.txt`; a line break written `\n`),
// its translation (UTF-8; besides ASCII only the German letters, which the
// fonts have in cp437, `fnt.h`). Lines starting with `#` and texts without
// a translation are left out. A text the table lacks stays English.
#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>

#include "core/language.h"

namespace oa {

class Translation {
 public:
  // Throws `DataError` when a built-in table of `language` is malformed.
  explicit Translation(Language language);
  // No translation: every text stays as read (the original language).
  static const Translation& none();

  // Returns the translation of `text` (cp437, as read from data file
  // `file`, e.g. `restree.txt`, its case and extension ignored), `text`
  // itself when there is none.
  std::string text(std::string_view file, std::string_view text) const;

  // By English text: the cp437 translation.
  using Table = std::map<std::string, std::string, std::less<>>;
  // The tables by file stem (`restree`), for the checks of the tests.
  const std::map<std::string, Table, std::less<>>& tables() const { return tables_; }

 private:
  std::map<std::string, Table, std::less<>> tables_;
};

// A built-in table: the text of `src/lang/<language>/<file>.tsv`, `file`
// the data file's stem (`restree`).
struct TranslationTable {
  std::string_view language;
  std::string_view file;
  std::string_view text;
};

// The tables generated from `src/lang` at build time (`lang_tables.cpp`).
std::span<const TranslationTable> built_in_translations();

// Returns UTF-8 `text` in cp437: ASCII and the German letters; throws
// `DataError` naming `context` on any other character.
std::string utf8_to_cp437(std::string_view text, std::string_view context);

}  // namespace oa
