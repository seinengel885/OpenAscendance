#include "screens/status_wnd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

#include "core/c_format.h"
#include "core/error.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/session.h"
#include "game/tables.h"
#include "gfx/vfx_poly.h"
#include "screens/management_menu.h"

namespace oa {

namespace {

constexpr int kScreenStarSystem = 16;
constexpr int kScreenPlanet = 17;

// The static strings of the screen.
constexpr int kStringNoEnding = 0x1c;     // ""
constexpr int kStringPlural = 0x1d;       // "s"
constexpr int kStringSizes = 0x29;        // "Tiny".."Enormous"
constexpr int kStringTypes = 0x2e;        // "Husk".."Cornucopia"
constexpr int kStringPlanetKind = 0x88;   // "(%s %s Planet)"
constexpr int kStringSelfManaged = 0x89;  // "Self-Managed"
constexpr int kStringNoProgress = 0x8a;   // " - No Progress"
constexpr int kStringEta = 0x8b;          // " - %d day%s until completion"
constexpr int kStringBuilding = 0x8c;     // "Building %s%s"
constexpr int kStringNoProject = 0x8d;    // "No Project"
constexpr int kStringNoFreePop = 0x8e;    // "   (No Free Population)"
constexpr int kStringTitle = 0x97;        // "%s  (%d Planet%s Occupied, %d System%s Controlled)"
// The species names (static strings 101..121).
constexpr int kStringSpecies = 101;

// `gwshare.txt` tables.
constexpr int kPlanItemShapes = 30;  // planitem.shp
constexpr int kRaceFlags = 32;       // raceflag.shp, frame = species
constexpr int kResearchShapes = 38;  // planres.shp
constexpr int kIndustryShapes = 39;  // planind.shp
constexpr int kProsperityShapes = 40;  // planpro.shp
constexpr int kPlanetScreenShapes = 44;  // 17np.shp
// 17np.shp frames of the small population pips: working, idle, room.
constexpr int kWorkerPip = 6;
constexpr int kIdlePip = 7;
constexpr int kRoomPip = 8;

// The orbital items shown as small icons (0x869f8): Shipyard, Orbital Docks.
constexpr std::array<std::uint8_t, 2> kOrbitalIcons{22, 24};

constexpr std::int32_t kQuarterScale = 0x4000;
constexpr std::int32_t kHalfScale = 0x8000;

int race_text_color(int color) { return color * 4 + 0x13; }

// The outputs' picture frame: `chop(value ** 0.75)` as a `u16`, at most 20.
std::size_t output_frame(std::uint16_t value) {
  const auto f = static_cast<std::uint16_t>(game::chop(std::pow(static_cast<double>(value), 0.75)));
  return std::min<std::size_t>(f, 20);
}

}  // namespace

int StatusWnd::controlled_systems(const game::Cosmos& cosmos, std::uint8_t player) {
  const auto bit = static_cast<std::uint8_t>(1u << player);
  const game::StarIndex home = cosmos.races.at(player).home;
  int count = 0;
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    if (cosmos.stars.at(static_cast<std::size_t>(s)).owner_mask == bit && s != home) {
      ++count;
    }
  }
  if (home != game::kNone && (cosmos.stars.at(static_cast<std::size_t>(home)).owner_mask & bit) != 0) {
    ++count;
  }
  return count;
}

void StatusWnd::fill() {
  list_->clear();
  game::Session* session = wm_.services().game.core();
  if (session == nullptr) {
    return;
  }
  const game::Cosmos& cosmos = session->game->cosmos;
  for (std::int16_t p = 0; p < cosmos.planet_count; ++p) {
    const game::Planet& planet = cosmos.planets.at(static_cast<std::size_t>(p));
    if (planet.owner == session->player_race) {
      const int row = list_->add_item(std::string(game::name_view(planet.name)), kRowHeight);
      list_->set_item_data(row, p);
    }
  }
  // `ListItem_compare_value` (0x10a14) on the rows' data, the acquired day.
  list_->sort([&cosmos](const ListBox::Item& a, const ListBox::Item& b) {
    const std::int32_t da = cosmos.planets.at(static_cast<std::size_t>(a.data)).acquired_day;
    const std::int32_t db = cosmos.planets.at(static_cast<std::size_t>(b.data)).acquired_day;
    return da > db ? 1 : da == db ? 0 : -1;
  });
}

