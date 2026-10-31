#include "screens/end_game_wnd.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <sstream>

#include "assets/text_lines.h"
#include "core/c_format.h"
#include "core/error.h"
#include "game/race.h"
#include "game/session.h"
#include "gfx/vfx_poly.h"

namespace oa {

namespace {

// `gwshare.txt` tables: the race slots' small portraits (0..6), large
// portraits (7..13) and ship sprites (14..20), the planets, the research
// items, the race flags, `14intel.shp` and `9endgame.shp`.
constexpr int kRacePortraits = 0;
constexpr int kLargePortraits = 7;
constexpr int kRaceShips = 14;
constexpr int kPlanetShapes = 29;
constexpr int kResearchShapes = 31;
constexpr int kRaceFlags = 32;
constexpr int kIntelShapes = 41;
constexpr int kEndGameShapes = 43;
// `14intel.shp` frames: the ring in a species' colour, the war and
// alliance marks.
constexpr int kRingFrame = 3;
constexpr int kWarFrame = 8;
constexpr int kAllianceFrame = 9;
// `static.txt`: "Your score is %d%%.\nThis gives you the title of %s".
constexpr int kStringScore = 0xf;

// The ending files by `Cosmos::ending` (1..5).
constexpr std::array<std::string_view, 5> kEndingFiles = {"end-xtnc.txt", "end-dest.txt", "end-capt.txt",
                                                          "end-unit.txt", "end-ctrl.txt"};
constexpr int kEndingExtinct = 1;

// The text boxes: the description, the score and the congratulations.
constexpr Pane kRankPane{7, 0xd7, 0x12f, 0x17d};
constexpr Pane kDescriptionPane{7, 0xd7, 0x12f, 0x107};
constexpr Pane kScorePane{7, 0x14d, 0x12f, 0x17d};
constexpr Pane kCongratulationsPane{7, 0x18d, 0x278, 0x1d1};
constexpr int kTextWidth = 0x118;
constexpr int kCongratulationsWidth = 0x25a;
// The rank picture's position in its pane.
constexpr int kRankX = 0x94;
constexpr int kRankY = 0x53;

// The species met: a row of three, then one of three (race slots 1..),
// 0x3e apart; the flag and relation mark below left of the portrait.
constexpr int kRacesX = 0x15b;
constexpr int kRacesY = 0x2a;
constexpr int kRacesSecondY = 0x85;
constexpr int kRacesStep = 0x3e;
constexpr int kRacesPerRow = 3;
constexpr int kFlagDx = -7;
constexpr int kFlagDy = 0x21;

// The scattered pictures: ships and planets in the right part (their
// boxes, in that pane), the discoveries in rows of six.
constexpr Pane kScatterPane{0x138, 7, 0x278, 0x17d};
constexpr int kShipsX0 = 0x1fe;
constexpr int kShipsY0 = 7;
constexpr int kShipsX1 = 0x278;
constexpr int kShipsY1 = 0xba;
constexpr int kPlanetsX0 = 0x138;
constexpr int kPlanetsY0 = 0xd2;
constexpr int kPlanetsX1 = 0x1ff;
constexpr int kPlanetsY1 = 0x17d;
constexpr int kResearchX = 0x213;
constexpr int kResearchY = 0xcb;
constexpr int kResearchStep = 0x11;
constexpr int kResearchPerRow = 6;
// 16.16 scales: the portraits at a half, the scattered pictures at a third.
constexpr std::int32_t kHalf = 0x8000;
constexpr std::int32_t kThird = 0x5555;

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
         });
}

// `sscanf("%s")`: the first word.
std::string first_word(const std::string& line) {
  std::istringstream in(line);
  std::string w;
  in >> w;
  return w;
}

// The draw's text conversion.
void convert(std::string& s) {
  for (char& c : s) {
    if (c == '\n' || c == '_') {
      c = ' ';
    } else if (c == '@') {
      c = '\n';
    }
  }
}

// The haze of the scattered pictures: none for few of them, darker as
// they pass `count` / (level + 1) (the level counted from 7 - (20 - n) *
// 7 / 17 for n under 20, clamped to 0..6).
int haze_level(std::int32_t n) {
  int level = 7;
  if (n < 20) {
    level -= (20 - n) * 7 / 17;
  }
  return std::clamp(level, 0, 6);
}

}  // namespace

