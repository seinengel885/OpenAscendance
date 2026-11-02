// The game state: the original's `Cosmos` object (global 0x92df8, 0x354ef
// bytes, `cosmos.cpp`), the research state (global 0xf472c) and the few
// globals saved with them (`docs/04-structures.md`, `recon/structures.h`).
//
// Representation: each record keeps the original's fields, widths and
// meanings, field by field in the original order (the comments give the
// packed offsets), but pointers into the cosmos arrays are held as array
// indices (`kNone` for a null pointer). The state is thus position
// independent and plainly copyable; `game/save.h` converts the indices to and
// from the runtime addresses the original writes, which keeps saves byte
// exact. Battle-only scratch pointers, whose target type varies, stay raw
// runtime addresses (`RawAddress`). Bytes the original leaves stale (name
// tails, list entries past their count, unused fields) are kept, since the
// original writes them to the saves.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "game/float_math.h"
#include "game/variant.h"

namespace oa::game {

inline constexpr std::size_t kMaxRaces = 7;
inline constexpr std::size_t kSpeciesCount = 21;
inline constexpr std::size_t kMaxStars = 100;
inline constexpr std::size_t kMaxLanes = 148;
inline constexpr std::size_t kMaxSquares = 20000;
inline constexpr std::size_t kMaxPlanets = 500;
inline constexpr std::size_t kMaxShips = 107;
inline constexpr std::size_t kMaxBackdrops = 9;
inline constexpr std::size_t kMaxClusters = 24;
inline constexpr std::size_t kMaxResearchItems = 100;
inline constexpr std::size_t kLanesPerStar = 6;
inline constexpr std::size_t kPlanetsPerStar = 5;
inline constexpr std::size_t kShipSlots = 25;
inline constexpr std::uint16_t kOrbitalSquares = 10;

// Indices standing for the original's pointers into the cosmos arrays.
using StarIndex = std::int16_t;
using LaneIndex = std::int16_t;
using PlanetIndex = std::int16_t;
using SquareIndex = std::int16_t;
inline constexpr std::int16_t kNone = -1;
// A pointer kept as the runtime address the original holds (0 = null).
using RawAddress = std::uint32_t;

// Returns an array of `N` null indices.
template <std::size_t N>
constexpr std::array<std::int16_t, N> null_indices() {
  std::array<std::int16_t, N> a{};
  a.fill(kNone);
  return a;
}

// Returns the text of a fixed NUL-padded name field (up to the first NUL).
template <std::size_t N>
std::string_view name_view(const std::array<char, N>& name) {
  std::size_t n = 0;
  while (n < N && name[n] != '\0') {
    ++n;
  }
  return {name.data(), n};
}

// Star system (0x60 bytes, `Star_serialize` 0x1d920).
struct Star {
  std::int32_t type = 0;              // +0x00 0..12 (`starstat.txt` order)
  std::int16_t index = 0;             // +0x04 own index
  std::int16_t cluster = 0;           // +0x06 lane-generator component, -1 none
  Vec3 position{};                    // +0x08
  std::uint8_t owner_mask = 0;        // +0x14 races with a colony; 0x80 home
  std::uint8_t ships_mask = 0;        // +0x15 races with ships here
  std::uint8_t lanes_known_mask = 0;  // +0x16 races knowing a lane ending here
  std::uint8_t explored_mask = 0;     // +0x17 races that know the system
  std::uint32_t flags = 0;            // +0x18 bit 0: red-link endpoint
  std::array<char, 16> name{};        // +0x1c
  std::array<LaneIndex, kLanesPerStar> lanes = null_indices<kLanesPerStar>();          // +0x2c
  std::int16_t lane_count = 0;                                                         // +0x44
  std::array<PlanetIndex, kPlanetsPerStar> planets = null_indices<kPlanetsPerStar>();  // +0x46 by orbit
  std::int16_t planet_count = 0;                                                       // +0x5a
  std::int32_t battle_done = 0;  // +0x5c -1 once the day's combat is done
};

// Star lane (0x27 bytes).
struct Lane {
  StarIndex star_a = kNone;     // +0x00
  StarIndex star_b = kNone;     // +0x04
  Vec3 end_a{};                 // +0x08 lane end offset from star A
  Vec3 end_b{};                 // +0x14 = -end_a
  std::uint8_t known_mask = 0;  // +0x20 races knowing the lane
  std::int16_t visited = 0;     // +0x21 daily flood-fill mark
  std::uint32_t flags = 0;      // +0x23 bit 0 red link, bit 1 blocked
};

// Planet surface square (4 bytes).
struct Square {
  std::uint8_t color = 0;   // bits 0-2 colour (0 black, 1 white, 2 red, 3
                            // green, 4 blue), bit 3 xeno ruins
  std::uint8_t item = 0;    // planitem index, 0xff empty
  std::uint16_t flags = 0;  // bit 0 built, bit 1 automated, bits 8-15 value
};
inline constexpr std::uint8_t kSquareRuins = 8;
inline constexpr std::uint8_t kNoItem = 0xff;
inline constexpr std::uint16_t kSquareBuilt = 1;
inline constexpr std::uint16_t kSquareAutomated = 2;

// Planet (0x7b bytes, `Planet_serialize` 0x359e8).
struct Planet {
  Vec3 position{};                    // +0x00 relative to the star
  StarIndex star = 0;                 // +0x0c
  std::int16_t orbit = 0;             // +0x0e 0..4
  SquareIndex first_square = kNone;   // +0x10 pointer into the squares
  std::uint16_t size = 0;             // +0x14 0..4 Tiny..Enormous
  std::uint16_t type = 0;             // +0x16 0..10 Husk..Cornucopia
  std::uint16_t surface_squares = 0;  // +0x18
  std::uint16_t square_count = 0;     // +0x1a surface + 10 orbital
  std::uint16_t free_surface = 0;     // +0x1c
  std::uint16_t free_orbital = 0;     // +0x1e
  std::uint16_t free_black = 0;       // +0x20
  std::uint16_t unknown_22 = 0;       // +0x22 never written
  std::array<char, 30> name{};        // +0x24
  std::uint16_t population = 0;       // +0x42
  std::int16_t industry = 0;          // +0x44 per day
  std::int16_t research = 0;          // +0x46
  std::int16_t prosperity = 0;        // +0x48
  std::uint16_t max_population = 0;   // +0x4a
  std::uint16_t used_population = 0;  // +0x4c
  std::uint16_t growth = 0;           // +0x4e
  std::uint16_t progress = 0;         // +0x50 industry spent on the project
  std::uint16_t project_square = 0;   // +0x52 0xffff none / planet-wide
  std::uint8_t project_item = 0;      // +0x54 0xff none
  std::uint16_t base_population = 0;  // +0x55
  std::uint8_t owner = 0;             // +0x57 race slot, 0xff none
  std::uint8_t weapon_count = 0;      // +0x58
  std::uint8_t shield_count = 0;      // +0x59
  std::int32_t self_managed = 0;      // +0x5a 0 / -1
  std::int32_t unknown_5e = 0;        // +0x5e cleared on generation
  std::int32_t protected_turn = 0;    // +0x62 -1 bombardment skipped
  std::uint8_t flags = 0;             // +0x66 bit 0 growth bomb, bit 1 ruins
  std::int32_t acquired_day = 0;      // +0x67
  RawAddress fire_square = 0;         // +0x6b battle scratch
  RawAddress fire_target = 0;         // +0x6f battle scratch
  std::uint32_t fire_result = 0;      // +0x73 battle scratch
  std::int32_t ship_cost = 0;         // +0x77 design being built here
};
inline constexpr std::uint8_t kNoOwner = 0xff;
inline constexpr std::uint8_t kPlanetRuins = 2;

// Ship gizmo slot (7 bytes).
struct GizmoSlot {
  std::int8_t gizmo = 0;       // +0 gizmos.txt index, -1 empty
  std::int16_t uses_left = 0;  // +1
  std::int32_t toggled = 0;    // +3 0 / -1
};

// Ship location kinds (`Ship::location_kind`, +0x58) and the array the
// location indexes.
enum class ShipLocation : std::uint8_t {
  kNone = 0,
  kBuilding = 1,  // planet
  kRefit = 2,     // planet
  kOrbit = 3,     // planet
  kSystem = 4,    // star
  kLane = 5,      // lane
};
// Ship standing orders (`Ship::destination_kind`, +0x5d, names 0x86298).
enum class ShipOrder : std::uint8_t {
  kNothing = 0,
  kGoStar = 1,        // star
  kColonize = 2,      // planet
  kOrbitPlanet = 3,   // planet
  kPatrol = 4,        // battle only (not saveable)
  kDefendPlanet = 5,  // battle only
  kAttack = 6,        // battle only
  kGoSpace = 7,       // a point (no index; written as 0)
};

// Ship (0x162 bytes, `Ship_serialize` 0x46294). Defaults: a free slot
// (`Cosmos_new_galaxy` and `Cosmos_load` free every slot before use;
// `Cosmos_alloc_ship` 0x1fda8 resets a slot it takes).
struct Ship {
  std::int32_t weapon_damage = 0;                    // +0x00 stats recomputed from the gizmos
  std::int32_t shield_max = 0;                       // +0x04
  std::int32_t lane_speed = 0;                       // +0x08
  std::int32_t hyper_speed = 0;                      // +0x0c
  std::int32_t drive_speed = 0;                      // +0x10
  std::int32_t drive_base = 0;                       // +0x14
  std::int32_t generator_power = 0;                  // +0x18
  std::int32_t drive_power = 0;                      // +0x1c
  std::int32_t scanner_level = 0;                    // +0x20
  std::int32_t cloakers = 0;                         // +0x24
  std::int32_t has_colonizer = 0;                    // +0x28 0 / -1
  std::array<std::int32_t, 2> reserved_2c{};         // +0x2c
  std::array<char, 30> name{};                       // +0x34
  std::int32_t built_day = 0;                        // +0x52
  std::int16_t owner = -1;                           // +0x56 race slot, -1 free slot
  ShipLocation location_kind = ShipLocation::kNone;  // +0x58
  std::int16_t location = kNone;                     // +0x59 index by kind
  ShipOrder destination_kind = ShipOrder::kNothing;  // +0x5d
  std::int16_t destination = kNone;                  // +0x5e index by kind
  std::uint8_t action = 0;                           // +0x62 battle: 0 move, 1 use gizmo
  RawAddress action_slot = 0;                        // +0x63 battle scratch (a GizmoSlot)
  std::uint8_t target_kind = 0;                      // +0x67 0 ship, 1 planet, 2 lane, 3 point, 5 none
  RawAddress target = 0;                             // +0x68 battle scratch
  Vec3 target_point{};                               // +0x6c
  Vec3 action_point{};                               // +0x78
  std::int32_t action_result = 0;                    // +0x84
  std::int32_t power = 0;                            // +0x88
  std::int32_t hit_points = 0;                       // +0x8c
  std::int32_t shield = 0;                           // +0x90
  std::int32_t moves = 0;                            // +0x94
  std::int32_t max_hit_points = 0;                   // +0x98
  std::uint16_t battle_flags = 0;                    // +0x9c
  Vec3 position{};                                   // +0x9e lane: distance, direction
  std::uint8_t hull_size = 0;                        // +0xaa 0..3
  std::array<GizmoSlot, kShipSlots> slots{};         // +0xab
  std::int32_t slot_count = 0;                       // +0x15a 5, 10, 15, 25
  std::int32_t gizmo_count = 0;                      // +0x15e
  // +0x5e as a raw pointer while `destination_kind` is 0 (or 7): what the
  // last order left there. Most ways of dropping an order clear only the
  // code (`ship_clear_order_code`), so the pointer of the order's star or
  // planet stays and the saves write it (`Ship_serialize` 0x46294, cleared
  // on load); the others write 0.
  RawAddress stale_destination = 0;
};

// Relation of a race toward another (`Race::relation`).
enum RaceRelation : std::uint8_t { kNotMet = 0, kPeace = 1, kWar = 2, kAlliance = 3 };
// Diplomatic action the race wants to propose to the player: none.
inline constexpr std::uint8_t kNoDipAction = 14;

// Race (0x1ee bytes, `Race_serialize` 0x3f660). The defaults are the
// constructed race (`Race_ctor` 0x39a50, `Race_reset` 0x39ab8).
struct Race {
  std::uint8_t index = 0xff;                                 // +0x000 slot
  std::uint8_t species = 0xff;                               // +0x001 0..20
  std::uint8_t color = 0;                                    // +0x002 0..6
  std::int32_t extinct = 0;                                  // +0x003 0 / -1
  StarIndex home = kNone;                                    // +0x007
  std::array<StarIndex, 100> targets = null_indices<100>();  // +0x00b AI target stars
  std::int16_t target_count = 0;                             // +0x19b
  std::uint8_t ability = 0;                                  // +0x19d = species
  std::int32_t ability_charge = 0;                           // +0x19e days since the last use
  std::array<std::uint8_t, 8> unused_1a2{};                  // +0x1a2
  std::int16_t ai_build_threshold_a = 18;                    // +0x1aa
  std::int16_t ai_build_threshold_b = 22;                    // +0x1ac
  std::int16_t ai_unknown_1ae = 0;                           // +0x1ae
  std::int16_t ship_patience = 0;                            // +0x1b0 DIP
  std::array<std::int16_t, kMaxRaces> attitude{};            // +0x1b2
  std::array<std::uint8_t, kMaxRaces> relation{};            // +0x1c0 RaceRelation
  std::int16_t neg_player_turns = 0;                         // +0x1c7 DIP values
  std::int16_t declare_war = 0;                              // +0x1c9
  std::int16_t accept_peace = 0;                             // +0x1cb
  std::int16_t propose_peace = 0;                            // +0x1cd
  std::int16_t break_alliance = 0;                           // +0x1cf
  std::int16_t accept_alliance = 0;                          // +0x1d1
  std::int16_t propose_alliance = 0;                         // +0x1d3
  std::int16_t start_attitude = 0;                           // +0x1d5
  std::int16_t bio_period = 0;                               // +0x1d7
  std::int16_t bio_max_positive = 0;                         // +0x1d9
  std::int16_t bio_max_negative = 0;                         // +0x1db
  float bio_sin_x = 0;                                       // +0x1dd
  float bio_sin_half_x = 0;                                  // +0x1e1
  float bio_sin_twice_x = 0;                                 // +0x1e5
  std::uint8_t pending_action = kNoDipAction;                // +0x1e9 DipAction
  RawAddress pending_action_arg = 0;                         // +0x1ea race index or star pointer
};

// Per-race ranking (6 bytes, cosmos+0xd8a, `Cosmos_rank_races` 0x2078c).
struct RaceRank {
  std::int16_t rank = 0;
  std::int16_t fleet = 0;
  std::int16_t colonies = 0;
};

// Lane-generator cluster (0x19e bytes).
struct Cluster {
  std::array<StarIndex, kMaxStars> stars = null_indices<kMaxStars>();  // +0x000
  std::int16_t star_count = 0;                                         // +0x190
  Vec3 centroid{};                                                     // +0x192
};

// Star-system backdrop sprite (0xd bytes).
struct Backdrop {
  std::uint8_t frame = 0;  // +0x0
  Vec3 position{};         // +0x1
};

// Atmosphere of the game (cosmos +0xdb4; not saved).
enum class Atmosphere : std::uint8_t { kPeaceful = 0, kNeutral = 1, kHostile = 2 };

// The `Cosmos` object (0x354ef bytes; cosmos.cpp). The defaults are the
// constructed object (`Cosmos_ctor` 0x1da04 on the zeroed global, then
// `Cosmos_init` 0x1dafc). Records past each count keep whatever the previous
// galaxies of the session left there.
struct Cosmos {
  std::int32_t day = 0;                             // +0x00000
  std::int16_t battle_cursor = 0;                   // +0x00004
  std::array<Race, kMaxRaces> races{};              // +0x00006
  std::int16_t race_count = 0;                      // +0x00d88
  std::array<RaceRank, kMaxRaces> ranks{};          // +0x00d8a
  Atmosphere atmosphere = Atmosphere::kNeutral;     // +0x00db4
  std::array<Star, kMaxStars> stars{};              // +0x00db5
  std::int16_t star_count = 0;                      // +0x03335
  std::array<Lane, kMaxLanes> lanes{};              // +0x03337
  std::int16_t lane_count = 0;                      // +0x049c3
  std::array<Square, kMaxSquares> squares{};        // +0x049c5
  std::int16_t square_count = 0;                    // +0x18245
  std::array<Planet, kMaxPlanets> planets{};        // +0x18247
  std::int16_t planet_count = 0;                    // +0x27283
  std::array<Ship, kMaxShips> ships{};              // +0x27285
  std::int16_t ship_count = 0;                      // +0x3067b used slots
  std::array<Backdrop, kMaxBackdrops> backdrops{};  // +0x3067d
  std::int16_t backdrop_count = 0;                  // +0x306f2
  StarIndex selected_star = kNone;                  // +0x306f4
  PlanetIndex selected_planet = kNone;              // +0x306f8
  std::int32_t score = 0;                           // +0x306fc final %, -1
  std::uint8_t ending = 0;                          // +0x30700 0, 1..5 end-*.txt
  std::array<Cluster, kMaxClusters> clusters{};     // +0x30701
  std::int16_t cluster_count = 0;                   // +0x32dd1
  std::int16_t mean_home_distance = 0;              // +0x32dd3
  std::int16_t mean_cluster_size = 0;               // +0x32dd5
  // +0x32ddb lane hops between stars (0 self, 0xff none); the "already
  // linked" matrix while the lanes are generated.
  std::array<std::array<std::uint8_t, kMaxStars>, kMaxStars> hops{};
  std::int32_t controlled_lanes = 0;  // +0x354eb
};

// Research item (0x4b bytes).
struct ResearchItem {
  std::array<char, 60> name{};             // +0x00 '^' replaced by ' '
  std::uint16_t cost = 0;                  // +0x3c
  std::array<std::uint8_t, 5> prereqs{};   // +0x3e 0xff-terminated
  std::array<std::uint8_t, 5> leads_to{};  // +0x43 0xff-terminated
  std::uint8_t known_mask = 0;             // +0x48
  std::uint8_t unseen_mask = 0;            // +0x49
  std::uint8_t category = 0;               // +0x4a
};

// Research state (0x1d72 bytes, global 0xf472c).
struct Research {
  std::uint16_t count = 0;                              // +0x0000
  std::array<ResearchItem, kMaxResearchItems> items{};  // +0x0002
  std::array<std::int16_t, kMaxRaces> project{};        // +0x1d4e -1 none
  std::array<std::uint16_t, kMaxRaces> progress{};      // +0x1d5c
  std::int32_t self_managed = 0;                        // +0x1d6a 0 / -1
  std::int32_t basics_done = 0;                         // +0x1d6e 0 / -1
};

// peCustom: a day per pair of races (`Game::last_attack_days`).
using RaceDays = std::array<std::array<std::int32_t, kMaxRaces>, kMaxRaces>;
inline constexpr std::int32_t kNoAttackDay = -100000;
constexpr std::array<std::int32_t, kMaxRaces> no_peace_offer_days() {
  std::array<std::int32_t, kMaxRaces> days{};
  days.fill(kNoAttackDay);
  return days;
}
constexpr RaceDays no_attack_days() {
  RaceDays days{};
  for (auto& row : days) {
    row.fill(kNoAttackDay);
  }
  return days;
}

// peCustom: what a self-managed planet of the player builds for
// (`Game::planet_goals`, `game/planet_plan.h`). The values are saved: never
// change them, append new goals. 0..5 were the goals of a former planner,
// never played: saves holding them load as balanced.
enum class PlanetGoal : std::uint8_t {
  kBalanced = 6,  // industry and research
  kIndustry = 7,  // industry (and so ships)
  kResearch = 8,  // research
};
// The first value of a goal, and the end of the values.
inline constexpr std::uint8_t kPlanetGoalFirst = 6;
inline constexpr std::uint8_t kPlanetGoalEnd = 9;

constexpr std::array<PlanetGoal, kMaxPlanets> balanced_goals() {
  std::array<PlanetGoal, kMaxPlanets> goals{};
  goals.fill(PlanetGoal::kBalanced);
  return goals;
}

// peCustom: how a managed planet treats a kind of building
// (`PlanetManagement::levels`). The values are saved: never change them.
enum class BuildLevel : std::uint8_t {
  kNo = 0,       // never
  kReserve = 1,  // room and workers kept, nothing built (orbital only)
  kAlong = 2,    // built as the planet grows
  kFirst = 3,    // built first (after housing and growth)
  kStandard = 0xff,  // the planet's standard (`planet_plan.h`)
};

// peCustom (save format 4): a planet's management beyond its strategy
// (`Game::planet_goals`), read by `planet_plan_v2.cpp`. -1 / kStandard: the
// standard for the planet (`planet_plan.h`).
struct PlanetManagement {
  enum Building : std::uint8_t { kPlanetaryDefence, kOrbitalDefence, kShipyard, kOrbitalDock, kDig, kBuildings };
  enum Count : std::uint8_t {
    kSurfaceShields,
    kObservation,
    kSurfaceCloaker,
    kTractorBeam,
    kOrbitalWeapons,
    kOrbitalCloaker,
    kOrbitalShields,  // save format 5 on
    kCounts
  };
  // The counts of save format 4.
  static constexpr std::size_t kCountsV4 = kOrbitalShields;
  std::array<BuildLevel, kBuildings> levels{BuildLevel::kStandard, BuildLevel::kStandard, BuildLevel::kStandard,
                                            BuildLevel::kStandard, BuildLevel::kStandard};
  // The defences of the planetary and orbital defence.
  std::array<std::int16_t, kCounts> counts{-1, -1, -1, -1, -1, -1, -1};
  std::int16_t min_industry = -1;  // kept when buildings make way
  std::int16_t min_research = -1;
  std::int16_t growth_days = -1;   // an inhabitant within these days (0 off)
  std::int8_t research_boost = -1;  // 0 / 1
  // The labels of `Game::planet_labels` the planet has (bit i: label i).
  std::uint32_t labels = 0;
  // Whether the planet's completion was reported (cleared when it builds
  // again).
  std::uint8_t reported = 0;
};
inline constexpr std::size_t kMaxPlanetLabels = 32;

// Everything a save holds about the game: the cosmos, the research state
// and the saved globals.
struct Game {
  Cosmos cosmos;
  Research research;
  // 0xf471c / 0xf4720 / 0xf4724: days of the player's last star-lane, star
  // and research trade proposals (-10 never).
  std::array<std::int32_t, 3> last_trade_days{};
  Variant variant = Variant::kStandard;
  // peCustom: [victim][aggressor] the day of the aggressor's last attack on
  // the victim or of their declaration of war (`kNoAttackDay` never).
  RaceDays last_attack_days = no_attack_days();
  // peCustom: per race the day the player last offered it peace (once a
  // day; `kNoAttackDay` never).
  std::array<std::int32_t, kMaxRaces> peace_offer_days = no_peace_offer_days();
  // peCustom: the goal of each planet's self-management (the player's
  // planets; the AI picks its own).
  std::array<PlanetGoal, kMaxPlanets> planet_goals = balanced_goals();
  // peCustom: the rest of each planet's management, and the player's planet
  // labels (at most `kMaxPlanetLabels`).
  std::array<PlanetManagement, kMaxPlanets> planet_management{};
  std::vector<std::string> planet_labels;
};

// The save header (0x74 bytes, `Game_save` 0x4f458): the load-list summary,
// ignored by `Game_load`. Per-race entries past `race_count` and `stale` are
// stale stack bytes.
struct SaveHeader {
  std::int32_t day = 0;                                    // +0x00
  std::int32_t race_count = 0;                             // +0x04
  std::int32_t player_race = 0;                            // +0x08
  std::array<std::int32_t, kMaxRaces> species{};           // +0x0c
  std::array<std::int32_t, kMaxRaces> colors{};            // +0x28
  std::array<std::int32_t, kMaxRaces> player_relations{};  // +0x44
  std::int32_t fleet = 0;                                  // +0x60 player's ships
  std::int32_t colonies = 0;                               // +0x64 player's planets
  std::int32_t discoveries = 0;                            // +0x68 player's research
  std::array<std::uint8_t, 8> stale{};                     // +0x6c
};

}  // namespace oa::game