bool StatusWnd::handle_event(int type, int a, int b) {
  switch (type) {
    case kEventActivate:
      if (list_ == nullptr) {
        list_ = dynamic_cast<ListBox*>(wm_.find_window("STATPLANLST"));
        if (list_ == nullptr) {
          throw DataError("windows.txt: no STATPLANLST list box");
        }
        list_->set_line_height(kLineHeight);
        list_->set_draw_item([this](const Pane& row, const ListBox::Item& item, bool hilite) {
          draw_row(row, static_cast<game::PlanetIndex>(item.data), hilite);
        });
        list_->set_colors(0x31, 0, 0);
        list_->set_redraw_hover(true);
      }
      fill();
      list_->scroll_to(top_);
      return GWindow::handle_event(type, a, b);
    case kEventDeactivate:
      top_ = list_ != nullptr ? list_->top() : 0;
      return GWindow::handle_event(type, a, b);
    case kEventListPick: {
      game::Session* session = wm_.services().game.core();
      if (session != nullptr && list_ != nullptr && a >= 0 && a < list_->count()) {
        game::Cosmos& cosmos = session->game->cosmos;
        const game::PlanetIndex planet = row_planet(a);
        if (const auto area = management_areas_.find(planet);
            area != management_areas_.end() && area->second.contains(wm_.input().x, wm_.input().y)) {
          run_management_menu(wm_, *session, cosmos.planets.at(static_cast<std::size_t>(planet)));
          list_->draw(kDrawNormal);
          return false;
        }
        if (wm_.input().x < kStarClickLimit) {
          cosmos.selected_star = cosmos.planets.at(static_cast<std::size_t>(planet)).star;
          wm_.post_message(kMessageChangeScreen, kScreenStarSystem, 1);
        } else {
          cosmos.selected_planet = planet;
          wm_.post_message(kMessageChangeScreen, kScreenPlanet, 1);
        }
        top_ = list_->top();
      }
      return false;
    }
    default:
      return GWindow::handle_event(type, a, b);
  }
}

void StatusWnd::draw(int /*mode*/) {
  if (!active_) {
    return;
  }
  const game::Session* session = wm_.services().game.core();
  if (session != nullptr && list_ != nullptr) {
    const game::Cosmos& cosmos = session->game->cosmos;
    const std::uint8_t player = session->player_race;
    const game::Race& race = cosmos.races.at(player);
    const int systems = controlled_systems(cosmos, player);
    const int planets = list_->count();
    const std::string title = c_format(
        wm_.static_string(kStringTitle), wm_.static_string(kStringSpecies + race.species).c_str(), planets,
        wm_.static_string(planets == 1 ? kStringNoEnding : kStringPlural).c_str(), systems,
        wm_.static_string(systems == 1 ? kStringNoEnding : kStringPlural).c_str());
    GameFont& font = wm_.large_font();
    font.pane() = pane_;
    font.draw_wrapped(wm_.screen(), 0x19, 9, title, kFontCenterH, race_text_color(race.color), 0xff, 0);
    wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(race.species), 3, 3, race.color);
  }
  draw_children();
}