EndingText parse_ending(std::span<const std::uint8_t> data, int rank, std::string_view name,
                        const Translation& translation) {
  TextLineReader r(data);
  EndingText t;
  const auto next = [&]() -> std::string {
    std::optional<std::string> line = r.content_line();
    if (!line) {
      throw DataError(std::format("{}: no END", name));
    }
    // `fgets` keeps the line end.
    return *line + '\n';
  };
  const auto section = [&](std::string& out) {
    std::string text;
    for (std::string line = next(); line.size() < 7 || !iequals(std::string_view(line).substr(0, 7), "ENDTEXT");
         line = next()) {
      text += line;
    }
    out += translation.text(name, text);
  };
  for (;;) {
    const std::string word = first_word(next());
    if (iequals(word, "END")) {
      break;
    }
    if (iequals(word, "ENDDESC")) {
      section(t.description);
    } else if (iequals(word, "CONGRAT")) {
      section(t.congratulations);
    } else if (iequals(word, "RANKS")) {
      for (int i = 0; i < rank; ++i) {
        next();
      }
      t.rank = translation.text(name, next());
    }
  }
  convert(t.description);
  convert(t.congratulations);
  convert(t.rank);
  return t;
}

int EndGameWnd::rank_of(int score) { return std::clamp(score / 10 - 1, 0, 9); }

bool EndGameWnd::handle_event(int type, int a, int b) {
  if (type == kEventActivate) {
    UiServices& s = wm_.services();
    // GSystem_fade_out 0x2ba18: 64 steps to black.
    for (int k = 0; k < 0x40; ++k) {
      s.gsystem.fade_to_color_step(k);
      s.gsystem.wait_vblank();
    }
    wm_.start_pending_fade();
    int tune = kVictoryTune;
    if (const game::Session* core = s.game.core(); core != nullptr && core->game->cosmos.ending == kEndingExtinct) {
      tune = kFirstSpeciesTheme + core->game->cosmos.races[core->player_race].species;
    }
    s.sound.play_tune(tune, false);
  }
  return GWindow::handle_event(type, a, b);
}

