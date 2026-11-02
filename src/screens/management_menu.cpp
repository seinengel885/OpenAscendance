#include "screens/management_menu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/c_format.h"
#include "game/address.h"
#include "game/planet_plan.h"
#include "game/player_actions.h"
#include "game/session.h"
#include "game/variant.h"
#include "gfx/font.h"

namespace oa {

namespace {

using game::BuildLevel;
using game::PlanetManagement;

// The window's colours (`game.pal`): the toolkit's background and white
// text, green labels, blue-grey section heads and standard values, the
// teal frames of the values and buttons.
constexpr std::uint8_t kBack = GWindow::kBackColor;
constexpr std::uint8_t kHotBack = GWindow::kHotBackColor;
constexpr std::uint8_t kWhite = kTextColor;
constexpr std::uint8_t kGreen = 0xef;
constexpr std::uint8_t kHead = 0xfb;
constexpr std::uint8_t kGrey = 0xfc;
constexpr std::uint8_t kFrame = 0xfe;

// The window: its rectangle, the columns (labels, a sub-row's indent, the
// value box with its arrows, the standard column) and the buttons.
constexpr Pane kBox{60, 14, 579, 465};
constexpr int kLabelX = 16;
constexpr int kIndent = 18;
constexpr int kValueX0 = 268;
constexpr int kValueX1 = 418;
constexpr int kArrow = 16;
constexpr int kStandardX0 = 428;
constexpr int kStandardX1 = 508;
constexpr int kButtonWidth = 110;

// The largest number typed into a value.
constexpr int kMaxNumber = 999;
constexpr std::uint16_t kScanBackspace = 0x0e;
constexpr std::uint16_t kScanEnter = 0x1c;

// What a row of the window sets.
enum class Kind : std::uint8_t { kHead, kStrategy, kLevel, kCount, kIndustry, kResearch, kGrowth, kBoost };

struct Row {
  Row(Kind k, std::string l, int i = 0, bool s = false, std::vector<BuildLevel> v = {})
      : kind(k), label(std::move(l)), index(i), sub(s), levels(std::move(v)) {}
  Kind kind;
  std::string label;  // English (`remake_text` key)
  int index;          // the building or count
  bool sub;           // indented under the row before
  // The levels a building row offers.
  std::vector<BuildLevel> levels;
};

const std::array<BuildLevel, 3> kSurfaceLevels{BuildLevel::kNo, BuildLevel::kAlong, BuildLevel::kFirst};
const std::array<BuildLevel, 4> kOrbitalLevels{BuildLevel::kNo, BuildLevel::kReserve, BuildLevel::kAlong,
                                               BuildLevel::kFirst};

std::string_view level_name(BuildLevel level) {
  switch (level) {
    case BuildLevel::kNo:
      return "Never";
    case BuildLevel::kReserve:
      return "Reserve";
    case BuildLevel::kAlong:
      return "Along";
    case BuildLevel::kFirst:
      return "First";
    default:
      return "Standard";
  }
}

// The management window of one planet: its strategy (self-management off
// or a goal) and its `PlanetManagement`, each value with arrows (a click on
// a number types it), a standard value grey, an own one white with a
// button back to the standard; OK applies, Cancel and Esc do not.
class ManagementBox final : public GWindow {
 public:
  ManagementBox(WinMgr& wm, game::Session& session, game::Planet& planet)
      : GWindow(wm, "MANAGEMENTBOX", kBox),
        session_(session),
        planet_(planet),
        index_(static_cast<std::size_t>(game::planet_index(session.game->cosmos, planet))),
        management_(session.game->planet_management.at(index_)),
        standard_(game::planet_management_standard(session, planet)) {
    mouse_focus_ = true;
    if (planet.self_managed != 0) {
      strategy_ = game::planet_goal(session, planet);
    }
    for (const game::PlanetGoal goal : game::kPlanetGoals) {
      if (game::planet_goal_available(session, planet, goal)) {
        goals_.push_back(goal);
      }
    }
    const std::vector<BuildLevel> surface(kSurfaceLevels.begin(), kSurfaceLevels.end());
    const std::vector<BuildLevel> orbital(kOrbitalLevels.begin(), kOrbitalLevels.end());
    rows_ = {{Kind::kStrategy, "Strategy"},
             {Kind::kHead, "Buildings (Never / Reserve / Along / First)"},
             {Kind::kLevel, "Planetary defence", PlanetManagement::kPlanetaryDefence, false, surface},
             {Kind::kCount, "Surface Shields", PlanetManagement::kSurfaceShields, true},
             {Kind::kCount, "Observation Installation", PlanetManagement::kObservation, true},
             {Kind::kCount, "Surface Cloaker", PlanetManagement::kSurfaceCloaker, true},
             {Kind::kCount, "Tractor Beam", PlanetManagement::kTractorBeam, true},
             {Kind::kLevel, "Orbital defence", PlanetManagement::kOrbitalDefence, false, orbital},
             {Kind::kCount, "Orbital weapons and shields", PlanetManagement::kOrbitalDefences, true},
             {Kind::kCount, "Orbital Cloaker", PlanetManagement::kOrbitalCloaker, true},
             {Kind::kLevel, "Shipyard", PlanetManagement::kShipyard, false, orbital},
             {Kind::kLevel, "Orbital Dock", PlanetManagement::kOrbitalDock, false, orbital}};
    if (game::planet_goal_available(session, planet, game::PlanetGoal::kArchaeology)) {
      rows_.push_back({Kind::kLevel, "Xeno Archeological Dig", PlanetManagement::kDig, false, surface});
    }
    rows_.push_back({Kind::kHead, "Keep (0: off)"});
    rows_.push_back({Kind::kIndustry, "Industry at least"});
    rows_.push_back({Kind::kResearch, "Research at least"});
    rows_.push_back({Kind::kGrowth, "Growth: an inhabitant within"});
    rows_.push_back({Kind::kBoost, "Research boost"});
  }

