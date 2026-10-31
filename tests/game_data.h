// The original game files of the tests: the install named by the OA_DATA
// environment variable; the tests of its content are skipped without it.
// OA_LANGUAGE=de shows its texts in German, for the screenshots (OA_SHOTS);
// the tests expect the original's texts.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <optional>

#include "assets/cfile.h"
#include "core/install.h"
#include "core/language.h"

namespace oa::test {

// Returns the install named by OA_DATA, or nothing when it is unset.
inline std::optional<Install> game_install() {
  const char* dir = std::getenv("OA_DATA");
  if (dir == nullptr || *dir == '\0') {
    return std::nullopt;
  }
  return Install(dir);
}

// Returns the language named by OA_LANGUAGE (`en`, `de`), the original's
// when it is unset or unknown: the texts of the tests' screens in German.
inline Language game_language() {
  const char* key = std::getenv("OA_LANGUAGE");
  return (key != nullptr ? parse_language(key) : std::nullopt).value_or(Language::kOriginal);
}

}  // namespace oa::test

// Declares `files` (an `oa::GameFiles` over the OA_DATA install), or skips
// the test when OA_DATA is unset.
#define OA_REQUIRE_GAME_FILES(files)                       \
  const auto files##_install = ::oa::test::game_install(); \
  if (!files##_install) {                                  \
    SKIP("OA_DATA is not set to an Ascendancy install");   \
  }                                                        \
  const ::oa::GameFiles files(*files##_install, ::oa::test::game_language())
