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
#include "screens/options_wnd.h"

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

// One planet of the management window: its settings as edited, its
// standards, its strategy (none: self-management off).
struct Target {
  game::Planet* planet;
  PlanetManagement own;
  PlanetManagement standard;
  std::optional<game::PlanetGoal> strategy;
};

// The management window of one or more planets: the strategy
// (self-management off or a goal) and the `PlanetManagement`, each value
// with arrows (a click on a number types it), a standard value grey, an
// own one white with a button back to the standard. A row the planets
// differ in shows "-"; a change sets the row on all of them, the rows not
// changed stay each planet's own. OK applies, Cancel and Esc do not.
class ManagementBox final : public GWindow {
 public:
  ManagementBox(WinMgr& wm, game::Session& session, std::span<game::Planet* const> planets)
      : GWindow(wm, "MANAGEMENTBOX", kBox), session_(session) {
    mouse_focus_ = true;
    bool ruins = false;
    for (game::Planet* p : planets) {
      const auto index = static_cast<std::size_t>(game::planet_index(session.game->cosmos, *p));
      targets_.push_back({p, session.game->planet_management.at(index), game::planet_management_standard(session, *p),
                          p->self_managed != 0 ? std::optional(game::planet_goal(session, *p)) : std::nullopt});
      ruins = ruins || game::planet_goal_available(session, *p, game::PlanetGoal::kArchaeology);
    }
    // The goals every planet may take.
    for (const game::PlanetGoal goal : game::kPlanetGoals) {
      if (std::ranges::all_of(planets, [&](const game::Planet* p) { return game::planet_goal_available(session, *p, goal); })) {
        goals_.push_back(goal);
      }
    }
    const std::vector<BuildLevel> surface(kSurfaceLevels.begin(), kSurfaceLevels.end());
    const std::vector<BuildLevel> orbital(kOrbitalLevels.begin(), kOrbitalLevels.end());
    rows_ = {{Kind::kStrategy, "Strategy"},
             {Kind::kBoost, "Short-term research boost"},
             {Kind::kHead, "Buildings (Never / Reserve / Along / First)"},
             {Kind::kLevel, "Planetary defence", PlanetManagement::kPlanetaryDefence, false, surface},
             {Kind::kCount, "Surface Shields", PlanetManagement::kSurfaceShields, true},
             {Kind::kCount, "Observation Installation", PlanetManagement::kObservation, true},
             {Kind::kCount, "Surface Cloaker", PlanetManagement::kSurfaceCloaker, true},
             {Kind::kCount, "Tractor Beam", PlanetManagement::kTractorBeam, true},
             {Kind::kLevel, "Orbital defence", PlanetManagement::kOrbitalDefence, false, orbital},
             {Kind::kCount, "Orbital weapons", PlanetManagement::kOrbitalWeapons, true},
             {Kind::kCount, "Orbital shields", PlanetManagement::kOrbitalShields, true},
             {Kind::kCount, "Orbital Cloaker", PlanetManagement::kOrbitalCloaker, true},
             {Kind::kLevel, "Shipyard", PlanetManagement::kShipyard, false, orbital},
             {Kind::kLevel, "Orbital Dock", PlanetManagement::kOrbitalDock, false, orbital}};
    if (ruins) {
      rows_.push_back({Kind::kLevel, "Xeno Archeological Dig", PlanetManagement::kDig, false, surface});
    }
    rows_.push_back({Kind::kHead, "Keep (0: off)"});
    rows_.push_back({Kind::kIndustry, "Industry at least"});
    rows_.push_back({Kind::kResearch, "Research at least"});
    rows_.push_back({Kind::kGrowth, "Growth: an inhabitant within"});
  }

  bool ok() const { return ok_; }