  bool ok() const { return ok_; }

  // Applies the settings to the planet.
  void apply() {
    game::planet_set_management(session_, planet_, strategy_);
    session_.game->planet_management.at(index_) = management_;
  }

  void press_areas(std::vector<PressArea>& out) const override { out.push_back({pane_, true}); }

  bool handle_event(int type, int a, int b) override {
    switch (type) {
      case kEventLeftClick:
      case kEventRightClick:
        if (!contains(a, b)) {
          return false;
        }
        click(a - pane_.x0, b - pane_.y0, type == kEventRightClick);
        return true;
      case kEventKey:
        if (editing_ < 0) {
          return GWindow::handle_event(type, a, b);
        }
        type_key(static_cast<char>(a), static_cast<std::uint16_t>(b));
        return true;
      case kEventMouseEnter:
      case kEventMouseLeave:
        return true;
      default:
        return GWindow::handle_event(type, a, b);
    }
  }

  void draw(int /*mode*/) override {
    Image8& screen = wm_.screen();
    vfx_pane_wipe(screen, pane_, kBack);
    draw_border(kFrame);
    GameFont& font = wm_.large_font();
    font.pane() = pane_;
    const std::string title = c_format(wm_.remake_text("Management of %s"),
                                       std::string(game::name_view(planet_.name)).c_str());
    font.draw_string(screen, (width() - font.string_width(title)) / 2, 6, title, 0, kWhite, 0xff);
    for (std::size_t r = 0; r < rows_.size(); ++r) {
      draw_row(r);
    }
    draw_button(ok_pane(), wm_.remake_text("OK"));
    draw_button(cancel_pane(), wm_.remake_text("Cancel"));
  }
  void draw_hilite(int /*mode*/) override {}

 private:
  int width() const { return pane_.x1 - pane_.x0 + 1; }
  int line() { return wm_.large_font().font().height + 10; }
  int row_top(std::size_t r) { return 6 + line() + 6 + static_cast<int>(r) * line(); }
  Pane ok_pane() const { return {width() - 2 * kButtonWidth - 24, 418, width() - kButtonWidth - 24, 440}; }
  Pane cancel_pane() const { return {width() - kButtonWidth - 12, 418, width() - 12, 440}; }