void EndGameWnd::draw(int /*mode*/) {
  game::Session* session = wm_.services().game.core();
  if (session == nullptr) {
    return;
  }
  game::Cosmos& cosmos = session->game->cosmos;
  if (cosmos.ending < 1 || cosmos.ending > kEndingFiles.size()) {
    return;
  }
  const int rank = rank_of(cosmos.score);
  const std::string_view file = kEndingFiles[cosmos.ending - 1u];
  const GameFiles& files = wm_.services().files;
  const EndingText text = parse_ending(files.load(file), rank, file, files.translation());
  Image8& screen = wm_.screen();
  const std::uint8_t player = session->player_race;
  const game::Race& me = cosmos.races[player];
  const int color = me.color * 4 + 0x13;

  if (cosmos.ending != kEndingExtinct) {
    vfx_shape_draw(screen, kRankPane, wm_.shared_shapes(kEndGameShapes).at(static_cast<std::size_t>(rank)), kRankX,
                   kRankY);
  }
  GameFont& font = wm_.large_font();
  font.pane() = kDescriptionPane;
  font.draw_wrapped(screen, 0xe, 0, c_format("%s", text.description.c_str()), kFontCenterV, color, 0xff, kTextWidth);
  font.pane() = kScorePane;
  font.draw_wrapped(screen, 0, 0, c_format(wm_.static_string(kStringScore), cosmos.score, text.rank.c_str()),
                    kFontCenterV | kFontCenterH, color, 0xff, kTextWidth);
  font.pane() = kCongratulationsPane;
  font.draw_wrapped(screen, 7, 0, text.congratulations, kFontCenterV, color, 0xff, kCongratulationsWidth);

  // The player's portrait and flag.
  const Pane& full = GSystem::kScreenPane;
  vfx_shape_draw(screen, full, wm_.shared_shapes(kLargePortraits + player).at(0), 7, 7);
  const auto draw_flag = [&](int x, int y, int race) {
    const game::Race& r = cosmos.races[static_cast<std::size_t>(race)];
    wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(r.species), x, y, r.color);
  };
  draw_flag(10, 10, player);

  // The other race slots: portrait, ring, flag and the relation mark.
  const ShapeTable& intel = wm_.shared_shapes(kIntelShapes);
  int x = kRacesX;
  int y = kRacesY;
  for (int i = 1; i < cosmos.race_count; ++i) {
    if (i == kRacesPerRow + 1) {
      x = kRacesX;
      y = kRacesSecondY;
    }
    vfx_shape_transform_draw(screen, full, wm_.shared_shapes(kRacePortraits + i).at(0), x, y, kHalf, kHalf);
    wm_.draw_colored(full, intel.at(kRingFrame), x, y, cosmos.races[static_cast<std::size_t>(i)].color);
    draw_flag(x + kFlagDx, y + kFlagDy, i);
    const std::uint8_t relation = me.relation[static_cast<std::size_t>(i)];
    if (relation == game::kAlliance || relation == game::kWar) {
      vfx_shape_draw(screen, full, intel.at(relation == game::kAlliance ? kAllianceFrame : kWarFrame), x + kFlagDx,
                     y + kFlagDy);
    }
    x += kRacesStep;
  }

  // The ships, then the planets, scattered at random (x drawn first), the
  // later ones hazed darker past each share of the ships.
  std::vector<HazeTable>& haze = wm_.services().gsystem.haze_tables();
  const std::int32_t ships = static_cast<std::int32_t>(game::race_list_ships(cosmos, me).size());
  const auto scatter = [&](int level, std::int32_t share, int count, auto owned, auto picture, int x0, int y0, int x1,
                           int y1) {
    const int range_x = std::max(x1 - x0 - 0x14, 2);
    const int range_y = std::max(y1 - y0 - 0x14, 2);
    for (int k = 0; k < count; ++k) {
      if (!owned(k)) {
        continue;
      }
      // The counter is every record's, not reset.
      if (k > share && level > 0) {
        --level;
      }
      const HazeTable* table = level > 0 ? &haze.at(static_cast<std::size_t>(7 - level)) : nullptr;
      const int px = x0 + 0xa - kScatterPane.x0 + session->rng.rand() % range_x;
      const int py = y0 + 0xa - kScatterPane.y0 + session->rng.rand() % range_y;
      const auto [shape, frame] = picture(k);
      vfx_shape_transform_draw(screen, kScatterPane, wm_.shared_shapes(shape).at(static_cast<std::size_t>(frame)), px,
                               py, kThird, kThird, table);
    }
  };
  const int ship_level = haze_level(ships);
  scatter(
      ship_level, ships / (ship_level + 1), cosmos.ship_count,
      [&](int k) { return cosmos.ships[static_cast<std::size_t>(k)].owner == player; },
      [&](int k) {
        return std::pair{
            kRaceShips + player,
            static_cast<int>(static_cast<std::int8_t>(cosmos.ships[static_cast<std::size_t>(k)].hull_size))};
      },
      kShipsX0, kShipsY0, kShipsX1, kShipsY1);
  const int planet_level = haze_level(game::race_count_planets(cosmos, me));
  // The share counts the ships (the original's).
  scatter(
      planet_level, ships / (planet_level + 1), cosmos.planet_count,
      [&](int k) { return cosmos.planets[static_cast<std::size_t>(k)].owner == player; },
      [&](int k) {
        const game::Planet& p = cosmos.planets[static_cast<std::size_t>(k)];
        return std::pair{kPlanetShapes, p.type * 5 + p.size};
      },
      kPlanetsX0, kPlanetsY0, kPlanetsX1, kPlanetsY1);

  // The discoveries.
  const game::Research& research = session->game->research;
  x = kResearchX;
  y = kResearchY;
  for (int i = 0; i < research.count; ++i) {
    if (i != 0 && i % kResearchPerRow == 0) {
      x = kResearchX;
      y += kResearchStep;
    }
    if ((research.items[static_cast<std::size_t>(i)].known_mask & (1u << player)) != 0) {
      vfx_shape_transform_draw(screen, pane_, wm_.shared_shapes(kResearchShapes).at(static_cast<std::size_t>(i)), x, y,
                               kThird, kThird);
    }
    x += kResearchStep;
  }
}

void add_end_game_windows(WindowFactory& factory) {
  factory.add(24, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<EndGameWnd>(wm, record, shapes);
  });
}

}  // namespace oa
