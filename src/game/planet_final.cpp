#include "game/planet_final.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

#include "game/planet.h"
#include "game/planet_plan_sim.h"
#include "game/tables.h"

namespace oa::game {

namespace {

using plan_detail::race_knows_item;

constexpr std::uint8_t kBlack = 0;
constexpr std::uint8_t kWhite = 1;
// The colour whose bonus each output's buildings get (red, blue, green).
constexpr std::array<std::uint8_t, PlanetManagement::kOutputs> kColour{2, 4, 3};

// The buildings the final build-out lays out anew: per output its plain
// buildings, then housing (Habitat, Metroplex, Outpost). Everything else on
// the planet stays.
constexpr std::array<std::uint8_t, 6> kOutputItems{0, 1, 2, 6, 7, 8};
constexpr std::array<std::uint8_t, 3> kHousingItems{3, 4, plan_detail::kOutpost};
// The planetary defences of `PlanetManagement::counts` and their items.
constexpr std::array<std::pair<PlanetManagement::Count, std::uint8_t>, 4> kDefences{
    {{PlanetManagement::kSurfaceShields, plan_detail::kSurfaceShield},
     {PlanetManagement::kObservation, 16},
     {PlanetManagement::kSurfaceCloaker, 11},
     {PlanetManagement::kTractorBeam, kItemTractorBeam}}};

std::int32_t output_of(const PlanItem& it, std::size_t output) {
  return output == PlanetManagement::kIndustry ? it.industry : output == PlanetManagement::kResearch ? it.research : it.prosperity;
}

bool laid_out(std::uint8_t item) {
  return std::ranges::find(kOutputItems, item) != kOutputItems.end() ||
         std::ranges::find(kHousingItems, item) != kHousingItems.end();
}

class Layout {
 public:
  Layout(const Session& session, const Planet& planet, const PlanetManagement& m)
      : session_(session), planet_(planet), m_(m) {
    const std::span<const Square> real = planet_squares(session.game->cosmos, planet);
    base_.assign(real.begin(), real.end());
    cap_ = planet.base_population;
    // Black squares are usable by Orfa's ability, and by all once
    // Terraforming is known (they will be terraformed).
    const bool black_usable = session.game->cosmos.races.at(planet.owner).ability == 2 ||
                              race_knows_item(session, planet, kItemTerraforming);
    std::int32_t defences_built = 0;
    std::array<std::int32_t, PlanetManagement::kOutputs> kept{};
    for (std::uint16_t i = 0; i < base_.size(); ++i) {
      Square& sq = base_[i];
      // An automated square needs no worker (laid out anew, it stays as
      // it is: one worker less for its square).
      const bool automated = (sq.flags & kSquareAutomated) != 0;
      if (laid_out(sq.item)) {
        const PlanItem& it = item(sq.item);
        for (std::size_t o = 0; o < kept.size(); ++o) {
          kept[o] += output_of(it, o) > 0 && it.max_population == 0 ? 1 : 0;
        }
        workers_ -= automated ? it.used_population : 0;
        sq = Square{sq.color, kNoItem, 0};
      }
      if (sq.item != kNoItem) {
        const PlanItem& it = item(sq.item);
        cap_ += it.max_population;
        workers_ += (it.flags & kItemNoPopulation) == 0 && !automated ? it.used_population : 0;
        defences_built += std::ranges::any_of(kDefences, [&](const auto& d) { return d.second == sq.item; }) ? 1 : 0;
      } else if (i < planet.surface_squares && (sq.color & kSquareRuins) == 0 &&
                 ((sq.color & 7u) != kBlack || black_usable)) {
        free_.push_back(i);
      }
    }
    kept_ = kept;
    // The planetary defences still to come take squares and workers.
    if (m.levels[PlanetManagement::kPlanetaryDefence] != BuildLevel::kNo) {
      std::int32_t wanted = 0;
      for (const auto& d : kDefences) {
        wanted += race_knows_item(session, planet, d.second) ? m.counts[d.first] : 0;
      }
      const std::int32_t more = std::clamp<std::int32_t>(wanted - defences_built, 0, static_cast<std::int32_t>(free_.size()));
      // On the squares worth least: black, white, then any.
      std::ranges::stable_sort(free_, [&](std::uint16_t a, std::uint16_t b) { return rank(a) < rank(b); });
      free_.erase(free_.begin(), free_.begin() + more);
      workers_ += more;
      other_extra_ = more;
    }
  }

  // The layout with the known housing that gives the maximised outputs
  // the most points (a Metroplex adds output, an Outpost or Habitat more
  // room).
  FinalLayout run() {
    FinalLayout best = run(kNoItem);
    double best_value = -1.0;
    for (const std::uint8_t house : kHousingItems) {
      if (!race_knows_item(session_, planet_, house) || net_room(house) <= 0) {
        continue;
      }
      FinalLayout layout = run(house);
      if (const double v = value(layout); v > best_value) {
        best = layout;
        best_value = v;
      }
    }
    return best;
  }