  // The value shown for row `r` and whether it is the planet's standard.
  std::pair<std::string, bool> value(const Row& row) {
    switch (row.kind) {
      case Kind::kStrategy:
        return {strategy_ ? wm_.remake_text(game::planet_goal_name(*strategy_)) : wm_.remake_text("Off"), false};
      case Kind::kLevel: {
        const BuildLevel own = management_.levels[static_cast<std::size_t>(row.index)];
        const bool standard = own == BuildLevel::kStandard;
        return {wm_.remake_text(level_name(standard ? standard_.levels[static_cast<std::size_t>(row.index)] : own)),
                standard};
      }
      case Kind::kCount:
        return number(management_.counts[static_cast<std::size_t>(row.index)],
                      standard_.counts[static_cast<std::size_t>(row.index)], "%d");
      case Kind::kIndustry:
        return number(management_.min_industry, standard_.min_industry, "%d");
      case Kind::kResearch:
        return number(management_.min_research, standard_.min_research, "%d");
      case Kind::kGrowth:
        return number(management_.growth_days, standard_.growth_days, wm_.remake_text("%d days"));
      case Kind::kBoost: {
        const bool standard = management_.research_boost < 0;
        const bool on = (standard ? standard_.research_boost : management_.research_boost) > 0;
        return {wm_.remake_text(on ? "On" : "Off"), standard};
      }
      case Kind::kHead:
        break;
    }
    return {"", false};
  }
  std::pair<std::string, bool> number(std::int32_t own, std::int32_t standard, const std::string& format) const {
    return {c_format(format, own < 0 ? standard : own), own < 0};
  }

  void draw_row(std::size_t r) {
    const Row& row = rows_[r];
    Image8& screen = wm_.screen();
    GameFont& font = wm_.large_font();
    const int y = row_top(r);
    const int text_y = y + 3;
    if (row.kind == Kind::kHead) {
      font.draw_string(screen, kLabelX - 6, text_y, wm_.remake_text(row.label), 0, kHead, 0xff);
      return;
    }
    font.draw_string(screen, kLabelX + (row.sub ? kIndent : 0), text_y, wm_.remake_text(row.label), 0, kGreen, 0xff);
    const Pane box{pane_.x0 + kValueX0, pane_.y0 + y, pane_.x0 + kValueX1, pane_.y0 + y + line() - 4};
    vfx_pane_wipe(screen, box, editing_ == static_cast<int>(r) ? kHotBack : kBack);
    frame(box, kFrame);
    const auto [text, standard] = value(row);
    const std::string shown = editing_ == static_cast<int>(r) ? typed_ + "_" : text;
    const int text_x = kValueX0 + (kValueX1 - kValueX0 - font.string_width(shown)) / 2;
    font.draw_string(screen, text_x, text_y, shown, 0, standard ? kGrey : kWhite, 0xff);
    arrow(box, true);
    arrow(box, false);
    if (row.kind == Kind::kStrategy) {
      return;
    }
    const std::string label = wm_.remake_text("Standard");
    const int label_x = kStandardX0 + (kStandardX1 - kStandardX0 - font.string_width(label)) / 2;
    if (standard) {
      font.draw_string(screen, label_x, text_y, label, 0, kGrey, 0xff);
    } else {
      // An own value: the button back to the standard.
      const Pane button{pane_.x0 + kStandardX0, box.y0, pane_.x0 + kStandardX1, box.y1};
      frame(button, kFrame);
      font.draw_string(screen, label_x, text_y, label, 0, kWhite, 0xff);
    }
  }

  void frame(const Pane& p, std::uint8_t color) {
    Image8& screen = wm_.screen();
    const int w = p.x1 - p.x0;
    const int h = p.y1 - p.y0;
    vfx_line_draw(screen, p, 0, 0, w, 0, color);
    vfx_line_draw(screen, p, 0, 0, 0, h, color);
    vfx_line_draw(screen, p, w, h, 0, h, color);
    vfx_line_draw(screen, p, w, h, w, 0, color);
  }

  // A small triangle at the left or right end of a value box.
  void arrow(const Pane& box, bool left) {
    Image8& screen = wm_.screen();
    const int cy = (box.y1 - box.y0) / 2;
    const int x = left ? 5 : box.x1 - box.x0 - 5;
    for (int k = 0; k < 5; ++k) {
      const int dx = left ? x + k : x - k;
      vfx_line_draw(screen, box, dx, cy - k, dx, cy + k, kGreen);
    }
  }

