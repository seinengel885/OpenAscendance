// The game tables read from the install's text files (`ASCEND00.COB`):
// planet items, gizmos, the research tree, the star names and the species'
// `.dip` values (`docs/02-data-formats.md`). They are read once and never
// saved, except the research tree, which the first `Research_load` copies
// into the saved research state (`game/state.h`).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "assets/cfile.h"
#include "game/state.h"

namespace oa::game {

// Planet item flag bits (`planitem.txt` flag numbers; tentative names).
enum PlanItemFlag : std::uint16_t {
  kItemAnySquare = 1u << 0,     // may go on black squares, no adjacency
  kItemWholePlanet = 1u << 1,   // no square (planet-wide project)
  kItemWeapon = 1u << 2,        // planetary weapon
  kItemShield = 1u << 3,        // planetary shield
  kItemContinuous = 1u << 5,    // continuous project
  kItemNoPopulation = 1u << 6,  // uses no population (Outpost, Transport Tubes)
};

// Planet item ids met in the generation code.
inline constexpr std::uint8_t kItemColonyBase = 5;
inline constexpr std::uint8_t kItemHyperpowerPlant = 12;
inline constexpr std::uint8_t kItemFertilizationPlant = 13;
inline constexpr std::uint8_t kItemInternet = 14;
inline constexpr std::uint8_t kItemTractorBeam = 17;
inline constexpr std::uint8_t kItemShipyard = 22;
inline constexpr std::uint8_t kItemShip = 23;
inline constexpr std::uint8_t kItemMissileBase = 28;
inline constexpr std::uint8_t kItemWhopper = 29;
inline constexpr std::uint8_t kItemMegaWhopper = 30;
inline constexpr std::uint8_t kItemEndlessParty = 33;
inline constexpr std::uint8_t kItemScientistTakeover = 34;
inline constexpr std::uint8_t kItemAutomation = 35;
inline constexpr std::uint8_t kItemTerraforming = 36;
inline constexpr std::uint8_t kItemXenoDig = 38;

// One `planitem.txt` record (`planitem_rec`, 0x28 bytes at 0xf3e28).
struct PlanItem {
  std::string name;                       // +0x0a, '^' replaced by ' '
  std::int8_t industry = 0;               // +0x00 per day
  std::int8_t research = 0;               // +0x01
  std::int8_t prosperity = 0;             // +0x02
  std::int8_t max_population = 0;         // +0x03 added to the planet's
  std::int8_t used_population = 0;        // +0x04 population needed
  std::uint8_t research_requirement = 0;  // +0x05 255 none, 254 never
  std::uint16_t flags = 0;                // +0x06 `PlanItemFlag`
  std::uint16_t cost = 0;                 // +0x08 industry
};

// `planitem.txt` as `PlanItem_load_table` 0x33450 stores it.
struct PlanItemTable {
  static constexpr std::size_t kItems = 39;
  static constexpr std::size_t kTypes = 11;
  static constexpr std::size_t kSizes = 5;
  static constexpr std::size_t kColumns = 15;

