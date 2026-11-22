// peCustom's final build-out of a self-managed planet: the layout its
// squares take once the planet is full (`PlanetManagement::final_outputs`),
// shown by the management window and aimed at by the rules
// (`planet_plan_rules.cpp`).
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// The final layout of a planet: per output (industry, research,
// prosperity) its squares and points, the squares of housing, of the other
// buildings (defences, specials, the Colony Base) and those left open (no
// output maximised).
struct FinalLayout {
  // Per output its building (`kNoItem`: none known) and squares.
  std::array<std::uint8_t, PlanetManagement::kOutputs> items{kNoItem, kNoItem, kNoItem};
  std::array<std::int16_t, PlanetManagement::kOutputs> squares{};
  std::array<std::int16_t, PlanetManagement::kOutputs> points{};
  std::uint8_t house = kNoItem;  // the housing (`kNoItem`: none known)
  std::int16_t housing = 0;
  std::int16_t other = 0;
  std::int16_t open = 0;
};

// The final layout of `planet` with `management` (resolved) and the
// buildings its owner knows: the defences it is set to have stay, housing
// enough for the workers of all squares, the minima first (shortened alike
// when the squares do not suffice for them), then the squares left by the
// weights of the maximised outputs; each output's best building on squares
// of its colour first. The points are the game's (colour bonus, output
// curve, planet-wide boosts already built).
FinalLayout planet_final_layout(const Session& session, const Planet& planet, const PlanetManagement& management);

// The output whose squares the final layout lays out `item` on (none: not
// laid out, or housing).
std::optional<PlanetManagement::Output> planet_final_output(std::uint8_t item);
// Whether the final layout counts `item` as housing.
bool planet_final_housing(std::uint8_t item);

}  // namespace oa::game