  void draw_button(const Pane& p, const std::string& caption) {
    Image8& screen = wm_.screen();
    const Pane screen_pane{pane_.x0 + p.x0, pane_.y0 + p.y0, pane_.x0 + p.x1, pane_.y0 + p.y1};
    vfx_pane_wipe(screen, screen_pane, kBack);
    frame(screen_pane, kWhite);
    GameFont& font = wm_.large_font();
    font.draw_string(screen, p.x0 + (p.x1 - p.x0 - font.string_width(caption)) / 2, p.y0 + 4, caption, 0, kWhite, 0xff);
  }

  // A click at (x, y) of the window (`back`: the right button, which steps
  // backwards).
  void click(int x, int y, bool back) {
    const bool in_ok = ok_pane().contains(x, y);
    if (in_ok || cancel_pane().contains(x, y)) {
      commit_typing();
      ok_ = in_ok;
      wm_.end_modal(in_ok ? 1 : 0);
      return;
    }
    std::optional<std::size_t> hit;
    for (std::size_t r = 0; r < rows_.size(); ++r) {
      if (y >= row_top(r) && y < row_top(r) + line() - 3) {
        hit = r;
      }
    }
    commit_typing();
    if (!hit || rows_[*hit].kind == Kind::kHead) {
      draw(kDrawNormal);
      return;
    }
    const Row& row = rows_[*hit];
    if (x >= kStandardX0 && x <= kStandardX1 && row.kind != Kind::kStrategy) {
      to_standard(row);
    } else if (x >= kValueX0 && x < kValueX0 + kArrow) {
      step(row, back ? 1 : -1);
    } else if (x > kValueX1 - kArrow && x <= kValueX1) {
      step(row, back ? -1 : 1);
    } else if (x >= kValueX0 && x <= kValueX1) {
      if (numeric(row)) {
        editing_ = static_cast<int>(*hit);
        typed_.clear();
      } else {
        step(row, back ? -1 : 1);
      }
    }
    draw(kDrawNormal);
  }

  static bool numeric(const Row& row) {
    return row.kind == Kind::kCount || row.kind == Kind::kIndustry || row.kind == Kind::kResearch ||
           row.kind == Kind::kGrowth;
  }

  // The stored number of a numeric row.
  std::int16_t& stored(const Row& row) {
    switch (row.kind) {
      case Kind::kCount:
        return management_.counts[static_cast<std::size_t>(row.index)];
      case Kind::kIndustry:
        return management_.min_industry;
      case Kind::kResearch:
        return management_.min_research;
      default:
        return management_.growth_days;
    }
  }
  std::int16_t standard_number(const Row& row) const {
    switch (row.kind) {
      case Kind::kCount:
        return standard_.counts[static_cast<std::size_t>(row.index)];
      case Kind::kIndustry:
        return standard_.min_industry;
      case Kind::kResearch:
        return standard_.min_research;
      default:
        return standard_.growth_days;
    }
  }

  void to_standard(const Row& row) {
    if (row.kind == Kind::kLevel) {
      management_.levels[static_cast<std::size_t>(row.index)] = BuildLevel::kStandard;
    } else if (row.kind == Kind::kBoost) {
      management_.research_boost = -1;
    } else if (numeric(row)) {
      stored(row) = -1;
    }
  }

  // Steps the value of `row` by `delta` (choices wrap, numbers stop at 0).
  void step(const Row& row, int delta) {
    switch (row.kind) {
      case Kind::kStrategy: {
        // Off, then the goals.
        const int n = static_cast<int>(goals_.size()) + 1;
        int at = 0;
        if (strategy_) {
          at = static_cast<int>(std::ranges::find(goals_, *strategy_) - goals_.begin()) + 1;
        }
        at = (at + delta + n) % n;
        strategy_ = at == 0 ? std::nullopt : std::optional(goals_[static_cast<std::size_t>(at - 1)]);
        break;
      }
      case Kind::kLevel: {
        BuildLevel& own = management_.levels[static_cast<std::size_t>(row.index)];
        const BuildLevel now = own == BuildLevel::kStandard ? standard_.levels[static_cast<std::size_t>(row.index)] : own;
        const int n = static_cast<int>(row.levels.size());
        int at = static_cast<int>(std::ranges::find(row.levels, now) - row.levels.begin());
        at = at >= n ? 0 : (at + delta + n) % n;
        own = row.levels[static_cast<std::size_t>(at)];
        break;
      }
      case Kind::kBoost: {
        const bool on = (management_.research_boost < 0 ? standard_.research_boost : management_.research_boost) > 0;
        management_.research_boost = on ? 0 : 1;
        break;
      }
      default: {
        std::int16_t& own = stored(row);
        const int now = own < 0 ? standard_number(row) : own;
        own = static_cast<std::int16_t>(std::clamp(now + delta, 0, kMaxNumber));
        break;
      }
    }
  }

