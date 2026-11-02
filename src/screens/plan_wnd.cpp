#include "screens/plan_wnd.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>

#include "core/c_format.h"
#include "core/error.h"
#include "game/address.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/planet_plan.h"
#include "game/player_actions.h"
#include "game/race.h"
#include "game/race_ai.h"
#include "game/research.h"
#include "game/ship.h"
#include "game/ship_design.h"
#include "screens/management_menu.h"
#include "screens/ship_des_wnd.h"
#include "ui/help_wnd.h"

namespace oa {

namespace {

// `gwshare.txt` tables.
constexpr int kShipShapes = 14;  // smship00..06, by race slot
constexpr int kGizmoShapes = 28;
constexpr int kPlanetShapes = 29;
constexpr int kItemShapes = 30;  // planitem.shp
constexpr int kHelpShapes = 37;
constexpr int kResearchPanel = 38;  // planres, planind, planpro
constexpr int kScreenShapes = 44;   // 17np.shp

// `17np.shp` frames.
constexpr int kStarfield = 4;
constexpr int kListBorder = 5;
constexpr int kPipFirst = 1;  // 1 working, 2 idle, 3 room to grow

// The ruins' frame of `planitem.shp`.
constexpr int kRuinsFrame = 39;

// The haze tables of `windows.txt`.
constexpr std::size_t kHazeBlack24 = 7;
constexpr std::size_t kHazeRed = 8;
constexpr std::size_t kHazeGreen = 9;
constexpr std::size_t kHazeBlue = 10;
constexpr std::size_t kHazeWhite02 = 13;
constexpr std::size_t kHazeHilite = 21;

constexpr std::uint8_t kHotColor = 0x96;

// peCustom: the bottom right corner of the management label.
constexpr int kManagementRight = 628;
constexpr int kManagementBottom = 466;
// peCustom: the left end of the planet's name and labels (right of the
// orbital squares' two columns, `planet_geometry::orbital_cell`), the gap
// between name, label symbol and labels, and the symbol's width.
constexpr int kLabelsLeft = planet_geometry::orbital_cell(9).x + 70 + 9;
constexpr int kLabelGap = 6;
constexpr int kLabelIconWidth = 16;

// static.txt.
constexpr int kStringNone = 0x1c;
constexpr int kStringPlural = 0x1d;
constexpr int kStringOk = 0x1e;
constexpr int kStringNameColony = 0x1f;
constexpr int kStringNameShip = 0x22;
constexpr int kStringRenameShip = 0x23;
constexpr int kStringColonySingular = 0x20;
constexpr int kStringColonyPlural = 0x21;
constexpr int kStringNoProject = 0x24;
constexpr int kStringPickLocation = 0x25;
constexpr int kStringDaysUntil = 0x26;
constexpr int kStringNoProgress = 0x27;
constexpr int kStringNoFreePopulation = 0x28;
constexpr int kStringSizes = 0x29;
constexpr int kStringTypes = 0x2e;
constexpr int kStringButtons = 0x39;  // by command - 0x33
constexpr int kStringResearch = 0x41;
constexpr int kStringBeDiscovered = 0x42;
constexpr int kStringNoProgressOn = 0x43;
constexpr int kStringIndustry = 0x44;
constexpr int kStringBeCompleted = 0x45;
constexpr int kStringProsperity = 0x46;
constexpr int kStringPopulation = 0x47;
constexpr int kStringGrow = 0x48;
constexpr int kStringNotGrowing = 0x49;
constexpr int kStringNoRoom = 0x4a;
constexpr int kStringWhite = 0x4b;
constexpr int kStringBlack = 0x4c;
constexpr int kStringRed = 0x4d;
constexpr int kStringRedOutput = 0x4e;
constexpr int kStringGreen = 0x4f;
constexpr int kStringGreenOutput = 0x50;
constexpr int kStringBlue = 0x51;
constexpr int kStringBlueOutput = 0x52;
constexpr int kStringOrbital = 0x53;
constexpr int kStringAutomated = 0x54;
constexpr int kStringOnSquare = 0x55;
constexpr int kStringNoProgressSquare = 0x56;
constexpr int kStringSquareDays = 0x57;
constexpr int kStringRuins = 0x58;
constexpr int kStringSquare = 0x59;
constexpr int kStringOrbitalHelp = 0x5a;
constexpr int kStringWhiteHelp = 0x5b;
constexpr int kStringBlackHelp = 0x5c;
constexpr int kStringColorHelp = 0x5d;
constexpr int kStringPlanetKind = 0x5e;
constexpr int kStringNoOne = 0x5f;
constexpr int kStringOwnedBy = 0x60;
constexpr int kStringMaxPopulation = 0x61;
constexpr int kStringSelfManaged = 0x62;
constexpr int kStringPerDay = 0x63;
constexpr int kStringWillIn = 0x64;
constexpr int kStringSpecies = 0x65;

// Scan codes of the screen's keys.
constexpr int kScanEsc = 0x01;
constexpr int kScanS = 0x1f;
constexpr int kScanM = 0x32;

constexpr int kScreenShipDesign = 8;
constexpr int kInputLength = 20;

// The planet items and gizmos the screen tests.
constexpr std::uint8_t kSurfaceCloaker = 11;
constexpr std::uint8_t kOrbitalDocks = 24;
constexpr std::uint8_t kOrbitalCloaker = 25;

// The edges of the diamonds (`0x866e4`): vertex pairs.
constexpr std::array<std::array<int, 2>, 4> kEdges{{{0, 3}, {1, 2}, {3, 2}, {0, 1}}};

std::string name_of(const auto& name) { return std::string(game::name_view(name)); }

void draw_edges(Image8& screen, const Pane& pane, const std::array<PolyPoint, 4>& v, std::uint8_t color) {
  for (const auto& e : kEdges) {
    vfx_line_draw(screen, pane, v[static_cast<std::size_t>(e[0])].x, v[static_cast<std::size_t>(e[0])].y,
                  v[static_cast<std::size_t>(e[1])].x, v[static_cast<std::size_t>(e[1])].y, color);
  }
}

// A table filled with `color`: the silhouette of a project under
// construction.
HazeTable flat_table(std::uint8_t color) {
  HazeTable t{};
  t.fill(color);
  return t;
}

// The square's colour byte, ruins bit included.
std::uint8_t color_of(const game::Square& sq) { return sq.color; }

bool square_built(const game::Square& sq) { return (sq.flags & game::kSquareBuilt) != 0; }

// The owner of the ship on `square` (a Ship item).
std::int16_t ship_owner(const game::Session& s, const game::Planet& p, std::uint16_t square) {
  const game::ShipIndex ship = game::planet_square_ship(s, p, square);
  return s.game->cosmos.ships.at(static_cast<std::size_t>(ship)).owner;
}

}  // namespace

// ---------------------------------------------------------------------------
// PlanWnd

game::Session* PlanWnd::session() const { return wm_.services().game.core(); }

game::Planet* PlanWnd::planet() const {
  game::Session* s = session();
  if (s == nullptr || s->game->cosmos.selected_planet < 0) {
    return nullptr;
  }
  return &s->game->cosmos.planets.at(static_cast<std::size_t>(s->game->cosmos.selected_planet));
}

ListBox& PlanWnd::list() const { return dynamic_cast<ListBox&>(*wm_.find_window("PLANLIST")); }

GWindow& PlanWnd::dialog() const { return *wm_.find_window("PLSQUARE"); }

std::uint16_t PlanWnd::square_at_point(int x, int y) const {
  const game::Session* s = session();
  const game::Planet* p = planet();
  if (s == nullptr || p == nullptr) {
    return kNoSquare;
  }
  if (!planet_geometry::in_orbit_area(x, y)) {
    const planet_geometry::RowColumn rc = planet_geometry::screen_to_rowcol(x, y);
    return game::planet_square_at(s->tables->planitems, *p, rc.row, rc.column);
  }
  return static_cast<std::uint16_t>(p->surface_squares + (x - 13) / 70 * 5 + (y - 119) / 70);
}

PlanShape PlanWnd::item_shape(std::uint8_t item) const {
  if (item > 38 && item != kNoSelection) {
    throw std::out_of_range("plan_item_shape: no such item");
  }
  PlanShape shape{kItemShapes, item == kNoSelection ? 0 : item};
  if (item == game::kItemShip) {
    shape = {kShipShapes + planet()->owner, 0};
  }
  return shape;
}

PlanShape PlanWnd::square_shape(std::uint16_t square) const {
  const game::Session& s = *session();
  const game::Planet& p = *planet();
  if (square >= p.square_count) {
    throw std::out_of_range("plan_square_shape: no such square");
  }
  const game::Square& sq = game::planet_squares(s.game->cosmos, p)[square];
  PlanShape shape{kItemShapes, 0};
  if (sq.item == game::kNoItem) {
    if ((sq.color & game::kSquareRuins) != 0) {
      shape.frame = kRuinsFrame;
    }
  } else {
    shape.frame = sq.item;
  }
  if (sq.item == game::kItemShip) {
    const game::Ship& ship = s.game->cosmos.ships.at(static_cast<std::size_t>(game::planet_square_ship(s, p, square)));
    shape = {kShipShapes + ship.owner, ship.hull_size};
  }
  return shape;
}

bool PlanWnd::handle_event(int type, int a, int b) {
  switch (type) {
    case kEventActivate: {
      grey_panels();
      for (const char* name : {"PLRES", "PLIND", "PLPRO", "PLPOP", "PLBUILD"}) {
        if (GWindow* w = wm_.find_window(name)) {
          w->handle_event(kEventActivate, a, b);
        }
      }
      shown_ = true;
      active_ = true;
      selection_ = kNoSelection;
      hovered_ = kNoSquare;
      square_ = kNoSquare;
      if (design_square_ != kNoSquare) {
        finish_ship_design();
      }
      draw(kDrawNormal);
      if (name_ship_ != nullptr) {
        std::string name = name_of(name_ship_->name);
        wm_.input_box(wm_.static_string(name_title_), name, kInputLength);
        game::ship_set_name(*name_ship_, name);
        name_ship_ = nullptr;
      }
      return false;
    }
    case kEventKey:
      key(b);
      return false;
    case kEventLeftClick:
      if (!shown_) {
        return false;
      }
      if (management_hot(a, b)) {
        game::Planet* const planets[] = {planet()};
        run_management_menu(wm_, *session(), planets);
        draw(kDrawNormal);
        return false;
      }
      if (label_hot(a, b)) {
        game::Planet* const planets[] = {planet()};
        run_label_menu(wm_, *session(), planets);
        draw(kDrawNormal);
        return false;
      }
      left_click();
      if (design_square_ != kNoSquare) {
        ship_design(design_square_, false);
      } else {
        hover(a, b);
      }
      return false;
    case kEventRightClick:
      selection_ = kNoSelection;
      square_ = kNoSquare;
      draw(kDrawNormal);
      return true;
    case kEventMouseLeave:
      wm_.cancel_notify(id_, kEventMouseMove);
      if (active_ && shown_ && mouse_focus_) {
        draw(kDrawNormal);
      }
      return true;
    case kEventMouseEnter:
      wm_.request_notify(id_, WinMgr::kNotifyMove, kEventMouseMove, 0, 0);
      if (active_ && shown_ && mouse_focus_) {
        draw_hilite(kDrawNormal);
      }
      return true;
    case kEventMouseMove:
      hover(a, b);
      return false;
    case kEventDialog: {
      hovered_ = kNoSquare;
      const InputState& in = wm_.input();
      switch (a) {
        case 10:
          dialog().handle_event(kEventDeactivate, 0, 0);
          grey_panels();
          hover(in.x, in.y);
          return false;
        case 11:
          dialog().handle_event(kEventDeactivate, 0, 0);
          open_build_list(0);
          return false;
        case 12:
          dialog().handle_event(kEventDeactivate, 0, 0);
          selection_ = game::kItemColonyBase;
          hover(in.x, in.y);
          return false;
        default:
          open_dialog(a);
          return false;
      }
    }
    case kEventBuildList:
      open_build_list(a);
      return false;
    case kEventShipDesign:
      ship_design(static_cast<std::uint16_t>(a), b != 0);
      return false;
    case kEventListPick:
      pick_build_item(a);
      return false;
    case kEventListHelp:
      // A help-mode click on the list: the item's help.
      if (a >= 0 && a < static_cast<int>(build_items_.size())) {
        wm_.post_message(kMessageHelp, build_items_[static_cast<std::size_t>(a)], kHelpPlanet);
      }
      return true;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

bool PlanWnd::save_state(std::span<std::uint8_t> out) const {
  if (out.empty()) {
    return false;
  }
  out[0] = flags_;
  return true;
}

void PlanWnd::load_state(std::span<const std::uint8_t> in) {
  if (!in.empty()) {
    flags_ = in[0];
  }
}

Pane PlanWnd::management_pane() const {
  const game::Session* s = session();
  const game::Planet* p = planet();
  if (s == nullptr || p == nullptr || !management_shown(*s, *p)) {
    return Pane{0, 0, -1, -1};
  }
  GameFont& font = wm_.large_font();
  const int width = font.string_width(management_label(wm_, *s, *p));
  const int height = font.font().height;
  return Pane{kManagementRight - width, kManagementBottom - height, kManagementRight, kManagementBottom};
}

Pane PlanWnd::label_pane() const {
  const game::Session* s = session();
  const game::Planet* p = planet();
  if (s == nullptr || p == nullptr || !management_shown(*s, *p)) {
    return Pane{0, 0, -1, -1};
  }
  GameFont& font = wm_.large_font();
  const int x0 = kLabelsLeft + font.string_width(name_of(p->name)) + kLabelGap;
  const int width = kLabelIconWidth + kLabelGap + font.string_width(planet_labels_text(*s, *p));
  return Pane{x0, kManagementBottom - font.font().height, x0 + width, kManagementBottom};
}

bool PlanWnd::label_hot(int x, int y) const {
  return !list().shown() && !dialog().shown() && selection_ == kNoSelection && label_pane().contains(x, y);
}

bool PlanWnd::management_hot(int x, int y) const {
  return !list().shown() && !dialog().shown() && selection_ == kNoSelection && management_pane().contains(x, y);
}

void PlanWnd::hover(int x, int y) {
  const game::Session* s = session();
  const game::Planet* p = planet();
  if (s == nullptr || p == nullptr || list().shown() || dialog().shown()) {
    return;
  }
  hovered_ = square_at_point(x, y);
  const std::span<const game::Square> squares = game::planet_squares(s->game->cosmos, *p);
  const auto foreign_ship = [&](std::uint16_t sq) {
    return squares[sq].item != game::kItemShip || ship_owner(*s, *p, sq) != s->player_race;
  };
  // On another race's planet only the player's own ships can be hovered
  // (but for the Colony Base placement); nowhere another race's ship.
  if (p->owner != s->player_race && selection_ != game::kItemColonyBase && hovered_ != kNoSquare &&
      foreign_ship(hovered_)) {
    hovered_ = kNoSquare;
  }
  if (hovered_ != kNoSquare && squares[hovered_].item == game::kItemShip &&
      ship_owner(*s, *p, hovered_) != s->player_race) {
    hovered_ = kNoSquare;
  }
  draw(kDrawNormal);
}

void PlanWnd::left_click() {
  game::Session* s = session();
  game::Planet* p = planet();
  if (s == nullptr || p == nullptr) {
    return;
  }
  if (list().shown()) {
    list().handle_event(kEventDeactivate, 0, 0);
    return;
  }
  if (dialog().shown()) {
    dialog().handle_event(kEventDeactivate, 0, 0);
    return;
  }
  square_ = hovered_;
  if (square_ == kNoSquare) {
    selection_ = kNoSelection;
    return;
  }
  if (sound_ != -1) {
    wm_.services().sound.play_sfx(sound_);
  }
  if (selection_ == kNoSelection) {
    handle_event(kEventDialog, PlSquareWnd::kSquare, 0);
    return;
  }
  if (!game::planet_can_place_item(*s, *p, selection_, square_)) {
    return;
  }
  if (selection_ == game::kItemColonyBase) {
    colonize();
    return;
  }
  const bool placed = game::planet_set_square_item(*s, *p, square_, selection_, game::SquareMode::kProject);
  if (selection_ == game::kItemShip && placed) {
    design_square_ = square_;
  }
  selection_ = kNoSelection;
}

void PlanWnd::grey_panels() {
  const game::Session* s = session();
  const game::Planet* p = planet();
  const bool foreign = s != nullptr && p != nullptr && p->owner != s->player_race;
  for (const char* name : {"PLRES", "PLIND", "PLPRO", "PLBUILD"}) {
    if (GWindow* w = wm_.find_window(name)) {
      w->set_greyed(foreign);
    }
  }
}

void PlanWnd::colonize() {
  game::Session& s = *session();
  game::Planet& p = *planet();
  const auto& ship_dialog = dynamic_cast<const PlSquareWnd&>(dialog());
  std::string name = name_of(p.name);
  wm_.input_box(wm_.static_string(kStringNameColony), name, kInputLength);
  const game::ColonizeResult r = game::planet_colonize(s, p, ship_dialog.square(), square_, name);
  auto& help = dynamic_cast<HelpWnd&>(*wm_.find_window("HELPWINDOW"));
  const game::Cosmos& cosmos = s.game->cosmos;
  help.open_topic("help.txt", r.best_race == s.player_race ? "maxcolonize" : "colonize");
  HelpTopic& topic = help.content();
  const std::string player_species = wm_.static_string(kStringSpecies + cosmos.races.at(s.player_race).species);
  const std::string best_species = wm_.static_string(kStringSpecies + cosmos.races.at(r.best_race).species);
  topic.elements[0].text =
      c_format(topic.elements[0].text, player_species.c_str(), r.player_colonies,
               wm_.static_string(r.player_colonies == 1 ? kStringColonySingular : kStringColonyPlural).c_str(),
               best_species.c_str(), r.best_colonies);
  topic.elements[1].shape_index = kPlanetShapes;
  topic.elements[1].frame = p.type * 5 + p.size;
  topic.elements[2].text = c_format(topic.elements[2].text, name_of(p.name).c_str());
  wm_.post_message(kMessageHelp, 0, kHelpTopic);
  grey_panels();
  selection_ = kNoSelection;
}

void PlanWnd::open_dialog(int mode) {
  const game::Session* s = session();
  const game::Planet* p = planet();
  if (s == nullptr || p == nullptr) {
    return;
  }
  const std::span<const game::Square> squares = game::planet_squares(s->game->cosmos, *p);
  const bool own_ship =
      square_ != kNoSquare && squares[square_].item == game::kItemShip && ship_owner(*s, *p, square_) == s->player_race;
  if (mode != PlSquareWnd::kPlanet && p->owner != s->player_race && !own_ship) {
    return;
  }
  GWindow& d = dialog();
  d.handle_event(kEventDialog, mode, square_);
  broadcast_to_children(kEventMouseLeave, 0, 0);
  d.handle_event(kEventActivate, 0, 0);
  draw(kDrawNormal);
}

void PlanWnd::open_build_list(int from) {
  const game::Session* s = session();
  const game::Planet* p = planet();
  if (s != nullptr && p != nullptr && p->owner == s->player_race) {
    if (from == 1) {
      square_ = kNoSquare;
    }
    ListBox& l = list();
    l.set_line_height(0x4b);
    l.set_colors(0x31, kBackColor, kBackColor);
    l.set_draw_item(
        [this](const Pane& row, const ListBox::Item& item, bool hilite) { draw_build_row(row, item, hilite); });
    l.clear();
    build_items_.clear();
    const game::PlanItemTable& items = s->tables->planitems;
    for (std::uint8_t item = 0; item < game::PlanItemTable::kItems; ++item) {
      if (item != game::kItemColonyBase && game::planet_can_place_item(*s, *p, item, square_)) {
        build_items_.push_back(item);
        const int row = l.add_item(items.items[item].name);
        l.set_item_data(row, item);
      }
    }
    if (!build_items_.empty()) {
      broadcast_to_children(kEventMouseLeave, 0, 0);
      l.handle_event(kEventActivate, 0, 0);
    }
  }
  draw(kDrawNormal);
}

void PlanWnd::pick_build_item(int row) {
  game::Session* s = session();
  game::Planet* p = planet();
  if (s == nullptr || p == nullptr || row < 0 || row >= static_cast<int>(build_items_.size())) {
    return;
  }
  const std::uint8_t item = build_items_[static_cast<std::size_t>(row)];
  selection_ = item;
  if (square_ == kNoSquare) {
    if ((s->tables->planitems.items[item].flags & game::kItemWholePlanet) != 0) {
      game::planet_set_square_item(*s, *p, kNoSquare, item, game::SquareMode::kProject);
      selection_ = kNoSelection;
    }
  } else {
    if (game::planet_can_place_item(*s, *p, item, square_)) {
      if (item == game::kItemShip) {
        design_square_ = square_;
      }
      game::planet_set_square_item(*s, *p, square_, item, game::SquareMode::kProject);
    }
    selection_ = kNoSelection;
    hovered_ = kNoSquare;
    square_ = kNoSquare;
  }
  list().handle_event(kEventDeactivate, 0, 0);
  if (design_square_ != kNoSquare) {
    ship_design(design_square_, false);
    return;
  }
  draw(kDrawNormal);
}

void PlanWnd::ship_design(std::uint16_t square, bool refit) {
  game::Session& s = *session();
  game::Planet& p = *planet();
  if (!refit) {
    game::Ship* ship = game::race_ai_new_ship(s, s.game->cosmos.races.at(p.owner), p);
    if (ship == nullptr) {
      throw std::logic_error("PlanWnd: no free ship slot");
    }
    game::planet_set_square_ship(s, p, square, game::ship_index(s.game->cosmos, *ship));
  } else {
    design_square_ = dynamic_cast<const PlSquareWnd&>(dialog()).square();
  }
  game::Ship& ship = s.game->cosmos.ships.at(static_cast<std::size_t>(game::planet_square_ship(s, p, square)));
  design_window().open(&ship, refit ? ShipDesWnd::kRefit : ShipDesWnd::kNewDesign, false);
  wm_.post_message(kMessageChangeScreen, kScreenShipDesign, 1);
}

ShipDesWnd& PlanWnd::design_window() const {
  auto* design = dynamic_cast<ShipDesWnd*>(wm_.find_window("SHIPDESSCREEN"));
  if (design == nullptr) {
    throw DataError("windows.txt: no SHIPDESSCREEN");
  }
  return *design;
}

void PlanWnd::finish_ship_design() {
  game::Session& s = *session();
  game::Planet& p = *planet();
  const ShipDesWnd& design = design_window();
  // The design screen forgets its ship when left without a change: a new
  // ship is then destroyed, a refit left as it was.
  name_ship_ = design.ship();
  name_title_ = kStringNameShip;
  if (name_ship_ == nullptr) {
    if (design.mode() == ShipDesWnd::kNewDesign) {
      game::ship_design_abandon(s, p, design_square_);
    }
  } else if (design.mode() == ShipDesWnd::kRefit) {
    name_title_ = kStringRenameShip;
    game::ship_design_accept_refit(s, p, design_square_, *name_ship_, design.refit_cost(s));
  }
  design_square_ = kNoSquare;
}

void PlanWnd::key(int scan) {
  switch (scan) {
    case kScanEsc:
      if (list().shown()) {
        list().handle_event(kEventDeactivate, 0, 0);
      } else if (dialog().shown()) {
        dialog().handle_event(kEventDeactivate, 0, 0);
      } else {
        wm_.post_message(kMessageBack, 0, 0);
      }
      break;
    case kScanS:
      flags_ ^= kColoredSquares;
      break;
    case kScanM:
      // Shift+M is a developer key (`nougat.lf`).
      if (!wm_.input().shift) {
        game::Session* s = wm_.services().game.core();
        if (game::Planet* p = planet(); p != nullptr && s != nullptr && management_shown(*s, *p)) {
          // peCustom: the strategies in turn (off, then each one).
          game::planet_set_management(
              *s, *p,
              game::planet_next_strategy(p->self_managed != 0 ? std::optional(game::planet_goal(*s, *p)) : std::nullopt,
                                         1));
          game::pe_manage_planet_now(*s, *p);
        } else if (p != nullptr) {
          game::planet_toggle_self_managed(*p);
        }
      }
      break;
    default:
      break;
  }
  draw(kDrawNormal);
}

void PlanWnd::draw(int /*mode*/) {
  const game::Session* s = session();
  const game::Planet* p = planet();
  if (s == nullptr || p == nullptr) {
    return;
  }
  Image8& screen = wm_.screen();
  const ShapeTable& np = wm_.shared_shapes(kScreenShapes);
  if (frame_ != -1) {
    vfx_shape_draw(screen, pane_, np.at(kStarfield), 7, 0x73);
    if ((flags_ & kPlanetPicture) != 0) {
      const ShapeTable& planal = wm_.shapes().get(std::format("data\\planal{:02d}.shp", p->type));
      vfx_shape_draw(screen, pane_, planal.at(p->size), 0x198, 0x128);
    }
    vfx_shape_draw(screen, pane_, np.at(static_cast<std::size_t>(frame_)), 0, 0);
  }
  if (p->owner == s->player_race || game::planet_find_item(s->game->cosmos, *p, kSurfaceCloaker) == kNoSquare) {
    draw_surface(*s, *p);
  }
  draw_orbit(*s, *p);
  if (management_shown(*s, *p)) {
    // peCustom: the planet's management, a click opens its menu.
    GameFont& font = wm_.large_font();
    font.pane() = GSystem::kScreenPane;
    const Pane area = management_pane();
    const int color = management_hot(wm_.input().x, wm_.input().y)
                          ? kHotColor
                          : s->game->cosmos.races.at(s->player_race).color * 4 + 0x13;
    font.draw_wrapped(screen, area.x0, area.y0, management_label(wm_, *s, *p), 0, color, 0xff, 0);
    // Bottom left: the name, the label symbol and the labels.
    const Pane labels = label_pane();
    const int name_color = s->game->cosmos.races.at(s->player_race).color * 4 + 0x13;
    const int label_color = label_hot(wm_.input().x, wm_.input().y) ? kHotColor : name_color;
    font.draw_wrapped(screen, kLabelsLeft, labels.y0, name_of(p->name), 0, name_color, 0xff, 0);
    draw_label_icon(screen, labels.x0, labels.y0 + 2, static_cast<std::uint8_t>(label_color));
    font.draw_wrapped(screen, labels.x0 + kLabelIconWidth + kLabelGap, labels.y0, planet_labels_text(*s, *p), 0,
                      label_color, 0xff, 0);
  }
  if (list().shown()) {
    vfx_shape_draw(screen, Pane{0x70, 0x70, 0x210, 0x1ac}, np.at(kListBorder), 0, 0);
  }
  draw_children();
}

void PlanWnd::draw_surface(const game::Session& s, const game::Planet& p) {
  Image8& screen = wm_.screen();
  const Pane& pane = GSystem::kScreenPane;
  const std::vector<HazeTable>& haze = wm_.services().gsystem.haze_tables();
  const game::PlanItemTable& items = s.tables->planitems;
  const std::span<const game::Square> squares = game::planet_squares(s.game->cosmos, p);
  // The square showing the selected item's preview (0xfffe: none, an item
  // being selected).
  std::uint16_t preview = kNoSquare;
  if (selection_ != kNoSelection) {
    preview = 0xfffe;
    if (hovered_ < kNoSquare && squares[hovered_].item == game::kNoItem &&
        game::planet_can_place_item(s, p, selection_, hovered_)) {
      preview = hovered_;
    }
  }
  std::uint16_t sq = 0;
  for (int r = 0; r < static_cast<int>(game::PlanItemTable::kColumns); ++r) {
    const int start = items.column_start[p.size][static_cast<std::size_t>(r)];
    const int end = start + items.column_height[p.size][static_cast<std::size_t>(r)];
    for (int c = start; c < end; ++c, ++sq) {
      const PolyPoint top = planet_geometry::diamond_top(r, c);
      const std::array<PolyPoint, 4> v = planet_geometry::diamond(top);
      const game::Square& square = squares[sq];
      const std::uint8_t color = color_of(square);
      const bool buildable = game::planet_can_place_item(s, p, game::kNoItem, sq);
      const bool hot_empty = hovered_ == sq && selection_ == kNoSelection && square.item == game::kNoItem;
      if (hot_empty && buildable) {
        vfx_flat_polygon(screen, pane, v, kHotColor);
      } else if ((flags_ & kColoredSquares) != 0) {
        std::size_t table = kHazeWhite02;
        std::uint8_t line = 0x0c;
        switch (color) {
          case 0:
            table = kHazeBlack24;
            line = 0;
            break;
          case 2:
            table = kHazeRed;
            line = 0x75;
            break;
          case 3:
            table = kHazeGreen;
            line = 0x92;
            break;
          case 4:
            table = kHazeBlue;
            line = 0xff;
            break;
          default:
            break;
        }
        // Buildable squares (next to a built one) get the whole diamond,
        // the others a small one.
        const std::array<PolyPoint, 4> inset =
            buildable
                ? std::array<PolyPoint, 4>{{{v[0].x, v[0].y + 1},
                                            {v[1].x - 2, v[1].y},
                                            {v[2].x, v[2].y - 1},
                                            {v[3].x + 2, v[3].y}}}
                : std::array<PolyPoint, 4>{
                      {{v[0].x, v[0].y + 14}, {v[1].x - 28, v[1].y}, {v[2].x, v[2].y - 14}, {v[3].x + 28, v[3].y}}};
        draw_edges(screen, pane, inset, line);
        vfx_translate_polygon(screen, pane, inset, haze.at(table));
      }
      if (hot_empty) {
        draw_edges(screen, pane, v, kHotColor);
      }
      if (square.item == game::kNoItem && (color & game::kSquareRuins) == 0) {
        if (preview == sq) {
          const PlanShape shape = item_shape(selection_);
          vfx_shape_draw(screen, pane, wm_.shared_shapes(shape.table).at(static_cast<std::size_t>(shape.frame)), top.x,
                         top.y);
        }
        continue;
      }
      const PlanShape ps = square_shape(sq);
      const Shape& shape = wm_.shared_shapes(ps.table).at(static_cast<std::size_t>(ps.frame));
      const bool hot = hovered_ == sq;
      if (p.project_square == sq) {
        // The building rises as it is built: its silhouette, then the shape
        // clipped from the bottom to the progress.
        vfx_flat_polygon(screen, pane, v, kHotColor);
        const std::uint32_t cost = game::planet_project_cost(s, p, sq, game::kNoItem) & 0xffff;
        const int height = cost == 0 ? 0 : static_cast<int>(static_cast<std::uint32_t>(p.progress) * 34u / cost);
        vfx_shape_translate_draw(screen, pane, shape, top.x, top.y, flat_table(hot ? kHotColor : kBackColor));
        Pane clipped = pane;
        clipped.y0 = top.y + 34 - height;
        if (hot) {
          vfx_shape_translate_draw(screen, clipped, shape, top.x, top.y - clipped.y0, haze.at(kHazeHilite));
        } else {
          vfx_shape_draw(screen, clipped, shape, top.x, top.y - clipped.y0);
        }
      } else if (hot) {
        vfx_shape_translate_draw(screen, pane, shape, top.x, top.y, haze.at(kHazeHilite));
      } else {
        vfx_shape_draw(screen, pane, shape, top.x, top.y);
      }
    }
  }
}

void PlanWnd::draw_orbit(const game::Session& s, const game::Planet& p) {
  Image8& screen = wm_.screen();
  const Pane& pane = GSystem::kScreenPane;
  const std::vector<HazeTable>& haze = wm_.services().gsystem.haze_tables();
  const std::span<const game::Square> squares = game::planet_squares(s.game->cosmos, p);
  const bool cloaked =
      p.owner != s.player_race && game::planet_find_item(s.game->cosmos, p, kOrbitalCloaker) != kNoSquare;
  for (std::uint16_t sq = p.surface_squares; sq < p.square_count; ++sq) {
    const PolyPoint cell = planet_geometry::orbital_cell(sq - p.surface_squares);
    const int cx = cell.x + 35;
    const int cy = cell.y + 35;
    const game::Square& square = squares[sq];
    const bool own_ship = square.item == game::kItemShip && ship_owner(s, p, sq) == s.player_race;
    if (hovered_ == sq && square.item == game::kNoItem && !cloaked && selection_ == kNoSelection) {
      // draw_rect_frame (0x2b294) in the cell's pane.
      const Pane cell_pane{cell.x, cell.y, cell.x + 70, cell.y + 70};
      vfx_line_draw(screen, cell_pane, 1, 1, 0x45, 1, kHotColor);
      vfx_line_draw(screen, cell_pane, 1, 0x45, 0x45, 0x45, kHotColor);
      vfx_line_draw(screen, cell_pane, 1, 1, 1, 0x45, kHotColor);
      vfx_line_draw(screen, cell_pane, 0x45, 1, 0x45, 0x45, kHotColor);
    }
    if (square.item == game::kNoItem) {
      // The preview of the selected item.
      if (hovered_ == sq && selection_ != kNoSelection && square.item == game::kNoItem &&
          game::planet_can_place_item(s, p, selection_, sq)) {
        const PlanShape ps = item_shape(selection_);
        const std::int32_t scale = selection_ == game::kItemShip ? 0x8000 : kScaleOne;
        vfx_shape_transform_draw(screen, pane, wm_.shared_shapes(ps.table).at(static_cast<std::size_t>(ps.frame)), cx,
                                 cy, scale, scale);
      }
      continue;
    }
    const PlanShape ps = square_shape(sq);
    const Shape& shape = wm_.shared_shapes(ps.table).at(static_cast<std::size_t>(ps.frame));
    const std::int32_t scale = square.item == game::kItemShip ? 0x8000 : kScaleOne;
    const bool visible = !cloaked || own_ship;
    Pane clipped = pane_;
    if (p.project_square == sq) {
      const std::uint32_t cost = game::planet_project_cost(s, p, sq, game::kNoItem) & 0xffff;
      const int height = cost == 0 ? 0 : static_cast<int>(static_cast<std::uint32_t>(p.progress) * 70u / cost);
      if (visible) {
        const HazeTable silhouette = flat_table(hovered_ == sq ? kHotColor : kBackColor);
        vfx_shape_transform_draw(screen, pane, shape, cx, cy, scale, scale, &silhouette);
      }
      clipped.y0 = cell.y + 70 - height;
    }
    if (visible) {
      vfx_shape_transform_draw(screen, clipped, shape, cx, cy - clipped.y0, scale, scale,
                               hovered_ == sq ? &haze.at(kHazeHilite) : nullptr);
    }
  }
}

void PlanWnd::draw_build_row(const Pane& row, const ListBox::Item& item, bool hilite) {
  Image8& screen = wm_.screen();
  vfx_pane_wipe(screen, row, hilite ? kHotColor : kBackColor);
  const auto id = static_cast<std::uint8_t>(item.data);
  if (id != kNoSelection) {
    const PlanShape ps = item_shape(id);
    const Shape& shape = wm_.shared_shapes(ps.table).at(static_cast<std::size_t>(ps.frame));
    const PolyPoint at = center_shape(row, shape);
    const std::int32_t scale = id == game::kItemShip ? 0x8000 : kScaleOne;
    vfx_shape_transform_draw(screen, row, shape, 0x28, at.y, scale, scale);
  }
  GameFont& font = wm_.large_font();
  font.pane() = row;
  font.draw_wrapped(screen, 0x50, 0, item.text, kFontCenterV, kDefault, 0xff, 0);
}

// ---------------------------------------------------------------------------
// PlanStatButton

bool PlanStatButton::handle_event(int type, int a, int b) {
  const GWindow* list = wm_.find_window("PLANLIST");
  const GWindow* square = wm_.find_window("PLSQUARE");
  if ((list != nullptr && list->shown()) || (square != nullptr && square->shown())) {
    if (type != kEventHide && type != kEventShow) {
      return false;
    }
  }
  if (type == kEventLeftClick && bell_shown() && a >= pane_.x0 + kBellX - 2 && a < pane_.x0 + kBellX + kBellSize + 2 &&
      b >= pane_.y0 + kBellY - 2 && b < pane_.y0 + kBellY + kBellSize + 2) {
    game::Session& s = *wm_.services().game.core();
    std::uint8_t& bell = s.game->planet_management.at(static_cast<std::size_t>(s.game->cosmos.selected_planet)).bell;
    bell = bell == 0 ? 1 : 0;
    draw(kDrawHot);
    return true;
  }
  if (type == kEventMouseLeave) {
    draw(kDrawNotHot);
    return false;
  }
  if (type == kEventMouseEnter) {
    draw(1);
    return false;
  }
  return GWindow::handle_event(type, a, b);
}

bool PlanStatButton::bell_shown() const {
  const game::Session* s = wm_.services().game.core();
  const auto* plan = dynamic_cast<const PlanWnd*>(wm_.find_window("PLANETALLOC"));
  if (message_ != PlanWnd::kEventBuildList || s == nullptr || s->game->cosmos.selected_planet < 0 || plan == nullptr ||
      plan->selection() != PlanWnd::kNoSelection) {
    return false;
  }
  const game::Planet& p = s->game->cosmos.planets.at(static_cast<std::size_t>(s->game->cosmos.selected_planet));
  return management_shown(*s, p) && p.self_managed != 0 && p.project_item != game::kNoItem &&
         (s->tables->planitems.items[p.project_item].flags & game::kItemContinuous) == 0;
}

void PlanStatButton::draw(int mode) {
  game::Session* s = wm_.services().game.core();
  if (s == nullptr || s->game->cosmos.selected_planet < 0) {
    return;
  }
  const game::Planet& p = s->game->cosmos.planets.at(static_cast<std::size_t>(s->game->cosmos.selected_planet));
  Image8& screen = wm_.screen();
  bool hot = mode != kDrawNotHot && (wm_.hot_window() == id_ || mode == 1);
  const GWindow* list = wm_.find_window("PLANLIST");
  const auto* plan = dynamic_cast<const PlanWnd*>(wm_.find_window("PLANETALLOC"));
  const GWindow* square = wm_.find_window("PLSQUARE");
  if (list->shown() || square->shown() || greyed()) {
    hot = false;
  }
  int kind = param1_ == 4 ? 3 : param1_;
  if (message_ == PlanWnd::kEventBuildList) {
    kind = 4;
  }
  if (kind < 3) {
    vfx_pane_wipe(screen, pane_, 0);
  }
  // The pictures' frame: min(20, value ** 0.75) for research and industry,
  // min(20, prosperity).
  const auto curve = [](std::int16_t value) {
    const std::int32_t v = game::chop(std::pow(static_cast<double>(static_cast<std::uint16_t>(value)), 0.75));
    return std::min<std::uint16_t>(static_cast<std::uint16_t>(v), 20);
  };
  switch (kind) {
    case 0:
      vfx_shape_draw(screen, pane_, wm_.shared_shapes(kResearchPanel).at(curve(p.research)), 0, 0);
      break;
    case 1:
      vfx_shape_draw(screen, pane_, wm_.shared_shapes(kResearchPanel + 1).at(curve(p.industry)), 0, 0);
      break;
    case 2:
      vfx_shape_draw(screen, pane_,
                     wm_.shared_shapes(kResearchPanel + 2)
                         .at(std::min<std::uint16_t>(static_cast<std::uint16_t>(p.prosperity), 20)),
                     0, 0);
      break;
    case 3: {
      vfx_pane_wipe(screen, pane_, hot ? kHotColor : kBackColor);
      const ShapeTable& np = wm_.shared_shapes(kScreenShapes);
      int frame = kPipFirst;
      int x = 0;
      int y = 0;
      for (int i = 0; i < p.max_population; ++i) {
        if (i == p.used_population) {
          ++frame;
        }
        if (i == p.population) {
          ++frame;
        }
        vfx_shape_draw(screen, pane_, np.at(static_cast<std::size_t>(frame)), x, y);
        x += 10;
        if (i % 10 == 9) {
          x = 0;
          y += 10;
        }
      }
      break;
    }
    case 4: {
      vfx_pane_wipe(screen, pane_, hot && p.owner == s->player_race ? kHotColor : kBackColor);
      std::string line;
      std::string title = wm_.static_string(kStringNoProject);
      std::uint8_t item = PlanWnd::kNoSelection;
      std::uint16_t sq = PlanWnd::kNoSquare;
      if (plan->selection() != PlanWnd::kNoSelection) {
        line = wm_.static_string(kStringPickLocation);
        item = plan->selection();
      } else if (p.project_item != game::kNoItem) {
        const std::int32_t eta = game::planet_project_eta(*s, p);
        item = p.project_item;
        if ((s->tables->planitems.items[item].flags & game::kItemContinuous) == 0) {
          line = static_cast<std::int16_t>(eta) == -1
                     ? wm_.static_string(kStringNoProgress)
                     : c_format(wm_.static_string(kStringDaysUntil), eta,
                                wm_.static_string(eta == 1 ? kStringNone : kStringPlural).c_str());
        }
        if (p.project_item != game::kItemAutomation) {
          sq = p.project_square;
        }
      }
      GameFont& font = wm_.large_font();
      if (item == PlanWnd::kNoSelection) {
        if (p.population == p.used_population) {
          font.pane() = pane_;
          font.draw_wrapped(screen, 0, 0x19, wm_.static_string(kStringNoFreePopulation), kFontCenterH, kDefault, 0xff,
                            0);
        }
      } else {
        const PlanShape ps = sq == PlanWnd::kNoSquare ? plan->item_shape(item) : plan->square_shape(sq);
        title = s->tables->planitems.items[item].name;
        const Shape& shape = wm_.shared_shapes(ps.table).at(static_cast<std::size_t>(ps.frame));
        const PolyPoint at = center_shape(pane_, shape);
        const std::int32_t scale = item == game::kItemShip ? 0x8000 : kScaleOne;
        vfx_shape_transform_draw(screen, pane_, shape, at.x, at.y, scale, scale);
        font.pane() = pane_;
        font.draw_wrapped(screen, 0, 0x50, line, kFontCenterH, kDefault, 0xff, 0);
      }
      font.pane() = pane_;
      font.draw_wrapped(screen, 0, 10, title, kFontCenterH, kDefault, 0xff, 0);
      if (bell_shown()) {
        const bool on = s->game->planet_management.at(static_cast<std::size_t>(s->game->cosmos.selected_planet)).bell != 0;
        draw_bell(screen, pane_, kBellX, kBellY, on ? kBellColor : kBellOffColor);
      }
      break;
    }
    default:
      break;
  }
  if (kind < 3 && hot && p.owner == s->player_race) {
    // draw_rect_frame (0x2b294), three nested frames.
    const int w = pane_.x1 - pane_.x0;
    const int h = pane_.y1 - pane_.y0;
    for (int i = 0; i < 3; ++i) {
      vfx_line_draw(screen, pane_, i, i, w - i, i, kHotColor);
      vfx_line_draw(screen, pane_, i, h - i, w - i, h - i, kHotColor);
      vfx_line_draw(screen, pane_, i, i, i, h - i, kHotColor);
      vfx_line_draw(screen, pane_, w - i, i, w - i, h - i, kHotColor);
    }
  }
  if (greyed()) {
    grey_out(kind < 3 ? 0 : kBackColor);
  }
}

// ---------------------------------------------------------------------------
// PlSquareWnd

bool PlSquareWnd::handle_event(int type, int a, int b) {
  switch (type) {
    case kEventActivate:
      active_ = true;
      shown_ = true;
      layout_buttons();
      draw(kDrawNormal);
      return false;
    case kEventDeactivate:
      broadcast_to_children(kEventDeactivate, a, b);
      active_ = false;
      shown_ = false;
      wm_.release_hot(id_);
      wm_.cancel_notify(id_, -1);
      wm_.kill_timer(id_, -1);
      return false;
    case kEventMouseLeave:
      if (active_ && shown_ && mouse_focus_) {
        draw(kDrawNormal);
      }
      return true;
    case kEventMouseEnter:
      if (active_ && shown_ && mouse_focus_) {
        draw_hilite(kDrawNormal);
      }
      return true;
    case kEventLeftClick:
    case kEventRightClick:
      // Swallowed wherever they are (0x3876c): while the dialog is up, a
      // click outside it reaches no window under it.
      return true;
    case PlanWnd::kEventDialog:
      mode_ = a;
      square_ = static_cast<std::uint16_t>(b);
      return false;
    default:
      if (type >= kCancel && type <= kAutomate) {
        command(type);
        return false;
      }
      return GWindow::handle_event(type, a, b);
  }
}

void PlSquareWnd::layout_buttons() {
  game::Session* s = wm_.services().game.core();
  if (s == nullptr) {
    return;
  }
  const game::Planet& p = s->game->cosmos.planets.at(static_cast<std::size_t>(s->game->cosmos.selected_planet));
  commands_.clear();
  if (mode_ == kSquare) {
    const std::span<const game::Square> squares = game::planet_squares(s->game->cosmos, p);
    const std::uint8_t item = square_ < 0xffff ? squares[square_].item : game::kNoItem;
    if (item == game::kNoItem) {
      for (std::uint8_t i = 0; i < game::PlanItemTable::kItems; ++i) {
        if (i != game::kItemColonyBase && game::planet_can_place_item(*s, p, i, square_)) {
          commands_.push_back(kBuild);
          break;
        }
      }
    } else {
      if (item == game::kItemShip && square_built(squares[square_])) {
        const game::Ship& ship =
            s->game->cosmos.ships.at(static_cast<std::size_t>(game::planet_square_ship(*s, p, square_)));
        if (game::ship_find_gizmo(ship, game::kGizmoColonizer) != -1 && p.owner == game::kNoOwner) {
          commands_.push_back(kCreateColony);
        }
        if (game::ship_find_gizmo(ship, game::kGizmoInvasionModule) != -1 && p.owner != game::kNoOwner &&
            p.owner != s->player_race) {
          commands_.push_back(kInvade);
        }
        commands_.push_back(kLeaveOrbit);
        const std::uint16_t docks = game::planet_find_item(s->game->cosmos, p, kOrbitalDocks);
        if (docks < 0xffff && square_built(squares[docks]) && p.owner == s->player_race) {
          commands_.push_back(kRefit);
        }
      }
      if (item != game::kItemColonyBase) {
        commands_.push_back(kAbandon);
      }
      if (game::planet_can_place_item(*s, p, game::kItemAutomation, square_)) {
        commands_.push_back(kAutomate);
      }
    }
  }
  commands_.push_back(kCancel);
  // Side by side between x 160 and 479, equal widths.
  int x = 0xa0;
  int remaining = static_cast<int>(commands_.size());
  for (std::size_t k = 0; k < commands_.size(); ++k, --remaining) {
    GWindow* button = children_.at(k);
    button->set_name(wm_.static_string(kStringButtons + commands_[k] - kCancel));
    button->set_message(commands_[k], 0, 0);
    const int x0 = x;
    x += (0x1df - x) / remaining;
    x = std::min(x, 0x1df);
    button->set_pane(Pane{x0, 0x151, x, 0x16f});
    ++x;
    button->handle_event(kEventActivate, 0, 0);
  }
  if (commands_.size() == 1) {
    children_.at(0)->set_name(wm_.static_string(kStringOk));
  }
}

void PlSquareWnd::command(int id) {
  game::Session& s = *wm_.services().game.core();
  game::Planet& p = s.game->cosmos.planets.at(static_cast<std::size_t>(s.game->cosmos.selected_planet));
  GWindow& plan = *wm_.window(parent_);
  switch (id) {
    case kCreateColony:
      plan.handle_event(PlanWnd::kEventDialog, 12, 0);
      return;
    case kAbandon:
      game::planet_abandon_square(s, p, square_);
      break;
    case kBuild:
      plan.handle_event(PlanWnd::kEventDialog, 11, 0);
      return;
    case kLeaveOrbit: {
      game::Ship& ship = s.game->cosmos.ships.at(static_cast<std::size_t>(game::planet_square_ship(s, p, square_)));
      game::planet_undock_ship(s, p, ship);
      break;
    }
    case kRefit:
      plan.handle_event(PlanWnd::kEventDialog, 10, 0);
      plan.handle_event(PlanWnd::kEventShipDesign, square_, 1);
      return;
    case kInvade:
      invade();
      return;
    case kAutomate:
      game::planet_set_square_item(s, p, square_, game::kItemAutomation, game::SquareMode::kProject);
      break;
    default:
      break;
  }
  plan.handle_event(PlanWnd::kEventDialog, 10, 0);
}

void PlSquareWnd::invade() {
  game::Session& s = *wm_.services().game.core();
  game::Planet& p = s.game->cosmos.planets.at(static_cast<std::size_t>(s.game->cosmos.selected_planet));
  std::int32_t used = 0;
  const bool taken = game::planet_invade(s, p, square_, used);
  auto& help = dynamic_cast<HelpWnd&>(*wm_.find_window("HELPWINDOW"));
  const std::int32_t colonies = game::race_count_planets(s.game->cosmos, s.game->cosmos.races.at(s.player_race));
  help.open_topic("help.txt", taken ? "invadesuccess" : "invadefailure");
  HelpTopic& topic = help.content();
  topic.elements[0].text =
      c_format(topic.elements[0].text, name_of(p.name).c_str(), used,
               wm_.static_string(used == 1 ? kStringNone : kStringPlural).c_str(), colonies,
               wm_.static_string(colonies == 1 ? kStringColonySingular : kStringColonyPlural).c_str());
  topic.elements[1].shape_index = kGizmoShapes;
  topic.elements[1].frame = game::kGizmoInvasionModule;
  topic.elements[2].shape_index = kPlanetShapes;
  topic.elements[2].frame = p.type * 5 + p.size;
  wm_.window(parent_)->handle_event(PlanWnd::kEventDialog, 10, 0);
  wm_.post_message(kMessageHelp, 0, kHelpTopic);
}

void PlSquareWnd::draw(int /*mode*/) {
  game::Session* s = wm_.services().game.core();
  if (s == nullptr || s->game->cosmos.selected_planet < 0) {
    return;
  }
  const game::Planet& p = s->game->cosmos.planets.at(static_cast<std::size_t>(s->game->cosmos.selected_planet));
  const game::Cosmos& cosmos = s->game->cosmos;
  Image8& screen = wm_.screen();
  vfx_pane_wipe(screen, pane_, kBackColor);
  vfx_shape_draw(screen, pane_, wm_.shared_shapes(kHelpShapes).at(0), 0, 0);
  GameFont& font = wm_.large_font();
  font.pane() = Pane{pane_.x0 + 10, pane_.y0 + 10, pane_.x0 + 0x145, pane_.y0 + 0xcf};
  font.set_background(0xff);
  const auto line = [&](int y, const std::string& text, int color = kDefault) {
    font.draw_wrapped(screen, 0, y, text, kFontCenterH, color, kDefault, 0);
  };
  // The output modes: "<planet> <output>: N per day", then the project /
  // growth line.
  std::string label;
  std::int32_t value = 0;
  bool second = false;
  std::string topic;
  std::string verb;
  std::string stalled;
  std::int32_t days = 0xffff;
  switch (mode_) {
    case kResearch: {
      label = wm_.static_string(kStringResearch);
      value = static_cast<std::uint16_t>(p.research);
      const std::int16_t project = s->game->research.project.at(p.owner);
      if (static_cast<std::uint16_t>(project) < 0xffff) {
        second = true;
        const game::ResearchItem& item = s->game->research.items.at(static_cast<std::size_t>(project));
        topic = name_of(item.name);
        days = game::research_days_left(item.cost, s->game->research.progress.at(p.owner),
                                        game::research_race_rate(*s, p.owner));
      }
      verb = wm_.static_string(kStringBeDiscovered);
      stalled = wm_.static_string(kStringNoProgressOn);
      break;
    }
    case kIndustry: {
      label = wm_.static_string(kStringIndustry);
      value = static_cast<std::uint16_t>(p.industry);
      const std::uint16_t sq = p.project_square;
      const std::uint8_t item = sq < 0xffff ? game::planet_squares(cosmos, p)[sq].item : game::kNoItem;
      if (item != game::kNoItem) {
        topic = s->tables->planitems.items[item].name;
        second = true;
        days = game::research_days_left(
            static_cast<std::int32_t>(game::planet_project_cost(*s, p, sq, game::kNoItem) & 0xffff), p.progress,
            static_cast<std::uint16_t>(p.industry));
      }
      verb = wm_.static_string(kStringBeCompleted);
      stalled = wm_.static_string(kStringNoProgressOn);
      break;
    }
    case kProsperity:
      label = wm_.static_string(kStringProsperity);
      value = static_cast<std::uint16_t>(p.prosperity);
      topic = wm_.static_string(kStringPopulation);
      verb = wm_.static_string(kStringGrow);
      second = true;
      if (p.population < p.max_population) {
        days = game::research_days_left(s->tables->planitems.pop_growth, p.growth,
                                        static_cast<std::uint16_t>(p.prosperity));
        if ((days & 0xffff) == 0xffff) {
          stalled = wm_.static_string(kStringNotGrowing);
        }
      } else {
        stalled = wm_.static_string(kStringNoRoom);
      }
      break;
    case kSquare:
      draw_square(*s, p, font);
      break;
    case kPlanet: {
      line(0xf, name_of(p.name));
      line(0x3c, c_format(wm_.static_string(kStringPlanetKind), wm_.static_string(kStringSizes + p.size).c_str(),
                          wm_.static_string(kStringTypes + p.type).c_str()));
      std::string owner = wm_.static_string(kStringNoOne);
      // An unowned planet reads "race 0xff"'s colour past the races: the
      // high byte of planet 220's free orbital count (the original's
      // memory layout), in practice 0.
      std::uint8_t owner_color = 0;
      if (p.owner < 0xff) {
        owner = wm_.static_string(kStringSpecies + cosmos.races.at(p.owner).species);
        owner_color = cosmos.races.at(p.owner).color;
      } else {
        owner_color = static_cast<std::uint8_t>(cosmos.planets.at(220).free_orbital >> 8);
      }
      line(0x5a, c_format(wm_.static_string(kStringOwnedBy), owner_color * 4 + 0x13, owner.c_str()));
      line(0x78, c_format(wm_.static_string(kStringMaxPopulation), static_cast<int>(p.max_population)));
      if (p.owner == s->player_race && p.self_managed == -1) {
        line(0x96, wm_.static_string(kStringSelfManaged), cosmos.races.at(s->player_race).color * 4 + 0x13);
      }
      break;
    }
    default:
      break;
  }
  if (!label.empty()) {
    line(0xf, c_format(wm_.static_string(kStringPerDay), name_of(p.name).c_str(), label.c_str(), value));
    if (second) {
      if ((days & 0xffff) == 0xffff) {
        line(0x50, c_format(stalled, topic.c_str()));
      } else {
        line(0x50, c_format(wm_.static_string(kStringWillIn), topic.c_str(), verb.c_str(), days & 0xffff,
                            wm_.static_string((days & 0xffff) == 1 ? kStringNone : kStringPlural).c_str()));
      }
    }
  }
  if (!children_.empty()) {
    draw_children();
  }
}

void PlSquareWnd::draw_square(const game::Session& s, const game::Planet& p, GameFont& font) {
  if (square_ >= 0xffff) {
    return;
  }
  Image8& screen = wm_.screen();
  const game::Square& sq = game::planet_squares(s.game->cosmos, p)[square_];
  std::string color = wm_.static_string(kStringWhite);
  std::string output;
  switch (sq.color & 7) {
    case 0:
      color = wm_.static_string(kStringBlack);
      break;
    case 2:
      color = wm_.static_string(kStringRed);
      output = wm_.static_string(kStringRedOutput);
      break;
    case 3:
      color = wm_.static_string(kStringGreen);
      output = wm_.static_string(kStringGreenOutput);
      break;
    case 4:
      color = wm_.static_string(kStringBlue);
      output = wm_.static_string(kStringBlueOutput);
      break;
    default:
      break;
  }
  const bool orbital = square_ >= p.surface_squares;
  if (orbital) {
    color = wm_.static_string(kStringOrbital);
  }
  if (sq.item == game::kNoItem) {
    const bool ruins = (sq.color & game::kSquareRuins) != 0;
    const std::string title = ruins && p.owner == s.player_race
                                  ? wm_.static_string(kStringRuins)
                                  : c_format(wm_.static_string(kStringSquare), color.c_str());
    font.draw_wrapped(screen, 0, 0x14, title, kFontCenterH, kDefault, kDefault, 0);
    if (!ruins) {
      std::string help;
      if (orbital) {
        help = wm_.static_string(kStringOrbitalHelp);
      } else if ((sq.color & 7) == 1) {
        help = wm_.static_string(kStringWhiteHelp);
      } else if ((sq.color & 7) == 0) {
        help = wm_.static_string(kStringBlackHelp);
      } else {
        help = c_format(wm_.static_string(kStringColorHelp), color.c_str(), output.c_str());
      }
      font.draw_wrapped(screen, 10, 0x50, help, kFontCenterH, kDefault, kDefault, 0);
    }
    return;
  }
  std::string name = s.tables->planitems.items[sq.item].name;
  if (sq.item == game::kItemShip) {
    const game::Ship& ship = s.game->cosmos.ships.at(static_cast<std::size_t>(game::planet_square_ship(s, p, square_)));
    name = c_format("%s \"%s\"", name.c_str(), name_of(ship.name).c_str());
  }
  const std::string automated = (sq.flags & game::kSquareAutomated) != 0 ? wm_.static_string(kStringAutomated) : "";
  const std::string on = orbital ? "" : c_format(wm_.static_string(kStringOnSquare), color.c_str());
  font.draw_wrapped(screen, 0, 0xf, automated + name + on, kFontCenterH, kDefault, kDefault, 0);
  if (sq.flags == 0) {
    const std::int32_t days = game::research_days_left(
        static_cast<std::int32_t>(game::planet_project_cost(s, p, square_, game::kNoItem) & 0xffff), p.progress,
        static_cast<std::uint16_t>(p.industry));
    const std::string text = days == 0xffff
                                 ? wm_.static_string(kStringNoProgressSquare)
                                 : c_format(wm_.static_string(kStringSquareDays), days,
                                            wm_.static_string(days == 1 ? kStringNone : kStringPlural).c_str());
    font.draw_wrapped(screen, 0, 0x32, text, kFontCenterH, kDefault, kDefault, 0);
  }
}

void add_planet_windows(WindowFactory& factory) {
  factory.add(7, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<PlanWnd>(wm, record, shapes);
  });
  factory.add(14, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<PlSquareWnd>(wm, record, shapes);
  });
  factory.add(29, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<PlanStatButton>(wm, record, shapes);
  });
}

}  // namespace oa
