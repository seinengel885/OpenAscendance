#include "screens/start_game_wnd.h"

#include <format>
#include <optional>
#include <sstream>

#include "assets/text_lines.h"
#include "core/error.h"

namespace oa {

namespace {

// The first tune of the species themes (`music.txt`).
constexpr int kFirstSpeciesTheme = 8;
constexpr int kStringSpeciesNames = 0x65;
constexpr int kRaceFlags = 32;  // gwshare raceflag.shp
constexpr int kScreenStarMap = 1;

std::string first_word(const std::string& line) {
  std::istringstream in(line);
  std::string w;
  in >> w;
  return w;
}

bool iequals_prefix(std::string_view line, std::string_view word) {
  if (line.size() < word.size()) {
    return false;
  }
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(line[i])) != word[i]) {
      return false;
    }
  }
  return true;
}

bool iequals(std::string_view a, std::string_view b) { return a.size() == b.size() && iequals_prefix(a, b); }

// Appends the lines up to `end` (compared on its length, case-insensitive),
// each with its line end, translated as one text.
void read_section(TextLineReader& r, std::string_view end, std::string& out, const Translation& translation) {
  std::string section;
  for (;;) {
    const std::optional<std::string> line = r.content_line();
    if (!line) {
      throw DataError(std::format("history.txt: no {}", end));
    }
    if (iequals_prefix(*line, end)) {
      out += translation.text("history.txt", section);
      return;
    }
    section += *line;
    section += '\n';
  }
}

void convert(std::string& s) {
  for (char& c : s) {
    if (c == '\n' || c == '_') {
      c = ' ';
    } else if (c == '@') {
      c = '\n';
    }
  }
}

}  // namespace

SpeciesHistory parse_species_history(std::span<const std::uint8_t> data, int species, int atmosphere,
                                     std::string_view name, const Translation& translation) {
  TextLineReader r(data);
  for (;;) {
    const std::optional<std::string> line = r.content_line();
    if (!line) {
      throw DataError("history.txt: unexpected end");
    }
    const std::string word = first_word(*line);
    if (iequals(word, "SPECNUM")) {
      std::istringstream in(*line);
      std::string w;
      int n = -1;
      in >> w >> n;
      if (n == species) {
        break;
      }
    } else if (iequals(word, "END")) {
      throw DataError(std::format("history.txt: no species {}", species));
    }
  }
  SpeciesHistory h;
  bool intro_seen = false;
  for (;;) {
    const std::optional<std::string> line = r.content_line();
    if (!line) {
      break;
    }
    const std::string word = first_word(*line);
    if (iequals(word, "SPECNUM") || iequals(word, "END")) {
      break;
    }
    if (iequals(word, "POWER")) {
      h.title = std::format("{}: ", name);
      read_section(r, "ENDPOWER", h.title, translation);
    } else if (iequals(word, "INTRO")) {
      read_section(r, "ENDINTRO", h.intro, translation);
      intro_seen = true;
    } else if (iequals(word, "PEACE") && intro_seen && atmosphere == 0) {
      read_section(r, "ENDPEACE", h.intro, translation);
    } else if (iequals(word, "NEUTRAL") && intro_seen && atmosphere == 1) {
      read_section(r, "ENDNEUTRAL", h.intro, translation);
    } else if (iequals(word, "HOSTILE") && intro_seen && atmosphere == 2) {
      read_section(r, "ENDHOSTILE", h.intro, translation);
    } else if (iequals(word, "TEXT")) {
      read_section(r, "ENDTEXT", h.text, translation);
    }
  }
  convert(h.intro);
  convert(h.text);
  return h;
}

bool StartGameWnd::handle_event(int type, int a, int b) {
  const GameSession& game = wm_.services().game;
  if (type == kEventActivate) {
    wm_.services().sound.play_tune(game.race_species(game.player_race()) + kFirstSpeciesTheme, false);
  } else if (type == kEventDeactivate) {
    wm_.post_message(kMessageChangeScreen, kScreenStarMap, 1);
    return true;
  }
  return GWindow::handle_event(type, a, b);
}

void StartGameWnd::draw(int /*mode*/) {
  const GameSession& game = wm_.services().game;
  const int player = game.player_race();
  const int species = game.race_species(player);
  const GameFiles& files = wm_.services().files;
  const SpeciesHistory h = parse_species_history(files.load("history.txt"), species, game.atmosphere(),
                                                 wm_.static_string(kStringSpeciesNames + species), files.translation());
  Image8& screen = wm_.screen();
  // The large portrait of the player's race slot (gwshare 7 + race, the
  // species' LGRACEnn.SHP).
  vfx_shape_draw(screen, GSystem::kScreenPane, wm_.shapes().get(std::format("DATA\\LGRACE{:02d}.SHP", species)).at(0),
                 7, 0x2e);
  const int color = 0x13 + 4 * game.race_color(player);
  GameFont& font = wm_.large_font();
  font.pane() = Pane{7, 0x105, 0x278, 0x1d1};
  font.draw_wrapped(screen, 10, 10, h.text, 0, color, 0xff, 599);
  font.pane() = Pane{0x138, 0x2e, 0x278, 0xf5};
  font.draw_wrapped(screen, 10, 10, h.intro, 0, kTextColor, 0xff, 0x126);
  const Pane title{7, 7, 0x278, 0x25};
  wm_.draw_colored(title, wm_.shared_shapes(kRaceFlags).at(static_cast<std::size_t>(species)), 3, 3,
                   game.race_color(player));
  font.pane() = title;
  font.draw_wrapped(screen, 0, 10, h.title, kFontCenterH, color, 0xff, 0x25a);
}

void add_start_game_windows(WindowFactory& factory) {
  factory.add(30, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<StartGameWnd>(wm, record, shapes);
  });
}

}  // namespace oa