  void type_key(char c, std::uint16_t scan) {
    if (c >= '0' && c <= '9') {
      if (typed_.size() < 3) {
        typed_ += c;
      }
    } else if (scan == kScanBackspace) {
      if (!typed_.empty()) {
        typed_.pop_back();
      }
    } else if (scan == kScanEnter) {
      commit_typing();
    }
    draw(kDrawNormal);
  }

  // Ends the typing of a number, the value typed kept (nothing typed: the
  // value stays).
  void commit_typing() {
    if (editing_ < 0) {
      return;
    }
    if (!typed_.empty()) {
      stored(rows_[static_cast<std::size_t>(editing_)]) = static_cast<std::int16_t>(std::stoi(typed_));
    }
    editing_ = -1;
    typed_.clear();
  }

  game::Session& session_;
  game::Planet& planet_;
  std::size_t index_;
  PlanetManagement management_;
  PlanetManagement standard_;
  std::optional<game::PlanetGoal> strategy_;
  std::vector<game::PlanetGoal> goals_;
  std::vector<Row> rows_;
  int editing_ = -1;
  std::string typed_;
  bool ok_ = false;
};

// The label window of planets: a check box per label (set on all of
// them, on some: grey, on none), a last row to type a new one; OK applies
// and drops the labels no planet has any more.
class LabelBox final : public GWindow {
 public:
  LabelBox(WinMgr& wm, game::Session& session, std::span<game::Planet* const> planets)
      : GWindow(wm, "LABELBOX", kLabelBox), session_(session), labels_(session.game->planet_labels) {
    mouse_focus_ = true;
    for (game::Planet* p : planets) {
      masks_.push_back(&session.game->planet_management.at(
          static_cast<std::size_t>(game::planet_index(session.game->cosmos, *p))).labels);
    }
    for (std::size_t i = 0; i < labels_.size(); ++i) {
      const auto has = [&](std::uint32_t* m) { return ((*m >> i) & 1u) != 0; };
      states_.push_back(std::ranges::all_of(masks_, has) ? kOn : std::ranges::any_of(masks_, has) ? kSome : kOff);
    }
  }

  bool ok() const { return ok_; }

  void apply() {
    for (std::size_t i = 0; i < states_.size(); ++i) {
      for (std::uint32_t* m : masks_) {
        if (states_[i] == kOn) {
          *m |= 1u << i;
        } else if (states_[i] == kOff) {
          *m &= ~(1u << i);
        }
      }
    }
    // The labels no planet has: dropped, the bits above moved down.
    auto& all = session_.game->planet_management;
    for (std::size_t i = labels_.size(); i-- > 0;) {
      if (std::ranges::none_of(all, [&](const PlanetManagement& m) { return ((m.labels >> i) & 1u) != 0; })) {
        labels_.erase(labels_.begin() + static_cast<std::ptrdiff_t>(i));
        const std::uint32_t low = (1u << i) - 1u;
        for (PlanetManagement& m : all) {
          m.labels = (m.labels & low) | ((m.labels >> 1) & ~low);
        }
      }
    }
    session_.game->planet_labels = labels_;
  }

  void press_areas(std::vector<PressArea>& out) const override { out.push_back({pane_, true}); }

  bool handle_event(int type, int a, int b) override {
    switch (type) {
      case kEventLeftClick:
      case kEventRightClick:
        if (!contains(a, b)) {
          return false;
        }
        click(a - pane_.x0, b - pane_.y0);
        return true;
      case kEventKey:
        if (!typing_) {
          return GWindow::handle_event(type, a, b);
        }
        type_key(static_cast<char>(a), static_cast<std::uint16_t>(b));
        return true;
      case kEventMouseEnter:
      case kEventMouseLeave:
        return true;
      default:
        return GWindow::handle_event(type, a, b);
    }
  }