void StatusWnd::draw_row(const Pane& row, game::PlanetIndex index, bool hilite) {
  const game::Session* session = wm_.services().game.core();
  if (session == nullptr) {
    return;
  }
  const game::Cosmos& cosmos = session->game->cosmos;
  const game::Planet& planet = cosmos.planets.at(static_cast<std::size_t>(index));
  const game::Star& star = cosmos.stars.at(static_cast<std::size_t>(planet.star));
  const std::uint8_t player = session->player_race;
  const game::Race& race = cosmos.races.at(player);
  Image8& screen = wm_.screen();

  vfx_shape_draw(screen, row, wm_.shapes().get("DATA\\suns.shp").at(static_cast<std::size_t>(star.type)), 0x4b, 0x4b);
  vfx_shape_draw(screen, row, wm_.shapes().get("DATA\\planets.shp").at(planet.type * 5u + planet.size), 200, 0x4b);

  // The hovered row's star or planet name, by the pointer's side.
  int star_color = race_text_color(race.color);
  int planet_color = star_color;
  if (hilite) {
    (wm_.input().x < kStarHiliteLimit ? star_color : planet_color) = kTextColor;
  }
  const auto bit = static_cast<std::uint8_t>(1u << player);
  if (star.owner_mask == bit || (planet.star == race.home && (star.owner_mask & bit) != 0)) {
    wm_.draw_colored(row, wm_.shared_shapes(kRaceFlags).at(race.species), 4, 10, race.color);
  }
  GameFont& small = wm_.small_font();
  GameFont& large = wm_.large_font();
  small.pane() = row;
  large.pane() = row;
  small.draw_wrapped(screen, 0x78, 0x12, game::name_view(star.name), 0, star_color, 0xff, 0);
  const std::string_view name = game::name_view(planet.name);
  large.draw_wrapped(screen, 0x113, 0xf, name, 0, planet_color, 0xff, 0);
  const std::string kind =
      c_format(wm_.static_string(kStringPlanetKind), wm_.static_string(kStringSizes + planet.size).c_str(),
                    wm_.static_string(kStringTypes + planet.type).c_str());
  small.draw_wrapped(screen, large.string_width(name) + 0x127, 0x10, kind, 0, planet_color, 0xff, 0);
  int y = 0x1e;
  if (management_shown(*session, planet)) {
    // peCustom: the management, always shown; a click opens its menu.
    const std::string label = management_label(wm_, *session, planet);
    const bool hot =
        hilite && management_areas_.contains(index) && management_areas_[index].contains(wm_.input().x, wm_.input().y);
    small.draw_wrapped(screen, 0x113, 0x1e, label, 0, hot ? kTextColor : planet_color, 0xff, 0);
    management_areas_[index] = Pane{row.x0 + 0x113, row.y0 + 0x1e, row.x0 + 0x113 + small.string_width(label),
                                    row.y0 + 0x1e + small.font().height};
    y = 0x27;
  } else if (planet.self_managed == -1) {
    small.draw_wrapped(screen, 0x113, 0x1e, wm_.static_string(kStringSelfManaged), 0, planet_color, 0xff, 0);
    y = 0x27;
  }
  std::string project;
  int project_color = planet_color;
  if (planet.project_item == game::kNoItem) {
    project = wm_.static_string(kStringNoProject);
    if (planet.population == planet.used_population) {
      project += wm_.static_string(kStringNoFreePop);
    }
    project_color = kTextColor;
  } else {
    const game::PlanItem& item = session->tables->planitems.items.at(planet.project_item);
    std::string eta;
    if ((item.flags & game::kItemWholePlanet) == 0) {
      const auto days = static_cast<std::uint16_t>(game::planet_project_eta(*session, planet));
      if (days == 0xffff) {
        eta = wm_.static_string(kStringNoProgress);
      } else {
        eta = c_format(wm_.static_string(kStringEta), static_cast<int>(days),
                            wm_.static_string(days == 1 ? kStringNoEnding : kStringPlural).c_str());
      }
    }
    project = c_format(wm_.static_string(kStringBuilding), item.name.c_str(), eta.c_str());
  }
  small.draw_wrapped(screen, 0x113, y, project, 0, project_color, 0xff, 0);

  // The built Shipyard and Orbital Docks, at quarter size.
  const ShapeTable& items = wm_.shared_shapes(kPlanItemShapes);
  int icon_y = 0x19;
  const std::span<const game::Square> squares = game::planet_squares(cosmos, planet);
  for (const std::uint8_t it : kOrbitalIcons) {
    const std::uint16_t sq = game::planet_find_item(cosmos, planet, it);
    if (sq != game::kNoSquare && (squares[sq].flags & game::kSquareBuilt) != 0) {
      vfx_shape_transform_draw(screen, row, items.at(it), 0xf5, icon_y, kQuarterScale, kQuarterScale);
      icon_y += 0x19;
    }
  }
  // The outputs at half size.
  vfx_shape_transform_draw(screen, row,
                           wm_.shared_shapes(kResearchShapes).at(output_frame(static_cast<std::uint16_t>(planet.research))),
                           0x118, 0x3c, kHalfScale, kHalfScale);
  vfx_shape_transform_draw(screen, row,
                           wm_.shared_shapes(kIndustryShapes).at(output_frame(static_cast<std::uint16_t>(planet.industry))),
                           0x15e, 0x3c, kHalfScale, kHalfScale);
  const std::size_t prosperity = std::min<std::size_t>(static_cast<std::uint16_t>(planet.prosperity), 20);
  vfx_shape_transform_draw(screen, row, wm_.shared_shapes(kProsperityShapes).at(prosperity), 0x1a4, 0x3c, kHalfScale,
                           kHalfScale);
  // The population: one 5x5 pip per unit of the maximum, ten per row.
  const ShapeTable& pips = wm_.shared_shapes(kPlanetScreenShapes);
  int frame = kWorkerPip;
  int px = 0x1ea;
  int py = 0x3c;
  for (int i = 0; i < planet.max_population; ++i) {
    if (i == planet.used_population) {
      frame = kIdlePip;
    }
    if (i == planet.population) {
      frame = kRoomPip;
    }
    vfx_shape_draw(screen, row, pips.at(static_cast<std::size_t>(frame)), px, py);
    px += 5;
    if (i % 10 == 9) {
      py += 5;
      px = 0x1ea;
    }
  }
}

void add_status_windows(WindowFactory& factory) {
  factory.add(16, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<StatusWnd>(wm, record, shapes);
  });
}

}  // namespace oa