 private:
  // The layout with `house` as its housing (`kNoItem`: none).
  FinalLayout run(std::uint8_t house) {
    FinalLayout out;
    const auto f = static_cast<std::int32_t>(free_.size());
    // Housing for the workers of all squares.
    std::int32_t housing = 0;
    if (house != kNoItem) {
      const std::int32_t net = net_room(house);
      housing = std::clamp<std::int32_t>((f + workers_ - cap_ + net) / (net + 1), 0, f);
    }
    const std::int32_t squares = f - housing;
    // The squares each output asks for: kept, at least, maximised.
    const bool maximised = std::ranges::any_of(m_.final_outputs, [](const FinalOutput& fo) { return fo.mode == FinalMode::kMaximise; });
    std::array<std::int32_t, PlanetManagement::kOutputs> ask{};
    std::array<std::int32_t, PlanetManagement::kOutputs> weight{};
    for (std::size_t o = 0; o < ask.size(); ++o) {
      const FinalOutput& fo = m_.final_outputs[o];
      if (fo.mode == FinalMode::kKeep && o == PlanetManagement::kResearch) {
        // Research kept: on all blue squares, nowhere else.
        ask[o] = static_cast<std::int32_t>(
            std::ranges::count_if(free_, [&](std::uint16_t i) { return (base_[i].color & 7u) == kColour[o]; }));
      } else if (fo.mode == FinalMode::kKeep) {
        ask[o] = kept_[o];
      } else if (fo.mode == FinalMode::kAtLeast) {
        // At most half the squares while another output is maximised (a
        // small planet's minimum would take them all).
        ask[o] = squares_for(o, fo.value, maximised ? (squares + 1) / 2 : squares);
      } else {
        weight[o] = std::max<std::int32_t>(1, fo.weight);
      }
    }
    std::array<std::int32_t, PlanetManagement::kOutputs> got = share(ask, std::min(squares, sum(ask)));
    const std::int32_t left = squares - sum(got);
    const std::array<std::int32_t, PlanetManagement::kOutputs> more = share(weight, sum(weight) > 0 ? left : 0);
    for (std::size_t o = 0; o < got.size(); ++o) {
      got[o] += more[o];
    }
    // Lay it out and let the game count the points.
    std::vector<Square> layout = base_;
    std::vector<std::uint16_t> open = free_;
    for (std::size_t o = 0; o < got.size(); ++o) {
      place(layout, open, best_item(o), kColour[o], got[o]);
    }
    if (house != kNoItem) {
      place(layout, open, house, kWhite, housing);
    }
    const Planet result = points(layout);
    for (std::size_t o = 0; o < got.size(); ++o) {
      out.items[o] = best_item(o);
      out.squares[o] = static_cast<std::int16_t>(got[o]);
    }
    out.house = house;
    out.points = {static_cast<std::int16_t>(plan_detail::u16(result.industry)),
                  static_cast<std::int16_t>(plan_detail::u16(result.research)),
                  static_cast<std::int16_t>(plan_detail::u16(result.prosperity))};
    out.housing = static_cast<std::int16_t>(housing);
    out.other = static_cast<std::int16_t>(
        other_extra_ + std::count_if(base_.begin(), base_.begin() + planet_.surface_squares,
                                     [](const Square& sq) { return sq.item != kNoItem; }));
    out.open = static_cast<std::int16_t>(squares - sum(got));
    return out;
  }

  const PlanItem& item(std::uint8_t i) const { return session_.tables->planitems.items.at(i); }

  // How little a square is worth to the outputs: black, white, colours.
  std::int32_t rank(std::uint16_t square) const {
    const std::uint8_t c = base_[square].color & 7u;
    return c == kBlack ? 0 : c == kWhite ? 1 : 2;
  }

  // The known building with the most of `output` (without housing).
  std::uint8_t best_item(std::size_t output) const {
    std::uint8_t best = kNoItem;
    for (const std::uint8_t i : kOutputItems) {
      const PlanItem& it = item(i);
      if (output_of(it, output) > 0 && race_knows_item(session_, planet_, i) &&
          (best == kNoItem || output_of(it, output) > output_of(item(best), output))) {
        best = i;
      }
    }
    return best;
  }
  // The room for workers `house` adds (its own worker taken).
  std::int32_t net_room(std::uint8_t house) const {
    const PlanItem& h = item(house);
    return h.max_population - ((h.flags & kItemNoPopulation) == 0 ? h.used_population : 0);
  }
  // The points of the maximised outputs by their weights (the others count
  // a little, to tell equal layouts apart).
  double value(const FinalLayout& layout) const {
    double v = 0.0;
    for (std::size_t o = 0; o < layout.points.size(); ++o) {
      const FinalOutput& fo = m_.final_outputs[o];
      v += layout.points[o] * (fo.mode == FinalMode::kMaximise ? std::max<std::int32_t>(1, fo.weight) : 0.01);
    }
    return v;
  }

