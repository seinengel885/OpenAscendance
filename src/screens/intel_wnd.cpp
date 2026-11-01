#include "screens/intel_wnd.h"

#include <format>
#include <memory>
#include <string>

#include "core/c_format.h"
#include "core/error.h"
#include "game/race.h"
#include "game/session.h"
#include "gfx/vfx_poly.h"

namespace oa {

namespace {

// The static strings.
constexpr int kStringFleet = 0x14;
constexpr int kStringColonies = 0x15;
constexpr int kStringDiscoveries = 0x16;
constexpr int kStringTitle = 0x1b;     // "%s Intelligence"
constexpr int kStringNoEnding = 0x1c;  // ""
constexpr int kStringPlural = 0x1d;    // "s"
constexpr int kStringOk = 0x1e;        // "OK"
constexpr int kStringSpecies = 101;    // the species names, 101..121

// `gwshare.txt` tables.
constexpr int kRaceFlags = 32;
constexpr int kRaceRings = 33;
constexpr int kIntelShapes = 41;
// 14intel.shp frames.
constexpr int kRing = 3;
constexpr int kFleetPip = 5;
constexpr int kColonyPip = 6;
constexpr int kDiscoveryPip = 7;
constexpr int kWarMark = 8;
constexpr int kAllianceMark = 9;

// The row's layout (`IntelWnd_draw_race_row`).
constexpr int kNameX = 0x14;
constexpr int kTextY = 5;
constexpr int kPortraitX = 0x96;
constexpr int kPortraitY = 0x1e;
constexpr std::int32_t kHalfScale = 0x8000;
constexpr int kFleetX = 200;
constexpr int kColoniesX = 0x118;
constexpr int kDiscoveriesX = 0x168;
constexpr int kPipY = 0x14;
constexpr int kPipStep = 8;
constexpr int kPipsPerRow = 9;
constexpr int kRelationsX = 0x1cc;
constexpr int kRelationsMaxX = 600;
constexpr int kRelationStepX = 0x39;
constexpr int kRelationStepY = 0x1b;
// The title (`IntelWnd_draw`).
constexpr int kFlagPos = 3;
constexpr int kTitleY = 9;

// The countdown line of the ability topics (`+0xccb`, element 2).
constexpr std::size_t kCountdownElement = 2;

int race_text_color(int color) { return color * 4 + 0x13; }

}  // namespace

game::RaceIndex IntelWnd::row_race(int row) const {
  // Each row's text holds its race slot (the original's row is the race
  // record, whose first byte is the slot).
  return static_cast<game::RaceIndex>(list_->item(row).text.at(0));
}

bool IntelWnd::handle_event(int type, int a, int b) {
  if (type == kEventActivate && list_ == nullptr) {
    list_ = dynamic_cast<ListBox*>(wm_.find_window("INTELLIST"));
    if (list_ == nullptr) {
      throw DataError("windows.txt: no INTELLIST list box");
    }
    list_->set_line_height(kRowHeight);
    // The rows' press picks nothing here: the mouse may drag the list.
    list_->set_mouse_drag(true);
    list_->set_draw_item([this](const Pane& row, const ListBox::Item& item, bool /*hilite*/) {
      draw_row(row, static_cast<game::RaceIndex>(item.text.at(0)));
    });
  }
  return GWindow::handle_event(type, a, b);
}

void IntelWnd::draw(int /*mode*/) {
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr || list_ == nullptr) {
    return;
  }
  const game::Cosmos& cosmos = core->game->cosmos;
  const game::Race& player = cosmos.races[core->player_race];
  wm_.draw_colored(pane_, wm_.shared_shapes(kRaceFlags).at(player.species), kFlagPos, kFlagPos, player.color);
  GameFont& large = wm_.large_font();
  large.pane() = pane_;
  large.draw_wrapped(
      wm_.screen(), 0, kTitleY,
      c_format(wm_.static_string(kStringTitle), wm_.static_string(kStringSpecies + player.species).c_str()),
      kFontCenterH, race_text_color(player.color), 0xff, 0);
  list_->clear();
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    const game::Race& race = cosmos.races[static_cast<std::size_t>(r)];
    if (r == core->player_race ||
        (player.relation[static_cast<std::size_t>(r)] != game::kNotMet && race.extinct == 0)) {
      const std::int32_t value = game::race_fleet_strength(cosmos, race) + game::race_count_planets(cosmos, race);
      const int row = list_->add_item(std::string(1, static_cast<char>(r)));
      list_->set_item_data(row, -value);
    }
  }
  // `ListItem_compare_value` (0x10a14): ascending data.
  list_->sort([](const ListBox::Item& a, const ListBox::Item& b) {
    return a.data > b.data ? 1 : a.data == b.data ? 0 : -1;
  });
  draw_children();
}

