// The remake's translations of the game's texts, read at start from the
// language's folder (`lang/<language>/<file>.tsv`, `translation_folder`;
// not part of the sources): per data file of the original, named
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

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "core/language.h"

namespace oa {

class Translation {
 public:
  // No translation: every text stays as read (the original language).
  Translation() = default;
  // The tables `<file>.tsv` of `folder`, none when `folder` does not exist.
  // Throws `DataError` when a table cannot be read or is malformed.
  explicit Translation(const std::filesystem::path& folder);
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

// The folder of `language`'s tables: `<key>` (`de`) in the first of `roots`
// that has it; empty for the original language or when none has it.
std::filesystem::path translation_folder(Language language, std::span<const std::filesystem::path> roots);

// Returns UTF-8 `text` in cp437: ASCII and the German letters; throws
// `DataError` naming `context` on any other character.
std::string utf8_to_cp437(std::string_view text, std::string_view context);

}  // namespace oa