  void draw(int /*mode*/) override {
    Image8& screen = wm_.screen();
    vfx_pane_wipe(screen, pane_, kBack);
    draw_border(kFrame);
    GameFont& font = wm_.large_font();
    font.pane() = pane_;
    const std::string title = wm_.remake_text("Labels");
    font.draw_string(screen, (width() - font.string_width(title)) / 2, 6, title, 0, kWhite, 0xff);
    for (std::size_t i = 0; i <= labels_.size(); ++i) {
      const int y = row_top(i);
      if (i == labels_.size()) {
        if (labels_.size() < game::kMaxPlanetLabels) {
          const std::string text = typing_ ? typed_ + "_" : wm_.remake_text("New label ...");
          font.draw_string(screen, 40, y + 3, text, 0, typing_ ? kWhite : kGreen, 0xff);
        }
        break;
      }
      const Pane box{pane_.x0 + 14, pane_.y0 + y + 2, pane_.x0 + 30, pane_.y0 + y + 18};
      vfx_pane_wipe(screen, box, states_[i] == kSome ? kGrey : kBack);
      frame(box);
      if (states_[i] == kOn) {
        for (int k = 0; k < 3; ++k) {
          vfx_line_draw(screen, box, 3, 8 + k - 1, 6, 11 + k - 1, kWhite);
          vfx_line_draw(screen, box, 6, 11 + k - 1, 13, 3 + k - 1, kWhite);
        }
      }
      font.draw_string(screen, 40, y + 3, labels_[i], 0, kGreen, 0xff);
    }
    button(ok_pane(), wm_.remake_text("OK"));
    button(cancel_pane(), wm_.remake_text("Cancel"));
  }
  void draw_hilite(int /*mode*/) override {}

 private:
  enum State : std::uint8_t { kOff, kOn, kSome };
  static constexpr Pane kLabelBox{170, 40, 469, 439};
  static constexpr int kRowPitch = 22;

  int width() const { return pane_.x1 - pane_.x0 + 1; }
  static int row_top(std::size_t i) { return 34 + static_cast<int>(i) * kRowPitch; }
  Pane ok_pane() const { return {width() - 220, 370, width() - 116, 392}; }
  Pane cancel_pane() const { return {width() - 108, 370, width() - 10, 392}; }

  void frame(const Pane& p) {
    Image8& screen = wm_.screen();
    const int w = p.x1 - p.x0;
    const int h = p.y1 - p.y0;
    vfx_line_draw(screen, p, 0, 0, w, 0, kFrame);
    vfx_line_draw(screen, p, 0, 0, 0, h, kFrame);
    vfx_line_draw(screen, p, w, h, 0, h, kFrame);
    vfx_line_draw(screen, p, w, h, w, 0, kFrame);
  }
  void button(const Pane& p, const std::string& caption) {
    const Pane on_screen{pane_.x0 + p.x0, pane_.y0 + p.y0, pane_.x0 + p.x1, pane_.y0 + p.y1};
    vfx_pane_wipe(wm_.screen(), on_screen, kBack);
    frame(on_screen);
    GameFont& font = wm_.large_font();
    font.draw_string(wm_.screen(), p.x0 + (p.x1 - p.x0 - font.string_width(caption)) / 2, p.y0 + 4, caption, 0,
                     kWhite, 0xff);
  }

  void click(int x, int y) {
    commit();
    if (ok_pane().contains(x, y) || cancel_pane().contains(x, y)) {
      ok_ = ok_pane().contains(x, y);
      wm_.end_modal(ok_ ? 1 : 0);
      return;
    }
    for (std::size_t i = 0; i <= labels_.size(); ++i) {
      if (y >= row_top(i) && y < row_top(i) + kRowPitch) {
        if (i < labels_.size()) {
          states_[i] = states_[i] == kOn ? kOff : kOn;
        } else if (labels_.size() < game::kMaxPlanetLabels) {
          typing_ = true;
        }
      }
    }
    draw(kDrawNormal);
  }

  void type_key(char c, std::uint16_t scan) {
    const bool letter = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '-';
    if (letter && typed_.size() < kMaxLabelLength) {
      typed_ += c;
    } else if (scan == kScanBackspace && !typed_.empty()) {
      typed_.pop_back();
    } else if (scan == kScanEnter) {
      commit();
    }
    draw(kDrawNormal);
  }