void IntelWnd::draw_row(const Pane& row, game::RaceIndex index) {
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return;
  }
  const game::Cosmos& cosmos = core->game->cosmos;
  const game::Race& race = cosmos.races.at(index);
  const game::Race& player = cosmos.races[core->player_race];
  Image8& screen = wm_.screen();
  const int color = race_text_color(race.color);
  const ShapeTable& intel = wm_.shared_shapes(kIntelShapes);
  wm_.draw_colored(row, wm_.shared_shapes(kRaceFlags).at(race.species), kFlagPos, kFlagPos, race.color);
  GameFont& large = wm_.large_font();
  large.pane() = row;
  large.draw_wrapped(screen, kNameX, kTextY, wm_.static_string(kStringSpecies + race.species), 0, color, 0xff, 0);
  vfx_shape_transform_draw(screen, row, wm_.shared_shapes(index).at(0), kPortraitX, kPortraitY, kHalfScale, kHalfScale);
  wm_.draw_colored(row, intel.at(kRing), kPortraitX, kPortraitY, race.color);
  // A label and its pips, 9 per row.
  GameFont& small = wm_.small_font();
  const auto pips = [&](int label, int x0, std::int32_t count, int frame) {
    small.pane() = row;
    small.draw_wrapped(screen, x0, kTextY, wm_.static_string(label), 0, color, 0xff, 0);
    int x = x0;
    int y = kPipY;
    for (std::int32_t i = 0; i < count; ++i) {
      wm_.draw_colored(row, intel.at(static_cast<std::size_t>(frame)), x, y, race.color);
      x += kPipStep;
      if (i % kPipsPerRow == kPipsPerRow - 1) {
        x = x0;
        y += kPipStep;
      }
    }
  };
  pips(kStringFleet, kFleetX, game::race_fleet_strength(cosmos, race), kFleetPip);
  pips(kStringColonies, kColoniesX, game::race_count_planets(cosmos, race), kColonyPip);
  pips(kStringDiscoveries, kDiscoveriesX, game::race_count_discoveries(core->game->research, race) >> 2, kDiscoveryPip);
  // The species this one has met that the player knows (or the player),
  // with its war or alliance mark.
  int x = kRelationsX;
  int y = kTextY;
  for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
    const auto u = static_cast<std::size_t>(r);
    const game::Race& other = cosmos.races[u];
    if (u == index || race.relation[u] == game::kNotMet || other.extinct != 0 ||
        (player.relation[u] == game::kNotMet && r != core->player_race)) {
      continue;
    }
    wm_.draw_colored(row, wm_.shared_shapes(kRaceFlags).at(other.species), x, y, other.color);
    const std::uint8_t relation = race.relation[u];
    if (relation == game::kAlliance || relation == game::kWar) {
      vfx_shape_draw(screen, row, intel.at(relation == game::kAlliance ? kAllianceMark : kWarMark), x, y);
    }
    x += kRelationStepX;
    if (x > kRelationsMaxX) {
      y += kRelationStepY;
      x = kRelationsX;
    }
  }
}

bool AbilityWnd::handle_event(int type, int a, int b) {
  game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return HelpWnd::handle_event(type, a, b);
  }
  game::Race& player = core->game->cosmos.races[core->player_race];
  if (type == kEventActivate) {
    if (game::race_use_special_ability(*core, player, false)) {
      if (static_cast<int>(kCountdownElement) < content_.element_count) {
        content_.elements[kCountdownElement].kind = HelpElement::Kind::kNone;
      }
    } else {
      if (children_.size() < 2) {
        throw DataError("windows.txt: ABILITYWIN lacks its two buttons");
      }
      wm_.send_event(children_[0]->id(), kEventDeactivate, 0, 0);
      const std::int32_t days = game::race_ability_period(player) - player.ability_charge;
      HelpElement& line = content_.elements.at(kCountdownElement);
      line.text = c_format(line.text, days, wm_.static_string(days == 1 ? kStringNoEnding : kStringPlural).c_str());
      children_[1]->set_name(wm_.static_string(kStringOk));
    }
    shown_ = true;
    active_ = true;
    draw(kDrawNormal);
    return false;
  }
  if (type == kEventHelpButton && a == 1) {
    game::race_use_special_ability(*core, player, true);
    open_topic("help.txt", std::format("abres{:02}", player.ability));
    draw(kDrawNormal);
    return false;
  }
  return HelpWnd::handle_event(type, a, b);
}

void AbilityWnd::draw(int mode) {
  HelpWnd::draw(mode);
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return;
  }
  Image8& screen = wm_.screen();
  vfx_shape_draw(screen, pane_, wm_.shared_shapes(core->player_race).at(0), kPortraitX, kPortraitY);
  vfx_shape_draw(screen, pane_, wm_.shared_shapes(kRaceRings).at(core->game->cosmos.races[core->player_race].color),
                 kPortraitX, kPortraitY);
}

void add_intel_windows(WindowFactory& factory) {
  factory.add(12, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<IntelWnd>(wm, record, shapes);
  });
  factory.add(22, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<AbilityWnd>(wm, record, shapes);
  });
}

}  // namespace oa
