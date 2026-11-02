// The saved games (`NN.SAV`, `RESUME.GAM`; `docs/02-data-formats.md`,
// `tools/sav.py`), in two steps:
//
// - `parse_save` / `serialize_save` convert between the file and a
//   `SaveImage`, the records exactly as written (counts, stale bytes, the
//   raw pointers resolved to indices with the writer's cosmos address). The
//   conversion is lossless (`serialize_save(parse_save(f)) == f`) but for
//   the stale list slots pointing into another process's cosmos (below).
// - `save_game` / `load_game` move a `SaveImage` from and to a session as
//   `Game_save` 0x4f458 / `Game_load` 0x4f3c0 (`Cosmos_save` 0x20a94,
//   `Cosmos_load` 0x211c0) do, with the original's load semantics: star
//   lane/planet lists rebuilt, AI target counts zeroed, ship slots freed
//   first, clusters left as they were (only their count is read).
//
// The variant is an extension: a trailer after the window state (`OAvt`,
// `u32` variant), written only for a non-Standard game, so a Standard save is
// exactly the original format. `load_game` loads an original file (no
// trailer) as Antagonizer when its cosmos address is `ANTAG.EXE`'s, else as
// Standard.
//
// A peCustom save follows the variant with its own format version (`u32`,
// `kPeCustomSaveVersion`), so that later versions can append data and still
// load the saves of the earlier ones: data is only ever appended, and a save
// of an older version loads with that data's defaults.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "game/session.h"
#include "game/state.h"

namespace oa::game {

// Runtime addresses of the cosmos object under DOS/4GW, which the raw
// pointers of the saves hold: `PATCH.EXE` (image 0x92df8 + 0x182000, the
// shipped and the oracle's saves) and `ANTAG.EXE` (from its saves' square
// pointers). A save of an Antagonizer game uses the latter.
inline constexpr RawAddress kPatchCosmosAddress = 0x214df8;
inline constexpr RawAddress kAntagCosmosAddress = 0x219f6c;

// The format version of the peCustom saves this build writes, the newest it
// loads. Raise it when appending data to the peCustom trailer.
// 2: `Game::last_attack_days`.
// 3: `Game::planet_goals` (a byte per planet).
inline constexpr std::uint32_t kPeCustomSaveVersion = 3;

// Size of the window state of the v1.6.5 window table (`COSMOSWnd` 0xbe,
// 4 values, a 7-entry list box, 1 value, `BatModeWnd` 0x1a08, 2 values, the
// planet window byte, the research window camera 0x98).
inline constexpr std::size_t kWindowStateSize = 0xbe + 16 + 7 * 14 + 4 + 0x1a08 + 8 + 1 + 0x98;

// A save file's records, as written.
struct SaveImage {
  SaveHeader header;
  // Runtime address of the writer's cosmos object: planet 0's square
  // pointer minus the squares' offset (by variant when saved).
  RawAddress cosmos_address = kPatchCosmosAddress;
  std::int32_t day = 0;
  StarIndex selected_star = 0;
  PlanetIndex selected_planet = 0;
  std::uint8_t ending = 0;
  std::int32_t score = 0;
  std::array<std::int32_t, 3> last_trade_days{};
  Research research;
  std::vector<Star> stars;
  std::vector<Square> squares;
  std::vector<Planet> planets;
  std::vector<std::pair<std::int32_t, Ship>> ships;  // used slots: slot, record
  std::vector<Race> races;
  std::vector<Lane> lanes;
  std::array<std::array<std::uint8_t, kMaxStars>, kMaxStars> hops{};
  std::vector<Cluster> clusters;
  std::vector<Backdrop> backdrops;
  std::vector<std::uint8_t> window_state;  // kWindowStateSize bytes
  Variant variant = Variant::kStandard;    // the trailer's, Standard without
  // The peCustom format version read (`kPeCustomSaveVersion` when saved); 0
  // for the other variants.
  std::uint32_t pe_custom_version = 0;
  // peCustom version 2 on (never attacked before).
  RaceDays last_attack_days = no_attack_days();
  // peCustom version 3 on (balanced before): a goal per planet.
  std::vector<PlanetGoal> planet_goals;
};

// Parses a save file. Throws `DataError` on a record-size mismatch, a count
// or index out of range, a raw pointer that is not an element of its array,
// a truncated file or bytes after the window state that are not a variant
// trailer. Pointers that are not records of the writer's cosmos are
// resolved against `foreign_cosmos` when given: a build saving a game loaded
// from another build's file writes that file's stale pointers back (the
// ANTAG saves of a PATCH game); such a file then does not round-trip.
// The slots past their count of the stars' lane and planet lists and of
// the races' AI targets that still point nowhere are read as none: the
// tutorials' games (`<name>.tsv`) hold stale pointers of their author's
// process there, which the original writes back unchanged (the remake
// writes them as none or as its own addresses).
SaveImage parse_save(std::span<const std::uint8_t> data, std::optional<RawAddress> foreign_cosmos = std::nullopt);

// Returns the bytes of `image`. Throws `std::logic_error` for a state the
// original cannot write (a used ship in no location, a battle-only order).
std::vector<std::uint8_t> serialize_save(const SaveImage& image);

// Returns the save of the session's game (`Game_save`: the header computed
// from the game, the stale header bytes and the window state of the session).
SaveImage save_game(const Session& session);

// Loads `image` into the session's game (`Game_load`): the variant (that of
// `ANTAG.EXE` for its trailer-less files, see above), the window state and
// the stale header bytes go to the session too; the player's race is not
// read (the header is ignored).
void load_game(Session& session, const SaveImage& image);

}  // namespace oa::game
