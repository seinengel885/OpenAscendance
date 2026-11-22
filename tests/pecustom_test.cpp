// The peCustom variant (`game/variant.h`): the Antagonizer's game with
// fixes of its own, and its saves (`game/save.h`), which must keep loading
// in later builds. `pecustom_v<N>.sav` in the folder named by
// OA_PECUSTOM_SAVES is a save of peCustom save format N, kept as written
// then; it holds text of the game files, so it is not part of this
// repository (the tests using it are skipped without it).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "core/error.h"
#include "game/cosmos.h"
#include "game/new_game.h"
#include "game/planet.h"
#include "game/planet_plan.h"
#include "game/save.h"
#include "game/session.h"
#include "game_oracle.h"

namespace oa::game {
namespace {

using oracle_test::first_difference;

// A new game of `variant` (50 stars, 7 races, seed 7) played `days` days by
// the AI, the player doing nothing.
void play(Session& session, Variant variant, int days) {
  session.game->variant = variant;
  const CosmosTxt txt{.stars = 50, .races = 7, .seed = 7, .species = {}};
  program_start(session, 0, &txt, false);
  NewGameSetup setup;
  setup.activate(session);
  setup.begin(session);
  session.window_state.assign(kWindowStateSize, 0);
  for (int d = 0; d < days; ++d) {
    cosmos_next_turn(session);
    session.events.take();
  }
}

// Returns the player's first planet.
PlanetIndex player_planet(const Session& session) {
  const Cosmos& cosmos = session.game->cosmos;
  for (PlanetIndex i = 0; i < cosmos.planet_count; ++i) {
    if (cosmos.planets[static_cast<std::size_t>(i)].owner == session.player_race) {
      return i;
    }
  }
  FAIL("the player has no planet");
  return kNone;
}

// Returns the save of format `version` (`pecustom_v<version>.sav`), nothing
// when OA_PECUSTOM_SAVES is unset.
std::optional<std::vector<std::uint8_t>> versioned_save(int version) {
  const char* dir = std::getenv("OA_PECUSTOM_SAVES");
  if (dir == nullptr || *dir == '\0') {
    return std::nullopt;
  }
  const std::filesystem::path path = std::filesystem::path(dir) / std::format("pecustom_v{}.sav", version);
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::optional<std::vector<std::uint8_t>> first_save() { return versioned_save(1); }

TEST_CASE("a peCustom game plays on, the same every time", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  Session first(*t);
  Session second(*t);
  play(first, Variant::kPeCustom, 60);
  play(second, Variant::kPeCustom, 60);
  CHECK(first.game->cosmos.day == 60);
  CHECK(first.game->variant == Variant::kPeCustom);
  CHECK(first_difference(serialize_save(save_game(first)), serialize_save(save_game(second))) == -1);
}

TEST_CASE("a peCustom game saves and loads as peCustom", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  Session session(*t);
  play(session, Variant::kPeCustom, 30);
  const PlanetIndex home = player_planet(session);
  // Its management and a label (format 4 on).
  PlanetManagement& management = session.game->planet_management.at(static_cast<std::size_t>(home));
  management.levels[PlanetManagement::kShipyard] = BuildLevel::kFirst;
  management.counts[PlanetManagement::kSurfaceShields] = 5;
  management.final_outputs[PlanetManagement::kIndustry] = {FinalMode::kAtLeast, 7, kFinalWeights};
  management.counts[PlanetManagement::kOrbitalShields] = 3;  // format 5 on
  management.labels = 1;
  session.game->planet_labels = {"Heimat"};
  session.game->peace_offer_days[1] = 29;  // format 6 on
  management.bell = 1;                     // format 7 on
  management.final_outputs[PlanetManagement::kResearch] = {FinalMode::kMaximise, 0, 3};  // format 8 on
  // The tenths carried (format 9 on); the outputs' tenths are the planet's.
  Planet& planet = session.game->cosmos.planets.at(static_cast<std::size_t>(home));
  planet.progress_carry = 13;
  planet.growth_carry = 7;
  const std::vector<std::uint8_t> bytes = serialize_save(save_game(session));
  if (const char* out = std::getenv("OA_WRITE_PECUSTOM_FIXTURE"); out != nullptr && *out != '\0') {
    std::ofstream(out, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  const SaveImage image = parse_save(bytes);
  CHECK(image.variant == Variant::kPeCustom);
  CHECK(image.pe_custom_version == kPeCustomSaveVersion);
  CHECK(image.day == 30);

  Session loaded(*t);
  load_game(loaded, image);
  CHECK(loaded.game->variant == Variant::kPeCustom);
  CHECK(loaded.game->cosmos.day == 30);
  const PlanetManagement& m = loaded.game->planet_management.at(static_cast<std::size_t>(home));
  CHECK(m.levels[PlanetManagement::kShipyard] == BuildLevel::kFirst);
  CHECK(m.levels[PlanetManagement::kDig] == BuildLevel::kStandard);
  CHECK(m.counts[PlanetManagement::kSurfaceShields] == 5);
  CHECK(m.final_outputs[PlanetManagement::kIndustry] == FinalOutput{FinalMode::kAtLeast, 7, kFinalWeights});
  CHECK(m.final_outputs[PlanetManagement::kResearch] == FinalOutput{FinalMode::kMaximise, 0, 3});
  CHECK(m.final_outputs[PlanetManagement::kProsperity].mode == FinalMode::kStandard);
  CHECK(m.counts[PlanetManagement::kOrbitalShields] == 3);
  CHECK(m.labels == 1);
  CHECK(loaded.game->planet_labels == std::vector<std::string>{"Heimat"});
  CHECK(loaded.game->peace_offer_days[1] == 29);
  CHECK(loaded.game->peace_offer_days[2] == kNoAttackDay);
  CHECK(m.bell == 1);
  const Planet& p = loaded.game->cosmos.planets.at(static_cast<std::size_t>(home));
  CHECK(p.progress_carry == 13);
  CHECK(p.growth_carry == 7);
  CHECK(p.industry_tenths == planet.industry_tenths);
  CHECK(p.prosperity_tenths == planet.prosperity_tenths);
  // Saved again: the same file but for what loading resets (the AI target
  // counts, the clusters), as for the original's saves.
  SaveImage expected = image;
  for (Race& race : expected.races) {
    race.target_count = 0;
  }
  expected.clusters.assign(expected.clusters.size(), Cluster{});
  CHECK(first_difference(serialize_save(save_game(loaded)), serialize_save(expected)) == -1);
  // The loaded game plays on.
  cosmos_next_turn(loaded);
  CHECK(loaded.game->cosmos.day == 31);
}

TEST_CASE("the peCustom trailer: variant, then its save version", "[game][pecustom]") {
  const auto saved = first_save();
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v1.sav");
  }
  const std::vector<std::uint8_t>& file = *saved;
  REQUIRE(file.size() > 12);
  const auto u32_at = [&file](std::size_t at) {
    return static_cast<std::uint32_t>(file[at] | file[at + 1] << 8 | file[at + 2] << 16 | file[at + 3] << 24);
  };
  const std::size_t trailer = file.size() - 12;
  CHECK(std::string(file.begin() + static_cast<std::ptrdiff_t>(trailer),
                    file.begin() + static_cast<std::ptrdiff_t>(trailer) + 4) == "OAvt");
  CHECK(u32_at(trailer + 4) == 0x50450001);
  CHECK(u32_at(trailer + 8) == 1);
  // A save of a newer build's format is refused, not misread.
  std::vector<std::uint8_t> newer = file;
  newer[trailer + 8] = static_cast<std::uint8_t>(kPeCustomSaveVersion + 1);
  CHECK_THROWS_AS(parse_save(newer), DataError);
  // So is a variant this build does not know.
  std::vector<std::uint8_t> unknown = file;
  unknown[trailer + 4] = 2;
  CHECK_THROWS_AS(parse_save(unknown), DataError);
}

TEST_CASE("the first peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = first_save();
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v1.sav");
  }
  const std::vector<std::uint8_t>& file = *saved;
  const SaveImage image = parse_save(file);
  CHECK(image.variant == Variant::kPeCustom);
  CHECK(image.pe_custom_version == 1);
  CHECK(image.day == 30);
  Session session(*t);
  load_game(session, image);
  CHECK(session.game->variant == Variant::kPeCustom);
  CHECK(session.game->cosmos.day == 30);
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
  // Saved again by this build: a save of the current format that loads.
  const SaveImage again = parse_save(serialize_save(save_game(session)));
  CHECK(again.variant == Variant::kPeCustom);
  CHECK(again.pe_custom_version == kPeCustomSaveVersion);
  CHECK(again.day == 31);
}

TEST_CASE("the version-2 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(2);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v2.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.variant == Variant::kPeCustom);
  CHECK(image.pe_custom_version == 2);
  CHECK(image.day == 30);
  Session session(*t);
  load_game(session, image);
  CHECK(session.game->cosmos.day == 30);
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-3 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(3);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v3.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.variant == Variant::kPeCustom);
  CHECK(image.pe_custom_version == 3);
  CHECK(image.day == 30);
  Session session(*t);
  load_game(session, image);
  CHECK(session.game->cosmos.day == 30);
  // Saved with a goal of the former planner (read and dropped).
  // Saved before the planets' management: all standard, no labels.
  CHECK(std::ranges::all_of(session.game->planet_management,
                            [](const PlanetManagement& m) {
                              return m.final_outputs[PlanetManagement::kIndustry].mode == FinalMode::kStandard && m.labels == 0;
                            }));
  CHECK(session.game->planet_labels.empty());
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-4 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(4);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v4.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.pe_custom_version == 4);
  Session session(*t);
  load_game(session, image);
  // The management and label the save was written with.
  const PlanetManagement& m = session.game->planet_management.at(static_cast<std::size_t>(player_planet(session)));
  CHECK(m.levels[PlanetManagement::kShipyard] == BuildLevel::kFirst);
  // Its industry minimum (format 4..7) as the final build-out's.
  CHECK(m.final_outputs[PlanetManagement::kIndustry] == FinalOutput{FinalMode::kAtLeast, 7, kFinalWeights});
  CHECK(m.final_outputs[PlanetManagement::kResearch].mode == FinalMode::kStandard);
  CHECK(session.game->planet_labels == std::vector<std::string>{"Heimat"});
  // Saved before the orbital shields had a count of their own.
  CHECK(m.counts[PlanetManagement::kOrbitalShields] == -1);
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-5 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(5);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v5.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.pe_custom_version == 5);
  Session session(*t);
  load_game(session, image);
  // The orbital shields count the save was written with.
  const PlanetManagement& m = session.game->planet_management.at(static_cast<std::size_t>(player_planet(session)));
  CHECK(m.counts[PlanetManagement::kOrbitalShields] == 3);
  // Saved before the peace offers were counted.
  CHECK(session.game->peace_offer_days[1] == kNoAttackDay);
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-6 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(6);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v6.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.pe_custom_version == 6);
  Session session(*t);
  load_game(session, image);
  // The day of the peace offer the save was written with.
  CHECK(session.game->peace_offer_days[1] == 29);
  // Saved before the project bells.
  CHECK(session.game->planet_management.at(static_cast<std::size_t>(player_planet(session))).bell == 0);
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-7 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(7);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v7.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.pe_custom_version == 7);
  Session session(*t);
  load_game(session, image);
  // The project bell the save was written with.
  CHECK(session.game->planet_management.at(static_cast<std::size_t>(player_planet(session))).bell == 1);
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-8 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(8);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v8.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.pe_custom_version == 8);
  Session session(*t);
  load_game(session, image);
  // The final build-out the save was written with.
  const PlanetManagement& m = session.game->planet_management.at(static_cast<std::size_t>(player_planet(session)));
  CHECK(m.final_outputs[PlanetManagement::kIndustry] == FinalOutput{FinalMode::kAtLeast, 7, kFinalWeights});
  CHECK(m.final_outputs[PlanetManagement::kResearch] == FinalOutput{FinalMode::kMaximise, 0, 3});
  // The outputs of peCustom's curve, in tenths, from the start.
  for (PlanetIndex i = 0; i < session.game->cosmos.planet_count; ++i) {
    const Planet& p = session.game->cosmos.planets[static_cast<std::size_t>(i)];
    if (p.owner == kNoOwner) {
      continue;
    }
    Planet again = p;
    planet_recalc_output(session, again);
    CAPTURE(i);
    CHECK(planet_industry_tenths(p) == planet_industry_tenths(again));
    CHECK(planet_prosperity_tenths(p) == planet_prosperity_tenths(again));
    CHECK(p.progress_carry == 0);
  }
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-9 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(9);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v9.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.pe_custom_version == 9);
  Session session(*t);
  load_game(session, image);
  // The tenths carried the save was written with.
  const Planet& p = session.game->cosmos.planets.at(static_cast<std::size_t>(player_planet(session)));
  CHECK(p.progress_carry == 13);
  CHECK(p.growth_carry == 7);
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("the version-10 peCustom saves still load", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  const auto saved = versioned_save(10);
  if (!saved) {
    SKIP("OA_PECUSTOM_SAVES is not set to the folder of pecustom_v10.sav");
  }
  const SaveImage image = parse_save(*saved);
  CHECK(image.pe_custom_version == 10);
  Session session(*t);
  load_game(session, image);
  const PlanetManagement& m =
      session.game->planet_management.at(static_cast<std::size_t>(player_planet(session)));
  CHECK(m.final_outputs[PlanetManagement::kResearch] == FinalOutput{FinalMode::kMaximise, 0, 3});
  cosmos_next_turn(session);
  session.events.take();
  CHECK(session.game->cosmos.day == 31);
}