  // Applies the settings to the planets (the strategy only when changed).
  void apply() {
    for (const Target& t : targets_) {
      if (strategy_changed_) {
        game::planet_set_management(session_, *t.planet, t.strategy);
      }
      session_.game->planet_management.at(static_cast<std::size_t>(game::planet_index(session_.game->cosmos, *t.planet))) =
          t.own;
    }
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
    const std::string title =
        targets_.size() == 1
            ? c_format(wm_.remake_text("Management of %s"), std::string(game::name_view(targets_[0].planet->name)).c_str())
            : c_format(wm_.remake_text("Management of %d planets"), static_cast<int>(targets_.size()));
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

  // The value of `row` for planet `t` and whether it is the standard.
  std::pair<std::string, bool> value(const Target& t, const Row& row) {
    const auto i = static_cast<std::size_t>(row.index);
    switch (row.kind) {
      case Kind::kStrategy:
        return {t.strategy ? wm_.remake_text(game::planet_goal_name(*t.strategy)) : wm_.remake_text("Off"), false};
      case Kind::kLevel: {
        const bool standard = t.own.levels[i] == BuildLevel::kStandard;
        return {wm_.remake_text(level_name(standard ? t.standard.levels[i] : t.own.levels[i])), standard};
      }
      case Kind::kBoost: {
        const bool standard = t.own.research_boost < 0;
        return {wm_.remake_text((standard ? t.standard.research_boost : t.own.research_boost) > 0 ? "Yes" : "No"),
                standard};
      }
      case Kind::kHead:
        return {"", false};
      default: {
        const std::int16_t own = stored(const_cast<Target&>(t), row);
        const std::string format = row.kind == Kind::kGrowth ? wm_.remake_text("%d days") : std::string("%d");
        return {c_format(format, own < 0 ? standard_number(t, row) : own), own < 0};
      }
    }
  }
  // The value shown for `row`: the planets' common one, "-" where they
  // differ; standard while it is every planet's standard.
  std::pair<std::string, bool> value(const Row& row) {
    auto [text, standard] = value(targets_[0], row);
    for (const Target& t : targets_) {
      const auto [other, other_standard] = value(t, row);
      text = other == text ? text : "-";
      standard = standard && other_standard;
    }
    return {text, standard};
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

  // The stored number of a numeric row of planet `t`.
  static std::int16_t& stored(Target& t, const Row& row) {
    switch (row.kind) {
      case Kind::kCount:
        return t.own.counts[static_cast<std::size_t>(row.index)];
      case Kind::kIndustry:
        return t.own.min_industry;
      case Kind::kResearch:
        return t.own.min_research;
      default:
        return t.own.growth_days;
    }
  }
  static std::int16_t standard_number(const Target& t, const Row& row) {
    Target copy{nullptr, t.standard, t.standard, std::nullopt};
    return stored(copy, row);
  }

  // Sets `row` of every planet to that of the first one.
  void spread(const Row& row) {
    const Target& first = targets_[0];
    for (Target& t : targets_) {
      switch (row.kind) {
        case Kind::kStrategy:
          t.strategy = first.strategy;
          strategy_changed_ = true;
          break;
        case Kind::kLevel:
          t.own.levels[static_cast<std::size_t>(row.index)] = first.own.levels[static_cast<std::size_t>(row.index)];
          break;
        case Kind::kBoost:
          t.own.research_boost = first.own.research_boost;
          break;
        case Kind::kHead:
          break;
        default:
          stored(t, row) = stored(targets_[0], row);
          break;
      }
    }
  }

  void to_standard(const Row& row) {
    Target& first = targets_[0];
    if (row.kind == Kind::kLevel) {
      first.own.levels[static_cast<std::size_t>(row.index)] = BuildLevel::kStandard;
    } else if (row.kind == Kind::kBoost) {
      first.own.research_boost = -1;
    } else if (numeric(row)) {
      stored(first, row) = -1;
    }
    spread(row);
  }

  // Steps the value of `row` by `delta` (choices wrap, numbers stop at 0),
  // from the first planet's, for all.
  void step(const Row& row, int delta) {
    Target& t = targets_[0];
    switch (row.kind) {
      case Kind::kStrategy: {
        // Off, then the goals.
        const int n = static_cast<int>(goals_.size()) + 1;
        int at = 0;
        if (t.strategy) {
          at = static_cast<int>(std::ranges::find(goals_, *t.strategy) - goals_.begin()) + 1;
          at = at > static_cast<int>(goals_.size()) ? 0 : at;
        }
        at = (at + delta + n) % n;
        t.strategy = at == 0 ? std::nullopt : std::optional(goals_[static_cast<std::size_t>(at - 1)]);
        break;
      }
      case Kind::kLevel: {
        const auto i = static_cast<std::size_t>(row.index);
        BuildLevel& own = t.own.levels[i];
        const BuildLevel now = own == BuildLevel::kStandard ? t.standard.levels[i] : own;
        const int n = static_cast<int>(row.levels.size());
        int at = static_cast<int>(std::ranges::find(row.levels, now) - row.levels.begin());
        at = at >= n ? 0 : (at + delta + n) % n;
        own = row.levels[static_cast<std::size_t>(at)];
        break;
      }
      case Kind::kBoost: {
        const bool on = (t.own.research_boost < 0 ? t.standard.research_boost : t.own.research_boost) > 0;
        t.own.research_boost = on ? 0 : 1;
        break;
      }
      default: {
        std::int16_t& own = stored(t, row);
        const int now = own < 0 ? standard_number(t, row) : own;
        // No more than room is left for.
        own = static_cast<std::int16_t>(std::max(0, delta > 0 && now + delta > largest(row) ? now : now + delta));
        break;
      }
    }
    spread(row);
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

  // The largest number `row` may take on every planet: the orbit's room
  // left by the other orbital defences for theirs
  // (`planet_orbital_defence_room`).
  int largest(const Row& row) const {
    const std::array<PlanetManagement::Count, 3> orbital{PlanetManagement::kOrbitalWeapons, PlanetManagement::kOrbitalShields,
                                                         PlanetManagement::kOrbitalCloaker};
    if (row.kind != Kind::kCount || std::ranges::find(orbital, row.index) == orbital.end()) {
      return kMaxNumber;
    }
    int out = kMaxNumber;
    for (const Target& t : targets_) {
      const PlanetManagement m = game::planet_management_resolved(t.own, t.standard);
      int left = game::planet_orbital_defence_room(session_, *t.planet, m);
      for (const PlanetManagement::Count c : orbital) {
        left -= c == row.index ? 0 : m.counts[c];
      }
      out = std::min(out, left);
    }
    return std::max(0, out);
  }

  // Ends the typing of a number, the value typed kept for all (nothing
  // typed: the values stay).
  void commit_typing() {
    if (editing_ < 0) {
      return;
    }
    if (!typed_.empty()) {
      const Row& row = rows_[static_cast<std::size_t>(editing_)];
      stored(targets_[0], row) = static_cast<std::int16_t>(std::min(std::stoi(typed_), largest(row)));
      spread(row);
    }
    editing_ = -1;
    typed_.clear();
  }

  game::Session& session_;
  std::vector<Target> targets_;
  std::vector<game::PlanetGoal> goals_;
  std::vector<Row> rows_;
  bool strategy_changed_ = false;
  int editing_ = -1;
  std::string typed_;
  bool ok_ = false;
};

// The label dialog's state: per label (in `labels` order) whether all the
// planets have it, some or none.
enum class Mark : std::uint8_t { kNone, kSome, kAll };

// A label of the label dialog: a text button of the toolkit, in the red of
// the remake's selected choices while all the planets have the label
// (darker while some have it); a click toggles it.
class LabelButton final : public GWindow {
 public:
  LabelButton(WinMgr& wm, std::string caption, const Pane& pane, Mark& mark)
      : GWindow(wm, std::move(caption), pane), mark_(mark) {
    mouse_focus_ = true;
    border_ = true;
    set_text_pos(-1, 3);
  }

  bool handle_event(int type, int a, int b) override {
    if (type == kEventLeftClick && contains(a, b)) {
      mark_ = mark_ == Mark::kAll ? Mark::kNone : Mark::kAll;
      draw(kDrawHot);
      return true;
    }
    return GWindow::handle_event(type, a, b);
  }
  void draw(int mode) override {
    const bool hot = mode == kDrawHot;
    std::uint8_t back = hot ? kHotBack : kBack;
    if (mark_ == Mark::kAll) {
      back = hot ? kSelectedHotColor : kSelectedColor;
    } else if (mark_ == Mark::kSome) {
      back = kSomeColor;
    }
    draw_text(back);
  }
  void draw_hilite(int mode) override { draw(mode); }

 private:
  // The red ramp's darker step: some of the planets have the label.
  static constexpr std::uint8_t kSomeColor = 0x50;
  Mark& mark_;
};

// The longest label typed.
constexpr int kMaxLabelLength = 16;
// The label dialog's results.
constexpr int kLabelsOk = 1;
constexpr int kLabelsNew = 2;
// The labels the dialog offers at most (its buttons are the children of
// one window, with New, OK and Cancel).
constexpr std::size_t kMaxLabels = 16;

// Runs the dialog once over `labels` (sorted) and `marks`; returns its
// result (0 cancel).
int label_dialog(WinMgr& wm, std::string_view title, const std::vector<std::string>& labels, std::vector<Mark>& marks) {
  GameFont& font = wm.large_font();
  const int line = font.font().height + 8;
  constexpr int kColumn = 150;
  constexpr int kGap = 8;
  const int rows = std::max<int>(1, (static_cast<int>(labels.size()) + 1) / 2);
  const int width = 2 * kColumn + 3 * kGap;
  const int height = line * (rows + 2) + 3 * kGap;
  const int x0 = (kScreenWidth - width) / 2;
  const int y0 = (kScreenHeight - height) / 2;
  auto box = std::make_unique<GWindow>(wm, std::string(title), Pane{x0, y0, x0 + width, y0 + height});
  box->set_text_pos(-1, 5);
  box->set_border(true);
  GWindow& box_ref = *box;
  const int box_id = wm.register_window(std::move(box));
  std::vector<int> ids;
  const auto add = [&](std::unique_ptr<GWindow> w) {
    w->set_parent(box_id);
    box_ref.add_child(w.get());
    ids.push_back(wm.register_window(std::move(w)));
  };
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const int col = static_cast<int>(i) / rows;
    const int row = static_cast<int>(i) % rows;
    const int x = x0 + kGap + col * (kColumn + kGap);
    const int y = y0 + line + kGap + row * line;
    add(std::make_unique<LabelButton>(wm, labels[i], Pane{x, y, x + kColumn, y + line - 3}, marks[i]));
  }
  // The bottom row: New (while there is room), OK, Cancel.
  const int y = y0 + height - line - kGap;
  const int third = (width - 4 * kGap) / 3;
  const std::array<std::pair<std::string, int>, 3> buttons{
      {{wm.remake_text("New label ..."), kLabelsNew}, {wm.remake_text("OK"), kLabelsOk}, {wm.remake_text("Cancel"), 0}}};
  for (std::size_t k = 0; k < buttons.size(); ++k) {
    if (buttons[k].second == kLabelsNew && labels.size() >= kMaxLabels) {
      continue;
    }
    const int x = x0 + kGap + static_cast<int>(k) * (third + kGap);
    auto button = std::make_unique<GWindow>(wm, buttons[k].first, Pane{x, y, x + third, y + line - 3});
    button->set_text_pos(-1, 3);
    button->set_mouse_focus(true);
    button->set_border(true);
    button->set_message(kMessageToWinMgr, kMessageEndModal, buttons[k].second);
    add(std::move(button));
  }
  const int result = wm.do_modal(box_ref, true);
  for (const int id : ids) {
    wm.release_window(id);
  }
  wm.release_window(box_id);
  return result;
}

// Applies `marks` of `labels` to `planets`: the labels as the game's (new
// ones appended), set on them (all) or taken off (none); then the labels
// no planet has are dropped, the bits above moved down.
void apply_labels(game::Session& session, std::span<game::Planet* const> planets,
                  const std::vector<std::string>& labels, const std::vector<Mark>& marks) {
  std::vector<std::string>& all = session.game->planet_labels;
  auto& management = session.game->planet_management;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    auto at = std::ranges::find(all, labels[i]);
    if (at == all.end()) {
      at = all.insert(all.end(), labels[i]);
    }
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(at - all.begin());
    for (game::Planet* p : planets) {
      std::uint32_t& mask = management.at(static_cast<std::size_t>(game::planet_index(session.game->cosmos, *p))).labels;
      mask = marks[i] == Mark::kAll ? mask | bit : marks[i] == Mark::kNone ? mask & ~bit : mask;
    }
  }
  for (std::size_t i = all.size(); i-- > 0;) {
    if (std::ranges::none_of(management, [&](const PlanetManagement& m) { return ((m.labels >> i) & 1u) != 0; })) {
      all.erase(all.begin() + static_cast<std::ptrdiff_t>(i));
      const std::uint32_t low = (1u << i) - 1u;
      for (PlanetManagement& m : management) {
        m.labels = (m.labels & low) | ((m.labels >> 1) & ~low);
      }
    }
  }
}

}  // namespace

