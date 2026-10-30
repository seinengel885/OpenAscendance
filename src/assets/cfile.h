// The original's file layer (`cfile.cpp`): the `.COB` archives listed by
// `COB.CFG` and the loose files of the install. Every data file of the game
// goes through it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "assets/translation.h"
#include "core/install.h"
#include "core/language.h"

namespace oa {

// One archived file.
struct CobMember {
  std::string name;          // DOS path inside the archive (`data\1starmap.shp`)
  std::size_t archive = 0;   // index in `CobDirectory::archives()`
  std::uint32_t offset = 0;  // absolute offset in the archive file
  std::uint32_t size = 0;    // up to the next member (or the archive end)
};

// Returns the members of the `.COB` archive `data` (its first bytes, the
// whole directory at least) for an archive file of `file_size` bytes, the
// archive having index `archive`. Layout (little-endian): `u32 count`,
// `count` 50-byte NUL-padded names, `count` `u32` absolute offsets, then the
// data. Throws `DataError` naming `context` when the directory is
// inconsistent with the file size.
std::vector<CobMember> parse_cob_directory(std::span<const std::uint8_t> data, std::uint64_t file_size,
                                           std::size_t archive, std::string_view context);

// The COB directory (global 0x8a0e0, `CobDirectory_load_cfg` 0x1c3c4 and
// `CobDirectory_add_archive`): the archives named by the lines of `COB.CFG`
// (at most 5) and their members (at most 500 in all).
class CobDirectory {
 public:
  static constexpr std::size_t kMaxArchives = 5;
  static constexpr std::size_t kMaxMembers = 500;

  // Reads `COB.CFG` and the archives' directories from `install`. Throws
  // `DataError` when one is missing or malformed, or the limits are passed.
  explicit CobDirectory(const Install& install);

  const std::vector<std::filesystem::path>& archives() const { return archives_; }
  const std::vector<CobMember>& members() const { return members_; }

  // Returns the member named `dos_path` (case-insensitive, '/' and '\\'
  // equivalent, as the original's `stricmp` of the full path), if any.
  const CobMember* find(std::string_view dos_path) const;

  // Returns the bytes of `member`; throws `DataError` if they cannot be read.
  std::vector<std::uint8_t> read(const CobMember& member) const;

 private:
  std::vector<std::filesystem::path> archives_;
  std::vector<CobMember> members_;
};

// Opens game files as `CFile_open` does: the archive member of that name
// first, else the loose file of the install (archive members win).
class GameFiles {
 public:
  // Throws `DataError` when the COB directory cannot be read. The texts are
  // in `language` (`text`).
  explicit GameFiles(Install install, Language language = Language::kEnglish);

  const Install& install() const { return install_; }
  const CobDirectory& directory() const { return directory_; }

  // Returns the whole file `dos_path`, nothing if it is neither a member nor
  // a loose file; throws `DataError` if it cannot be read.
  std::optional<std::vector<std::uint8_t>> try_load(std::string_view dos_path) const;
  // Returns the whole file `dos_path`; throws `DataError` if it is missing.
  std::vector<std::uint8_t> load(std::string_view dos_path) const;

  // Returns `text` of data file `file` (`restree.txt`) in the language of the
  // files (`Translation::text`).
  std::string text(std::string_view file, std::string_view text) const { return translation_.text(file, text); }

 private:
  Install install_;
  CobDirectory directory_;
  Translation translation_;
};

}  // namespace oa
