// The runtime addresses the original stores in the battle scratch fields
// (`Ship::action_slot`, `Ship::target`, `Planet::fire_square`,
// `Planet::fire_target`), which the saves write as they are: the cosmos
// object's address under DOS/4GW (by variant, `game/save.h`) plus the
// record's offset. The day code computes them exactly so that the saves stay
// byte exact, and resolves them back to records.
#pragma once

#include <cstdint>
#include <optional>

#include "game/save.h"
#include "game/session.h"
#include "game/state.h"

namespace oa::game {

using ShipIndex = std::int16_t;
using RaceIndex = std::uint8_t;

// Offsets and record sizes of the cosmos arrays (`docs/04-structures.md`).
inline constexpr RawAddress kRacesOffset = 0x6;
inline constexpr RawAddress kRaceSize = 0x1ee;
inline constexpr RawAddress kStarsOffset = 0xdb5;
inline constexpr RawAddress kStarSize = 0x60;
inline constexpr RawAddress kLanesOffset = 0x3337;
inline constexpr RawAddress kLaneSize = 0x27;
inline constexpr RawAddress kSquaresOffset = 0x49c5;
inline constexpr RawAddress kSquareSize = 4;
inline constexpr RawAddress kPlanetsOffset = 0x18247;
inline constexpr RawAddress kPlanetSize = 0x7b;
inline constexpr RawAddress kShipsOffset = 0x27285;
inline constexpr RawAddress kShipSize = 0x162;
inline constexpr RawAddress kShipSlotsOffset = 0xab;
inline constexpr RawAddress kGizmoSlotSize = 7;

// Returns the cosmos object's runtime address of the session.
inline RawAddress cosmos_address(const Session& session) {
  if (session.cosmos_address != 0) {
    return session.cosmos_address;
  }
  return antagonizer_rules(session.game->variant) ? kAntagCosmosAddress : kPatchCosmosAddress;
}

// Returns the index of a record of the cosmos arrays.
inline ShipIndex ship_index(const Cosmos& cosmos, const Ship& ship) {
  return static_cast<ShipIndex>(&ship - cosmos.ships.data());
}
inline PlanetIndex planet_index(const Cosmos& cosmos, const Planet& planet) {
  return static_cast<PlanetIndex>(&planet - cosmos.planets.data());
}
inline StarIndex star_index(const Cosmos& cosmos, const Star& star) {
  return static_cast<StarIndex>(&star - cosmos.stars.data());
}
inline LaneIndex lane_index(const Cosmos& cosmos, const Lane& lane) {
  return static_cast<LaneIndex>(&lane - cosmos.lanes.data());
}

// Returns the runtime address of a record.
inline RawAddress ship_address(const Session& s, ShipIndex i) {
  return cosmos_address(s) + kShipsOffset + static_cast<RawAddress>(i) * kShipSize;
}
inline RawAddress gizmo_slot_address(const Session& s, ShipIndex ship, std::int32_t slot) {
  return ship_address(s, ship) + kShipSlotsOffset + static_cast<RawAddress>(slot) * kGizmoSlotSize;
}
inline RawAddress planet_address(const Session& s, PlanetIndex i) {
  return cosmos_address(s) + kPlanetsOffset + static_cast<RawAddress>(i) * kPlanetSize;
}
inline RawAddress star_address(const Session& s, StarIndex i) {
  return cosmos_address(s) + kStarsOffset + static_cast<RawAddress>(i) * kStarSize;
}
inline RawAddress lane_address(const Session& s, LaneIndex i) {
  return cosmos_address(s) + kLanesOffset + static_cast<RawAddress>(i) * kLaneSize;
}
inline RawAddress square_address(const Session& s, std::int32_t i) {
  return cosmos_address(s) + kSquaresOffset + static_cast<RawAddress>(i) * kSquareSize;
}

// Returns the raw pointer the ship's order holds (+0x5e): its star (order
// 1) or planet (2, 3), 0 for a point in space (7), the stale pointer for no
// order (0); 0 for the battle-only orders 4..6 (never given).
inline RawAddress destination_address(const Session& s, const Ship& ship) {
  switch (ship.destination_kind) {
    case ShipOrder::kNothing:
      return ship.stale_destination;
    case ShipOrder::kGoStar:
      return star_address(s, ship.destination);
    case ShipOrder::kColonize:
    case ShipOrder::kOrbitPlanet:
      return planet_address(s, ship.destination);
    default:
      return 0;
  }
}

// Drops the ship's order by clearing its code only, as `Race_battle_turn`
// 0x3c6d8 (an order fulfilled) and the System Display's actions
// (`BatControlWnd_handle` 0x10b24) do: the pointer stays (`Ship`
// `stale_destination`).
inline void ship_clear_order_code(const Session& s, Ship& ship) {
  ship.stale_destination = destination_address(s, ship);
  ship.destination_kind = ShipOrder::kNothing;
}

// Returns the index of the record at `raw` in an array (`offset`, `size`,
// `count`), nothing when `raw` is not the start of one of its records.
inline std::optional<std::int32_t> record_at(const Session& s, RawAddress raw, RawAddress offset, RawAddress size,
                                             std::size_t count) {
  const RawAddress base = cosmos_address(s) + offset;
  if (raw < base || (raw - base) % size != 0 || (raw - base) / size >= count) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>((raw - base) / size);
}
inline std::optional<std::int32_t> ship_at(const Session& s, RawAddress raw) {
  return record_at(s, raw, kShipsOffset, kShipSize, kMaxShips);
}
inline std::optional<std::int32_t> planet_at(const Session& s, RawAddress raw) {
  return record_at(s, raw, kPlanetsOffset, kPlanetSize, kMaxPlanets);
}
inline std::optional<std::int32_t> lane_at(const Session& s, RawAddress raw) {
  return record_at(s, raw, kLanesOffset, kLaneSize, kMaxLanes);
}
inline std::optional<std::int32_t> square_at(const Session& s, RawAddress raw) {
  return record_at(s, raw, kSquaresOffset, kSquareSize, kMaxSquares);
}
// Returns the slot number of the gizmo slot of ship `ship` at `raw`.
inline std::optional<std::int32_t> gizmo_slot_at(const Session& s, ShipIndex ship, RawAddress raw) {
  const RawAddress base = ship_address(s, ship) + kShipSlotsOffset;
  if (raw < base || (raw - base) % kGizmoSlotSize != 0 || (raw - base) / kGizmoSlotSize >= kShipSlots) {
    return std::nullopt;
  }
  return static_cast<std::int32_t>((raw - base) / kGizmoSlotSize);
}

}  // namespace oa::game