  // Ends the typing: a new label, set on the planets.
  void commit() {
    if (typing_ && !typed_.empty() && std::ranges::find(labels_, typed_) == labels_.end()) {
      labels_.push_back(typed_);
      states_.push_back(kOn);
    }
    typing_ = false;
    typed_.clear();
  }

  static constexpr std::size_t kMaxLabelLength = 16;
  game::Session& session_;
  std::vector<std::string> labels_;
  std::vector<std::uint32_t*> masks_;
  std::vector<State> states_;
  bool typing_ = false;
  std::string typed_;
  bool ok_ = false;
};

}  // namespace

bool management_shown(const game::Session& session, const game::Planet& planet) {
  return session.game->variant == game::Variant::kPeCustom && planet.owner == session.player_race;
}

std::string management_label(const WinMgr& wm, const game::Session& session, const game::Planet& planet) {
  const std::string state = planet.self_managed == 0
                                ? wm.remake_text("Off")
                                : wm.remake_text(game::planet_goal_name(game::planet_goal(session, planet)));
  return c_format(wm.remake_text("Management: %s"), state.c_str());
}

bool run_management_menu(WinMgr& wm, game::Session& session, game::Planet& planet) {
  auto box = std::make_unique<ManagementBox>(wm, session, planet);
  ManagementBox& box_ref = *box;
  const int box_id = wm.register_window(std::move(box));
  wm.do_modal(box_ref, true);
  const bool ok = box_ref.ok();
  if (ok) {
    box_ref.apply();
  }
  wm.release_window(box_id);
  return ok;
}

bool run_strategy_menu(WinMgr& wm, game::Session& session, std::span<game::Planet* const> planets) {
  std::vector<std::optional<game::PlanetGoal>> options{std::nullopt};
  std::vector<std::string> names{wm.remake_text("Off")};
  for (const game::PlanetGoal goal : game::kPlanetGoals) {
    // The goals every planet may take.
    if (std::ranges::all_of(planets, [&](const game::Planet* p) { return game::planet_goal_available(session, *p, goal); })) {
      options.emplace_back(goal);
      names.push_back(wm.remake_text(game::planet_goal_name(goal)));
    }
  }
  const std::string title = c_format(wm.remake_text("Strategy of %d planets"), static_cast<int>(planets.size()));
  const int pick = wm.choice_box(title, names, -1);
  if (pick < 0) {
    return false;
  }
  for (game::Planet* p : planets) {
    game::planet_set_management(session, *p, options[static_cast<std::size_t>(pick)]);
  }
  return true;
}

}  // namespace oa

namespace oa {

std::string planet_labels_text(const game::Session& session, const game::Planet& planet) {
  const std::uint32_t mask =
      session.game->planet_management.at(static_cast<std::size_t>(game::planet_index(session.game->cosmos, planet))).labels;
  std::string out;
  for (std::size_t i = 0; i < session.game->planet_labels.size(); ++i) {
    if (((mask >> i) & 1u) != 0) {
      out += (out.empty() ? "" : ", ") + session.game->planet_labels[i];
    }
  }
  return out;
}

void draw_label_icon(Image8& screen, int x, int y, std::uint8_t color) {
  // A tag: a box with a pointed left end and a hole.
  const Pane p{x, y, x + 15, y + 9};
  for (int k = 0; k <= 9; ++k) {
    const int left = std::abs(k - 4) + (k > 4 ? -1 : 0);
    vfx_line_draw(screen, p, left, k, 15, k, color);
  }
  vfx_line_draw(screen, p, 5, 4, 6, 4, kBack);
  vfx_line_draw(screen, p, 5, 5, 6, 5, kBack);
}

bool run_label_menu(WinMgr& wm, game::Session& session, std::span<game::Planet* const> planets) {
  auto box = std::make_unique<LabelBox>(wm, session, planets);
  LabelBox& box_ref = *box;
  const int box_id = wm.register_window(std::move(box));
  wm.do_modal(box_ref, true);
  const bool ok = box_ref.ok();
  if (ok) {
    box_ref.apply();
  }
  wm.release_window(box_id);
  return ok;
}

}  // namespace oa