  // Puts `n` of `it` on the open squares: of `colour` first, then white,
  // then any.
  static void place(std::vector<Square>& layout, std::vector<std::uint16_t>& open, std::uint8_t it, std::uint8_t colour,
                    std::int32_t n) {
    if (it == kNoItem) {
      return;
    }
    const auto order = [&](std::uint16_t sq) {
      const std::uint8_t c = layout[sq].color & 7u;
      return c == colour ? 0 : c == kWhite ? 1 : 2;
    };
    std::ranges::stable_sort(open, [&](std::uint16_t a, std::uint16_t b) { return order(a) < order(b); });
    const auto k = static_cast<std::ptrdiff_t>(std::min<std::size_t>(static_cast<std::size_t>(n), open.size()));
    for (auto at = open.begin(); at != open.begin() + k; ++at) {
      layout[*at] = Square{layout[*at].color, it, kSquareBuilt};
    }
    open.erase(open.begin(), open.begin() + k);
  }

  Planet points(const std::vector<Square>& layout) const {
    Planet p = planet_;
    planet_recalc_output(session_, p, layout);
    return p;
  }

  // The fewest squares (up to `most`) of `output`'s best building that
  // reach `value` points.
  std::int32_t squares_for(std::size_t output, std::int32_t value, std::int32_t most) const {
    const std::uint8_t it = best_item(output);
    if (value <= 0 || it == kNoItem) {
      return 0;
    }
    for (std::int32_t n = 1; n <= most; ++n) {
      std::vector<Square> layout = base_;
      std::vector<std::uint16_t> open = free_;
      place(layout, open, it, kColour[output], n);
      const Planet p = points(layout);
      const std::int32_t got = plan_detail::u16(output == PlanetManagement::kIndustry   ? p.industry
                                                : output == PlanetManagement::kResearch ? p.research
                                                                                        : p.prosperity);
      if (got >= value) {
        return n;
      }
    }
    return most;
  }

  static std::int32_t sum(const std::array<std::int32_t, PlanetManagement::kOutputs>& a) {
    return std::accumulate(a.begin(), a.end(), 0);
  }
  // `total` shared by `parts` (largest remainder first).
  static std::array<std::int32_t, PlanetManagement::kOutputs> share(
      const std::array<std::int32_t, PlanetManagement::kOutputs>& parts, std::int32_t total) {
    std::array<std::int32_t, PlanetManagement::kOutputs> out{};
    const std::int32_t all = sum(parts);
    if (all <= 0 || total <= 0) {
      return out;
    }
    std::array<std::int32_t, PlanetManagement::kOutputs> rest{};
    std::int32_t given = 0;
    for (std::size_t o = 0; o < out.size(); ++o) {
      out[o] = parts[o] * total / all;
      rest[o] = parts[o] * total % all;
      given += out[o];
    }
    while (given < total) {
      const auto at = static_cast<std::size_t>(std::ranges::max_element(rest) - rest.begin());
      ++out[at];
      rest[at] = -1;
      ++given;
    }
    return out;
  }

  const Session& session_;
  const Planet& planet_;
  const PlanetManagement& m_;
  std::vector<Square> base_;           // the squares without the buildings laid out
  std::vector<std::uint16_t> free_;    // the squares to lay out
  std::array<std::int32_t, PlanetManagement::kOutputs> kept_{};  // squares of each output now
  std::int32_t cap_ = 0;               // room of the planet and the buildings staying
  std::int32_t workers_ = 0;           // their workers (and the defences to come)
  std::int32_t other_extra_ = 0;       // squares of the defences to come
};

}  // namespace

FinalLayout planet_final_layout(const Session& session, const Planet& planet, const PlanetManagement& management) {
  return Layout(session, planet, management).run();
}

std::optional<PlanetManagement::Output> planet_final_output(std::uint8_t item) {
  // Per output its plain building and its large one (`kOutputItems`).
  constexpr std::array<PlanetManagement::Output, kOutputItems.size()> kOf{
      PlanetManagement::kIndustry,   PlanetManagement::kProsperity, PlanetManagement::kResearch,
      PlanetManagement::kIndustry,   PlanetManagement::kProsperity, PlanetManagement::kResearch};
  const auto at = std::ranges::find(kOutputItems, item);
  return at == kOutputItems.end() ? std::nullopt : std::optional(kOf[static_cast<std::size_t>(at - kOutputItems.begin())]);
}

bool planet_final_housing(std::uint8_t item) { return std::ranges::find(kHousingItems, item) != kHousingItems.end(); }

}  // namespace oa::game