  std::int32_t pop_growth = 0;  // 0x866cc, growth needed per population
  // Per planet type: % of black, red, green, blue surface squares (0xf44dc).
  std::array<std::array<std::uint8_t, 4>, kTypes> type_weights{};
  // Per size and surface column: first row (0xf4444) and height (0xf4490).
  std::array<std::array<std::uint8_t, kColumns>, kSizes> column_start{};
  std::array<std::array<std::uint8_t, kColumns>, kSizes> column_height{};
  std::uint16_t surface_count = 0;  // 0xf3e24: items [0, n) are surface items
  std::uint16_t orbital_count = 0;  // 0xf3e26
  std::array<PlanItem, kItems> items{};
};

// One `gizmos.txt` record (`asc_gizmo`, 0x6a bytes at 0xf64f4).
struct Gizmo {
  std::string name;                       // +0x00, '^' replaced by ' '
  std::uint8_t category = 0;              // +0x32 0 weapon .. 5 special
  std::uint16_t flags = 0;                // +0x33 GZ_* bits
  std::uint8_t research_requirement = 0;  // +0x35 255 none
  std::int32_t power = 0;                 // +0x36
  std::int32_t range = 0;                 // +0x3a
  std::int32_t level = 0;                 // +0x3e
  std::int32_t uses = 0;                  // +0x42 per day, 0 unlimited
  std::int32_t industry = 0;              // +0x46
  std::int32_t special1 = 0;              // +0x4a
  std::int32_t special2 = 0;              // +0x4e
  // Copies by category (0 for the other categories).
  std::int32_t weapon_damage = 0;    // +0x52 weapons: level
  std::int32_t shield_strength = 0;  // +0x56 shields: level
  std::int32_t drive_speed = 0;      // +0x5e drives: level
  std::int32_t scanner_level = 0;    // +0x62 scanners: level
  std::int32_t generator_power = 0;  // +0x66 generators: power
};

inline constexpr std::size_t kGizmoCount = 76;

// The research tree as the first `Research_load` 0x41ce0 leaves it in the
// (zeroed) research global: item count and records, masks clear.
struct ResearchTree {
  std::uint16_t count = 0;
  std::array<ResearchItem, kMaxResearchItems> items{};
};

// The 15 `DIPVALUES` of a `RACExx.DIP` file, in file order, as
// `Race_load_dip_values` 0x3f700 stores them (`int` read, `short` kept; the
// three `BIO_SIN` values become floats of that short).
struct DipValues {
  std::int16_t declare_war = 0;
  std::int16_t accept_peace = 0;
  std::int16_t propose_peace = 0;
  std::int16_t break_alliance = 0;
  std::int16_t accept_alliance = 0;
  std::int16_t propose_alliance = 0;
  std::int16_t start_attitude = 0;
  std::int16_t ship_patience = 0;
  std::int16_t bio_period = 0;
  std::int16_t bio_max_positive = 0;
  std::int16_t bio_max_negative = 0;
  float bio_sin_x = 0;
  float bio_sin_half_x = 0;
  float bio_sin_twice_x = 0;
  std::int16_t neg_player_turns = 0;
};

// A star name as `Cosmos_generate_stars` reads it (at most 15 characters:
// it is copied into the 16-byte `Star::name`).
using StarName = std::string;
inline constexpr std::size_t kStarNameCount = 100;

// The hull names (`static.txt` strings 131..134, copied to 0xf846c by the
// first `Ship_init` 0x4470c): Small, Medium, Large, Enormous.
inline constexpr std::size_t kHullCount = 4;
inline constexpr std::size_t kHullNameString = 131;

// Every table the game state needs.
struct GameTables {
  PlanItemTable planitems;
  std::array<Gizmo, kGizmoCount> gizmos;
  ResearchTree research;
  std::array<StarName, kStarNameCount> star_names;
  std::array<DipValues, kSpeciesCount> dip;  // by species
  std::array<std::string, kHullCount> hull_names;
  // `data\logo.gif`, which `main` 0x1c424 loads into the shared work buffer
  // at startup (`Session::scratch`).
  std::vector<std::uint8_t> logo_gif;
};

// Sets the name of `item` to `name`; throws `DataError` when it does not fit.
void set_research_name(ResearchItem& item, std::string_view name);

// The parsers take the whole file; each throws `DataError` on a malformed
// or truncated file.

// `planitem.txt` (`PlanItem_load_table` 0x33450).
PlanItemTable parse_planitems(std::span<const std::uint8_t> data);
// `gizmos.txt` (`Ship_load_gizmos` 0x444e4): exactly 76 records.
std::array<Gizmo, kGizmoCount> parse_gizmos(std::span<const std::uint8_t> data);
// `restree.txt` (`Research_load` 0x41ce0, first call).
ResearchTree parse_restree(std::span<const std::uint8_t> data);
// `names.txt` (`Cosmos_generate_stars` 0x1dd08): 100 `%s` tokens, the ones
// past the end of the file named `XM%02d` after their index.
std::array<StarName, kStarNameCount> parse_star_names(std::span<const std::uint8_t> data);
// A `RACExx.DIP` file (`Race_load_dip_values` 0x3f700); the six attitude
// thresholds must increase.
DipValues parse_dip_values(std::span<const std::uint8_t> data);

// Reads every table from the install (`RACE00.DIP`..`RACE20.DIP` by species,
// the hull names from `static.txt`), the names in the files' language.
GameTables load_game_tables(const GameFiles& files);

}  // namespace oa::game