TEST_CASE("a planet's buildings are none by standard, the home planet's Shipyard first", "[game][content][pecustom]") {
  OA_REQUIRE_TABLES(t);
  Session session(*t);
  play(session, Variant::kPeCustom, 0);
  const Cosmos& cosmos = session.game->cosmos;
  const Planet& home = cosmos.planets[static_cast<std::size_t>(player_planet(session))];
  const Planet* other = nullptr;
  for (PlanetIndex i = 0; i < cosmos.planet_count && other == nullptr; ++i) {
    other = cosmos.planets[static_cast<std::size_t>(i)].owner == kNoOwner ? &cosmos.planets[static_cast<std::size_t>(i)]
                                                                         : nullptr;
  }
  REQUIRE(other != nullptr);
  const PlanetManagement h = planet_management_standard(session, home);
  const PlanetManagement o = planet_management_standard(session, *other);
  for (std::size_t b = 0; b < o.levels.size(); ++b) {
    CAPTURE(b);
    CHECK(o.levels[b] == BuildLevel::kNo);
    CHECK(h.levels[b] == (b == PlanetManagement::kShipyard ? BuildLevel::kFirst : BuildLevel::kNo));
  }
  for (std::size_t c = 0; c < o.counts.size(); ++c) {
    CAPTURE(c);
    CHECK(o.counts[c] == 0);
    CHECK(h.counts[c] == 0);
  }
}

}  // namespace
}  // namespace oa::game
