#include "screens/ship_des_wnd.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/c_format.h"
#include "core/error.h"
#include "game/planet.h"
#include "game/research.h"
#include "game/session.h"
#include "game/ship.h"
#include "game/ship_design.h"
#include "game/tables.h"
#include "gfx/gsystem.h"

namespace oa {

namespace {

// The static strings of the screen.
constexpr int kStringNoEnding = 0x1c;  // ""
constexpr int kStringPlural = 0x1d;    // "s"
constexpr int kStringDays = 0x87;      // " (%d Day%s)"
constexpr int kStringSpecies = 101;    // the species names, 101..121

// `gwshare.txt` tables.
constexpr int kDockShapes = 21;  // dkship0N.shp of race slot N, frame = hull
constexpr int kGizmoShapes = 28;
constexpr int kRaceFlags = 32;
constexpr int kShipIcons = 35;
// The window's own shapes (`8SHIPDES.SHP`): the empty and hovered slots.
constexpr int kEmptySlot = 0;
constexpr int kHoveredSlot = 1;
// `shipicon.shp`: the strength icons, one frame per bar from here.
constexpr int kFirstStatIcon = 7;

// The panes the drawing wipes or draws in.
constexpr Pane kGridRight{0x14a, 0x72, 0x1dc, 0x1d8};
constexpr Pane kGridBottom{7, 0x159, 0x149, 0x1d8};
constexpr Pane kSelectedPane{0x13b, 7, 0x1dc, 0x69};
constexpr Pane kDockPane{7, 0x72, 0x141, 0x150};
constexpr Pane kInfoPane{7, 7, 0x130, 0x66};

constexpr std::uint8_t kSelectedBack = 0xf2;
constexpr std::uint8_t kListBarColor = 0xf2;
constexpr int kListLineHeight = 0x40;

// The strength bars: 15 icons 14 pixels apart, one per 4 points, the bar
// clipped after the value.
constexpr int kBarIcons = 15;
constexpr int kBarStep = 14;

int race_text_color(int color) { return color * 4 + 0x13; }

}  // namespace

game::Session* ShipDesWnd::session() const { return wm_.services().game.core(); }

void ShipDesWnd::open(game::Ship* ship, Mode mode, bool read_only) {
  ship_ = ship;
  mode_ = mode;
  if (mode == kRefit) {
    original_ = *ship;
  }
  read_only_ = read_only;
}

std::int32_t ShipDesWnd::refit_cost(const game::Session& session) const {
  if (mode_ != kRefit || ship_ == nullptr) {
    return kNoRefitCost;
  }
  return game::ship_design_refit_cost(session, *ship_, original_);
}

void ShipDesWnd::fill_gizmo_list() {
  list_->clear();
  const game::Session* s = session();
  if (s == nullptr) {
    return;
  }
  for (const std::int8_t gizmo :
       game::ship_design_gizmo_list(*s, static_cast<std::uint8_t>(category_), s->player_race)) {
    const int row = list_->add_item({}, -1);
    list_->set_item_data(row, gizmo);
  }
}

void ShipDesWnd::select_category(int category) {
  if (category < kWeapon || category > kSpecial || category == category_) {
    return;
  }
  category_ = category;
  fill_gizmo_list();
  selected_ = -1;
  if (list_->count() != 0) {
    selected_ = static_cast<std::int8_t>(list_->item(0).data);
  }
  draw(kDrawNormal);
}

bool ShipDesWnd::edit_slot(bool install) {
  if (hovered_ < 0) {
    return false;
  }
  game::Session* s = session();
  if (ship_ == nullptr || s == nullptr) {
    throw std::logic_error("ShipDesWnd: no ship");
  }
  return install ? game::ship_install_gizmo(*s, *ship_, selected_, hovered_)
                 : game::ship_remove_gizmo(*s, *ship_, hovered_);
}

LongPress ShipDesWnd::long_press(int x, int y) const {
  if (read_only_ || !active_ || ship_ == nullptr) {
    return LongPress::kHelp;
  }
  const int slot = game::ship_design_slot_at(x, y, ship_->hull_size);
  return slot >= 0 && ship_->slots.at(static_cast<std::size_t>(slot)).gizmo != -1 ? LongPress::kRight
                                                                                  : LongPress::kHelp;
}

bool ShipDesWnd::handle_event(int type, int a, int b) {
  switch (type) {
    case kEventActivate:
      if (ship_ == nullptr) {
        ship_ = &template_;
      }
      redraw_dock_ = true;
      modified_ = false;
      list_ = dynamic_cast<ListBox*>(wm_.find_window("GIZLIST"));
      if (list_ == nullptr) {
        throw DataError("windows.txt: no GIZLIST list box");
      }
      list_->set_draw_item(
          [this](const Pane& row, const ListBox::Item& item, bool hilite) { draw_gizmo_row(row, item.data, hilite); });
      list_->set_line_height(kListLineHeight);
      list_->set_style(0);
      list_->set_colors(0x31, GWindow::kBackColor, kListBarColor);
      if (selected_ == -1) {
        select_category(kGenerator);
      } else {
        fill_gizmo_list();
      }
      redraw_info_ = true;
      shown_ = true;
      active_ = true;
      draw(kDrawNormal);
      broadcast_to_children(kEventActivate, a, b);
      return false;
    case kEventDeactivate:
      shown_ = false;
      active_ = false;
      hovered_ = -1;
      if (ship_ != &template_ && ship_ != nullptr) {
        template_ = *ship_;
      }
      broadcast_to_children(type, a, b);
      // The original leaves the mouse-move notification to the window
      // manager's screen change.
      wm_.cancel_notify(id_, -1);
      if (!modified_) {
        ship_ = nullptr;
      }
      return false;
    case kEventLeftClick:
    case kEventRightClick:
      if (!active_ || !contains(a, b)) {
        return false;
      }
      if (!read_only_ && edit_slot(type == kEventLeftClick)) {
        if (sound_ != -1) {
          wm_.services().sound.play_sfx(sound_);
        }
        redraw_info_ = true;
        modified_ = true;
        draw(kDrawNormal);
      }
      return true;
    case kEventMouseLeave:
      if (read_only_) {
        return true;
      }
      wm_.cancel_notify(id_, kEventMouseMove);
      if (!shown_ || !active_ || (hovered_ < 0 && !mouse_focus_)) {
        hovered_ = -1;
      } else {
        hovered_ = -1;
        draw(kDrawNormal);
      }
      return true;
    case kEventMouseEnter:
      if (read_only_) {
        return true;
      }
      wm_.request_notify(id_, WinMgr::kNotifyMove, kEventMouseMove, 0, 0);
      if (shown_ && active_ && mouse_focus_) {
        draw_hilite(kDrawNormal);
      }
      return true;
    case kEventMouseMove:
      if (!read_only_) {
        if (ship_ == nullptr) {
          throw std::logic_error("ShipDesWnd: no ship");
        }
        hovered_ = game::ship_design_slot_at(a, b, ship_->hull_size);
        draw(kDrawNormal);
      }
      return false;
    case kMessageHull:
      if (!read_only_) {
        game::Session* s = session();
        if (mode_ == kNewDesign && s != nullptr && game::ship_design_cycle_hull(*s, *ship_, s->player_race)) {
          redraw_info_ = true;
          redraw_dock_ = true;
        }
        draw(kDrawNormal);
      }
      return false;
    case kMessageWeapons:
      select_category(kWeapon);
      return false;
    case kMessageDefenses:
      select_category(kShield);
      return false;
    case kMessageSensors:
      select_category(kScanner);
      return false;
    case kMessageEngines:
      select_category(kDrive);
      return false;
    case kMessageEnergy:
      select_category(kGenerator);
      return false;
    case kMessageSpecial:
      select_category(kSpecial);
      return false;
    case kEventListPick: {
      const auto gizmo = static_cast<std::int8_t>(list_->item(a).data);
      if (gizmo != selected_) {
        selected_ = gizmo;
        draw(kDrawNormal);
      }
      return false;
    }
    case kEventListHelp:
      // The list's help message (a row clicked in help mode): the
      // component's `gizhelp.txt` topic.
      wm_.post_message(kMessageHelp, list_->item(a).data, kHelpGizmo);
      return true;
    default:
      return GWindow::handle_event(type, a, b);
  }
}

void ShipDesWnd::draw(int /*mode*/) {
  if (ship_ == nullptr) {
    throw std::logic_error("ShipDesWnd: drawn without a ship");
  }
  const game::Session* s = session();
  if (s == nullptr) {
    return;
  }
  // The remake greys HULL when it would do nothing.
  if (GWindow* hull = wm_.find_window("HULL")) {
    hull->set_greyed(read_only_ || mode_ != kNewDesign ||
                     !game::ship_design_can_cycle_hull(*s, *ship_, s->player_race));
  }
  Image8& screen = wm_.screen();
  vfx_pane_wipe(screen, kGridRight, 0);
  vfx_pane_wipe(screen, kGridBottom, 0);
  draw_grid(*ship_);

  const ShapeTable& gizmos = wm_.shared_shapes(kGizmoShapes);
  if (selected_ != -1) {
    vfx_pane_wipe(screen, kSelectedPane, kSelectedBack);
    vfx_shape_draw(screen, kSelectedPane, gizmos.at(static_cast<std::size_t>(selected_)),
                   (kSelectedPane.x1 - kSelectedPane.x0) >> 1, 0x3a);
    GameFont& small = wm_.small_font();
    small.pane() = kSelectedPane;
    small.draw_wrapped(screen, 3, 3, s->tables->gizmos.at(static_cast<std::size_t>(selected_)).name, 0x42, kTextColor,
                       0xff, 0);
  }
  if (redraw_dock_) {
    vfx_pane_wipe(screen, kDockPane, 0);
    vfx_shape_draw(screen, kDockPane, wm_.shared_shapes(kDockShapes + s->player_race).at(ship_->hull_size), 0, 0);
    redraw_dock_ = false;
  }
  if (redraw_info_) {
    draw_info(*s, *ship_);
    redraw_info_ = false;
  }
  draw_children();
}

void ShipDesWnd::draw_grid(const game::Ship& ship) {
  Image8& screen = wm_.screen();
  const ShapeTable& gizmos = wm_.shared_shapes(kGizmoShapes);
  const std::span<const game::DesignCell> cells = game::ship_design_cells(ship.hull_size);
  for (std::size_t i = 0; i < cells.size(); ++i) {
    const Shape* shape = nullptr;
    if (hovered_ >= 0 && static_cast<std::size_t>(hovered_) == i) {
      shape = selected_ == -1 ? this->shape(kHoveredSlot) : &gizmos.at(static_cast<std::size_t>(selected_));
    } else {
      const std::int8_t gizmo = ship.slots.at(i).gizmo;
      if (gizmo == -1) {
        shape = this->shape(kEmptySlot);
      } else if (static_cast<std::size_t>(gizmo) >= game::kGizmoCount) {
        throw std::logic_error("ShipDesWnd: bad gizmo in a slot");
      } else {
        shape = &gizmos.at(static_cast<std::size_t>(gizmo));
      }
    }
    if (shape != nullptr) {
      const game::DesignPoint p = game::design_cell_point(cells[i]);
      vfx_shape_draw(screen, GSystem::kScreenPane, *shape, p.x, p.y);
    }
  }
}

void ShipDesWnd::draw_info(const game::Session& s, const game::Ship& ship) {
  Image8& screen = wm_.screen();
  const game::Cosmos& cosmos = s.game->cosmos;
  const game::Race& race = cosmos.races.at(s.player_race);
  vfx_pane_wipe(screen, kInfoPane, 0);
  wm_.draw_colored(kInfoPane, wm_.shared_shapes(kRaceFlags).at(race.species), 3, 3, race.color);

  // The build days, once the design was changed.
  std::string days_text;
  if (&ship != &template_ && modified_ && cosmos.selected_planet != game::kNone) {
    const game::Planet& planet = cosmos.planets.at(static_cast<std::size_t>(cosmos.selected_planet));
    const std::int32_t cost = mode_ == kRefit ? refit_cost(s) : game::ship_industry_cost(s, ship);
    const std::int32_t days = game::planet_build_days(planet, cost);
    if (days != 0xffff) {
      days_text = c_format(wm_.static_string(kStringDays), static_cast<int>(days),
                           wm_.static_string(days == 1 ? kStringNoEnding : kStringPlural).c_str());
    }
  }
  const std::string title = c_format("%s \"%s\"%s", wm_.static_string(kStringSpecies + race.species).c_str(),
                                     std::string(game::name_view(ship.name)).c_str(), days_text.c_str());
  GameFont& large = wm_.large_font();
  large.pane() = kInfoPane;
  large.draw_wrapped(screen, 0, 3, title, kFontCenterH, race_text_color(race.color), kDefault, 0);

  // The strength bars: weapons, shields, battle speed, scanners, power.
  const ShapeTable& icons = wm_.shared_shapes(kShipIcons);
  const std::array<std::int32_t, 5> stats{ship.weapon_damage, ship.shield_max, ship.drive_speed, ship.scanner_level,
                                          ship.generator_power};
  for (std::size_t r = 0; r < stats.size(); ++r) {
    Pane clip = kInfoPane;
    clip.x1 = clip.x0 + 0x27;
    if (stats[r] > 0) {
      clip.x1 += stats[r] * kBarStep / 4;
    }
    const Shape& icon = icons.at(kFirstStatIcon + r);
    for (int i = 0; i < kBarIcons; ++i) {
      vfx_shape_draw(screen, clip, icon, 0x28 + i * kBarStep, 0x14 + static_cast<int>(r) * kBarStep);
    }
  }
}

void ShipDesWnd::draw_gizmo_row(const Pane& row, int gizmo, bool hilite) {
  const game::Session* s = session();
  if (s == nullptr) {
    return;
  }
  Image8& screen = wm_.screen();
  if (hilite) {
    vfx_pane_wipe(screen, row, GWindow::kHotBackColor);
  }
  vfx_shape_draw(screen, row, wm_.shared_shapes(kGizmoShapes).at(static_cast<std::size_t>(gizmo)), 0x3f, 0x2d);
  GameFont& small = wm_.small_font();
  small.pane() = row;
  small.draw_wrapped(screen, 0x3f, 2, s->tables->gizmos.at(static_cast<std::size_t>(gizmo)).name, kFontAboveH, kDefault,
                     0xff, 0x7a);
}

void add_ship_design_windows(WindowFactory& factory) {
  factory.add(8, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<ShipDesWnd>(wm, record, shapes);
  });
}

}  // namespace oa
