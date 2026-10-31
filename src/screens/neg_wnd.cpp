#include "screens/neg_wnd.h"

#include <format>
#include <memory>

#include "core/error.h"
#include "game/session.h"
#include "game/state.h"
#include "gfx/vfx_poly.h"

namespace oa {

namespace {

constexpr int kScreenStarSystem = 16;

// The rows of PLAYERDIP and their text (`NegWnd_draw_option_row`).
constexpr int kRowHeight = 0x1d;
constexpr int kRowTextX = 8;
constexpr int kRowTextY = 3;
constexpr int kRowMargin = 0x10;
// The selected system box's texts.
constexpr int kStringSelectedSystem = 0x1a;
constexpr int kSystemLabelX = 2;
constexpr int kSystemLabelY = 2;
constexpr int kSystemNameX = 10;
constexpr int kSystemNameY = 0xd;
// The species names (static strings 101..121).
constexpr int kStringSpecies = 101;
constexpr int kSpeciesCount = 21;
// `gwshare.txt` tables: the 14intel rings (frame 3, 4 selected).
constexpr int kIntelShapes = 41;
constexpr int kRing = 3;
constexpr int kSelectedRing = 4;
constexpr std::int32_t kHalfScale = 0x8000;
// The hit test's half size around a portrait's centre.
constexpr int kPickRadius = 0x16;
constexpr int kClickSound = 0;

int race_text_color(int color) { return color * 4 + 0x13; }

// The sentences of the `.dip` file `file`, translated one by one.
game::DipSentences load_sentences(const GameFiles& files, const std::string& file) {
  game::DipSentences out = game::parse_dip_sentences(files.load(file));
  for (std::string& s : out.actions) {
    s = files.text(file, s);
  }
  for (std::string& s : out.responses) {
    s = files.text(file, s);
  }
  return out;
}

}  // namespace

void NegWnd::load_dialogue(int species) {
  const GameFiles& files = wm_.services().files;
  auto& alien = alien_sentences_.at(static_cast<std::size_t>(species));
  if (!alien) {
    alien = load_sentences(files, std::format("RACE{:02}.DIP", species));
  }
  if (!player_sentences_) {
    player_sentences_ = load_sentences(files, "PLAYER.DIP");
  }
}

void NegWnd::show_options() {
  const int top = list_->top();
  list_->clear();
  for (const game::Negotiation::Option& option : negotiation_->options()) {
    const int row = list_->add_item(option.text);
    list_->set_item_data(row, option.code);
  }
  if (negotiation_->keep_scroll()) {
    list_->scroll_to(top);
  }
  list_->draw(kDrawNormal);
}

void NegWnd::show_alien_text() {
  alien_->set_text(negotiation_->alien_text());
  alien_->draw(kDrawNormal);
}

void NegWnd::draw_option_row(const Pane& row, const ListBox::Item& item, bool hilite) {
  const game::Session* core = wm_.services().game.core();
  if (core == nullptr) {
    return;
  }
  const int color = hilite ? kTextColor : race_text_color(core->game->cosmos.races[core->player_race].color);
  GameFont& font = wm_.large_font();
  font.pane() = row;
  font.draw_wrapped(wm_.screen(), kRowTextX, kRowTextY, item.text, 0, color, 0xff, (row.x1 - row.x0) - kRowMargin);
}

void NegWnd::on_star_selected() {
  if (negotiation_) {
    negotiation_->star_selected();
    if (!negotiation_->replying()) {
      show_options();
    }
  }
  draw(kDrawNormal);
}

void NegWnd::press_areas(std::vector<PressArea>& out) const {
  out.push_back(PressArea{pane_, false});
  out.push_back(PressArea{kThirdPartyBox, false});
}

bool NegWnd::handle_event(int type, int a, int b) {
  const auto in_box = [&](int x, int y) {
    return x > kThirdPartyBox.x0 - 1 && x < kThirdPartyBox.x1 + 1 && y > kThirdPartyBox.y0 - 1 &&
           y < kThirdPartyBox.y1 + 1;
  };
  switch (type) {
    case kEventActivate: {
      game::Session* core = wm_.services().game.core();
      if (list_ == nullptr) {
        list_ = dynamic_cast<ListBox*>(wm_.find_window("PLAYERDIP"));
        alien_ = dynamic_cast<TextBox*>(wm_.find_window("ALIENDIP"));
        map_ = wm_.find_window("COSMOSWnd");
        if (list_ == nullptr || alien_ == nullptr || map_ == nullptr) {
          throw DataError("windows.txt: NEGSCREEN lacks PLAYERDIP, ALIENDIP or COSMOSWnd");
        }
        for (int s = 0; s < kSpeciesCount; ++s) {
          species_names_.push_back(wm_.static_string(kStringSpecies + s));
        }
      }
      list_->set_line_height(kRowHeight);
      list_->set_draw_item(
          [this](const Pane& row, const ListBox::Item& item, bool hilite) { draw_option_row(row, item, hilite); });
      list_->set_style(0);
      list_->set_colors(0x31, kBackColor, kBackColor);
      if (core != nullptr) {
        const auto target = static_cast<game::RaceIndex>(wm_.target_race());
        const game::Race& race = core->game->cosmos.races.at(target);
        load_dialogue(race.species);
        alien_->set_color(race_text_color(race.color));
        negotiation_.emplace(*core, species_names_);
        negotiation_->open(target, *alien_sentences_.at(race.species), *player_sentences_);
        show_options();
        show_alien_text();
      }
      return GWindow::handle_event(type, a, b);
    }
    case kEventLeftClick:
    case kEventRightClick: {
      if (!in_box(a, b)) {
        return false;
      }
      wm_.services().sound.play_sfx(kClickSound);
      if (!negotiation_) {
        return true;
      }
      const std::vector<game::RaceIndex>& thirds = negotiation_->third_parties();
      std::size_t i = 0;
      while (i < thirds.size() && !(static_cast<std::int16_t>(a - kThirdPartyX.at(i)) < kPickRadius &&
                                    static_cast<std::int16_t>(a - kThirdPartyX.at(i)) > -kPickRadius &&
                                    static_cast<std::int16_t>(b - kThirdPartyY.at(i)) < kPickRadius &&
                                    static_cast<std::int16_t>(b - kThirdPartyY.at(i)) > -kPickRadius)) {
        ++i;
      }
      if (negotiation_->pick_third_party(static_cast<int>(i))) {
        if (!negotiation_->replying()) {
          show_options();
        }
        draw(kDrawNormal);
      }
      return true;
    }
    case kEventHelpClick:
      if (in_box(a, b)) {
        wm_.post_message(kMessageHelp, 0, kHelpTopic, "tinyrace");
        return true;
      }
      return GWindow::handle_event(type, a, b);
    case kEventListPick: {
      if (!negotiation_ || a < 0 || a >= list_->count()) {
        return true;
      }
      const auto code = static_cast<std::uint8_t>(list_->item(a).data);
      switch (negotiation_->choose(code)) {
        case game::Negotiation::Outcome::kGoodbye:
          wm_.post_message(kMessageBack, 0, 0);
          break;
        case game::Negotiation::Outcome::kGoToStar:
          wm_.post_message(kMessageChangeScreen, kScreenStarSystem, 0);
          break;
        case game::Negotiation::Outcome::kApplied:
          show_alien_text();
          wm_.send_event(map_->id(), kEventRefresh, 0, 0);
          show_options();
          break;
      }
      return true;
    }
    default:
      return GWindow::handle_event(type, a, b);
  }
}

void NegWnd::draw(int /*mode*/) {
  Image8& screen = wm_.screen();
  const game::Session* core = wm_.services().game.core();
  if (negotiation_ && core != nullptr && !negotiation_->third_parties().empty()) {
    vfx_pane_wipe(screen, kThirdPartyBox, kBackColor);
    const ShapeTable& rings = wm_.shared_shapes(kIntelShapes);
    const std::vector<game::RaceIndex>& thirds = negotiation_->third_parties();
    for (std::size_t i = 0; i < thirds.size(); ++i) {
      const int x = kThirdPartyX.at(i);
      const int y = kThirdPartyY.at(i);
      vfx_shape_transform_draw(screen, GSystem::kScreenPane, wm_.shared_shapes(thirds[i]).at(0), x, y, kHalfScale,
                               kHalfScale);
      const int frame = static_cast<int>(i) == negotiation_->selected_third_party() ? kSelectedRing : kRing;
      wm_.draw_colored(GSystem::kScreenPane, rings.at(static_cast<std::size_t>(frame)), x, y,
                       core->game->cosmos.races[thirds[i]].color);
    }
  }
  vfx_pane_wipe(screen, kSystemBox, kBackColor);
  GameFont& small = wm_.small_font();
  small.pane() = kSystemBox;
  small.draw_wrapped(screen, kSystemLabelX, kSystemLabelY, wm_.static_string(kStringSelectedSystem), 0, kDefault, 0xff,
                     0);
  if (core != nullptr) {
    const game::Cosmos& c = core->game->cosmos;
    if (c.selected_star >= 0 && c.selected_star < c.star_count) {
      GameFont& large = wm_.large_font();
      large.pane() = kSystemBox;
      large.draw_wrapped(screen, kSystemNameX, kSystemNameY,
                         game::name_view(c.stars[static_cast<std::size_t>(c.selected_star)].name), 0, kDefault, 0xff,
                         0);
    }
  }
  if (!children_.empty()) {
    draw_children();
  }
}

void add_negotiation_windows(WindowFactory& factory) {
  factory.add(21, [](WinMgr& wm, const WindowRecord& record, const ShapeTable* shapes) -> std::unique_ptr<GWindow> {
    return std::make_unique<NegWnd>(wm, record, shapes);
  });
}

}  // namespace oa
