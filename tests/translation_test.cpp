// Tests: the translations' tables against the original's files. Every English
// text of a table must be a text the game looks up, as its reading code
// builds it (a mistyped one is never translated).

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <format>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "assets/flic_script.h"
#include "assets/help_txt.h"
#include "assets/static_strings.h"
#include "assets/text_lines.h"
#include "assets/translation.h"
#include "assets/windows_txt.h"
#include "game/negotiation.h"
#include "game/state.h"
#include "game/tables.h"
#include "game_data.h"

namespace oa {
namespace {

using Texts = std::set<std::string, std::less<>>;

bool istarts_with(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(s[i])) != std::toupper(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

// The whitespace-separated words of `line`.
std::vector<std::string> words(const std::string& line) {
  std::istringstream in(line);
  std::vector<std::string> out;
  for (std::string w; in >> w;) {
    out.push_back(w);
  }
  return out;
}

std::string first_word(const std::string& line) {
  const std::vector<std::string> w = words(line);
  return w.empty() ? std::string() : w[0];
}

// Every line of `data` (`newgame.txt`, `tutindex.txt`: their lines are
// looked up as read).
Texts lines(std::span<const std::uint8_t> data) {
  Texts out;
  TextLineReader r(data);
  while (std::optional<std::string> l = r.line()) {
    out.insert(*l);
  }
  return out;
}

// The texts and button titles of every topic of a help file (`help_wnd.cpp`).
Texts help_texts(std::span<const std::uint8_t> data, std::string_view file) {
  Texts out;
  TextLineReader r(data);
  while (std::optional<std::string> l = r.content_line()) {
    const std::vector<std::string> w = words(*l);
    if (w.size() < 2 || w[0].size() != 4 || !istarts_with(w[0], "HELP") ||
        (w[1].size() == 3 && istarts_with(w[1], "END"))) {
      continue;
    }
    const HelpTopic t = parse_help_topic(data, w[1], file, 0, 0);
    for (const HelpElement& e : t.elements) {
      if (e.kind == HelpElement::Kind::kText) {
        out.insert(e.text);
      }
    }
    for (const HelpButton& b : t.buttons) {
      if (b.title) {
        out.insert(*b.title);
      }
    }
  }
  return out;
}

// The sections of `history.txt` as `parse_species_history` reads them: the
// lines up to END<keyword>, each with its line end.
Texts history_sections(std::span<const std::uint8_t> data) {
  Texts out;
  TextLineReader r(data);
  while (std::optional<std::string> l = r.content_line()) {
    const std::string word = first_word(*l);
    for (const std::string_view keyword : {"POWER", "INTRO", "PEACE", "NEUTRAL", "HOSTILE", "TEXT"}) {
      if (word.size() != keyword.size() || !istarts_with(word, keyword)) {
        continue;
      }
      const std::string end = std::format("END{}", keyword);
      std::string section;
      for (std::optional<std::string> s = r.content_line(); s && !istarts_with(*s, end); s = r.content_line()) {
        section += *s + '\n';
      }
      out.insert(section);
    }
  }
  return out;
}

// The sections and ranks of an ending file as `parse_ending` reads them.
Texts ending_texts(std::span<const std::uint8_t> data) {
  Texts out;
  TextLineReader r(data);
  while (std::optional<std::string> l = r.content_line()) {
    const std::string word = first_word(*l);
    if (istarts_with(word, "ENDDESC") || istarts_with(word, "CONGRAT")) {
      std::string section;
      for (std::optional<std::string> s = r.content_line(); s && !istarts_with(*s, "ENDTEXT"); s = r.content_line()) {
        section += *s + '\n';
      }
      out.insert(section);
    } else if (istarts_with(word, "RANKS")) {
      for (int i = 0; i < 10; ++i) {
        if (std::optional<std::string> s = r.content_line()) {
          out.insert(*s + '\n');
        }
      }
    }
  }
  return out;
}

Texts dip_sentences(std::span<const std::uint8_t> data) {
  const game::DipSentences d = game::parse_dip_sentences(data);
  Texts out(d.actions.begin(), d.actions.end());
  out.insert(d.responses.begin(), d.responses.end());
  return out;
}

// The texts the game looks up in the data file of table `stem`.
Texts looked_up(const GameFiles& files, const std::string& stem) {
  const std::string file = stem + ".txt";
  if (stem == "static") {
    const std::vector<std::string> s = parse_static_strings(files.load(file));
    return Texts(s.begin(), s.end());
  }
  if (stem == "windows") {
    Texts out;
    for (const WindowRecord& w : parse_windows_txt(files.load(file)).windows) {
      out.insert(w.name);
      for (const WindowItem& item : w.items) {
        if (item.kind == 0) {
          out.insert(item.text);
        }
      }
    }
    return out;
  }
  if (stem == "restree") {
    Texts out;
    for (const game::ResearchItem& item : game::parse_restree(files.load(file)).items) {
      out.insert(std::string(game::name_view(item.name)));
    }
    return out;
  }
  if (stem == "gizmos") {
    Texts out;
    for (const game::Gizmo& g : game::parse_gizmos(files.load(file))) {
      out.insert(g.name);
    }
    return out;
  }
  if (stem == "planitem") {
    Texts out;
    for (const game::PlanItem& item : game::parse_planitems(files.load(file)).items) {
      out.insert(item.name);
    }
    return out;
  }
  if (stem == "newgame" || stem == "tutindex") {
    return lines(files.load(file));
  }
  if (stem == "subtitle") {
    const FlicScript s = parse_flic_script(files.load(file));
    return Texts(s.subtitles.begin(), s.subtitles.end());
  }
  if (stem == "history") {
    return history_sections(files.load(file));
  }
  if (stem.starts_with("end-")) {
    return ending_texts(files.load(file));
  }
  if (stem == "player" || stem.starts_with("race")) {
    return dip_sentences(files.load(stem + ".dip"));
  }
  // The help files: help, gizhelp, planhelp and the tutorials'.
  return help_texts(files.load(file), file);
}

TEST_CASE("translations: every English text is one the game looks up", "[translation][content]") {
  OA_REQUIRE_GAME_FILES(files);
  const Translation german(Language::kGerman);
  REQUIRE_FALSE(german.tables().empty());
  for (const auto& [stem, table] : german.tables()) {
    // The remake's own texts: looked up by the English of its code.
    if (stem == "openascendance") {
      continue;
    }
    const Texts texts = looked_up(files, stem);
    std::vector<std::string> unknown;
    for (const auto& [english, translation] : table) {
      if (!texts.contains(english)) {
        unknown.push_back(english);
      }
    }
    INFO(stem << ": " << unknown.size() << " unknown, the first: " << (unknown.empty() ? "" : unknown.front()));
    CHECK(unknown.empty());
  }
}

}  // namespace
}  // namespace oa