bool management_shown(const game::Session& session, const game::Planet& planet) {
  return session.game->variant == game::Variant::kPeCustom && planet.owner == session.player_race;
}

std::string management_label(const WinMgr& wm, const game::Session& session, const game::Planet& planet) {
  std::string state = planet.self_managed == 0
                          ? wm.remake_text("Off")
                          : wm.remake_text(game::planet_goal_name(game::planet_goal(session, planet)));
  if (planet.self_managed != 0 && game::planet_management(session, planet).research_boost > 0) {
    state = c_format(wm.remake_text("%s, research boost"), state.c_str());
  }
  return c_format(wm.remake_text("Management: %s"), state.c_str());
}

bool run_management_menu(WinMgr& wm, game::Session& session, std::span<game::Planet* const> planets) {
  if (planets.empty()) {
    return false;
  }
  auto box = std::make_unique<ManagementBox>(wm, session, planets);
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
  // The labels in ascending order, each marked by how many of the planets
  // have it.
  std::vector<std::string> labels = session.game->planet_labels;
  std::ranges::sort(labels);
  std::vector<Mark> marks;
  for (const std::string& label : labels) {
    const auto bit = 1u << static_cast<std::uint32_t>(std::ranges::find(session.game->planet_labels, label) -
                                                       session.game->planet_labels.begin());
    const auto has = [&](const game::Planet* p) {
      return (session.game->planet_management.at(static_cast<std::size_t>(game::planet_index(session.game->cosmos, *p)))
                  .labels &
              bit) != 0;
    };
    marks.push_back(std::ranges::all_of(planets, has) ? Mark::kAll
                    : std::ranges::any_of(planets, has) ? Mark::kSome
                                                        : Mark::kNone);
  }
  const std::string title =
      planets.size() == 1 ? c_format(wm.remake_text("Labels of %s"), std::string(game::name_view(planets[0]->name)).c_str())
                          : c_format(wm.remake_text("Labels of %d planets"), static_cast<int>(planets.size()));
  while (true) {
    const int result = label_dialog(wm, title, labels, marks);
    if (result == kLabelsOk) {
      apply_labels(session, planets, labels, marks);
      return true;
    }
    if (result != kLabelsNew) {
      return false;
    }
    // A new label (the game's input box), set on the planets.
    std::string name;
    if (wm.input_box(wm.remake_text("New label"), name, kMaxLabelLength) != 0 && !name.empty() &&
        std::ranges::find(labels, name) == labels.end()) {
      const auto at = std::ranges::upper_bound(labels, name);
      marks.insert(marks.begin() + (at - labels.begin()), Mark::kAll);
      labels.insert(at, name);
    }
  }
}

}  // namespace oa
