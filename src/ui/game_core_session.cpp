#include "ui/game_core_session.h"

#include <array>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>

#include "assets/static_strings.h"
#include "core/byte_reader.h"
#include "core/error.h"
#include "core/log.h"
#include "game/cosmos.h"
#include "game/galaxy.h"
#include "game/mood_log.h"
#include "game/save.h"

namespace oa {

namespace {

// The save header's size (`Game_save` 0x4f458).
constexpr std::size_t kHeaderSize = 0x74;

// peCustom, temporary: the attitude log next to the saves; the species
// names in `static.txt`.
constexpr const char* kMoodLogName = "STIMMUNG.TXT";
constexpr int kStringSpecies = 101;

// Reads the load-list summary of a save header (`LoadWnd_read_headers`
// 0x30dc8).
SaveHeader parse_header(std::span<const std::uint8_t> data) {
  ByteReader in(data, "save header");
  SaveHeader h;
  h.day = in.i32();
  h.race_count = in.i32();
  h.player_race = in.i32();
  for (int& v : h.species) {
    v = in.i32();
  }
  for (int& v : h.colors) {
    v = in.i32();
  }
  for (int& v : h.relations) {
    v = in.i32();
  }
  h.fleet = in.i32();
  h.colonies = in.i32();
  h.discoveries = in.i32();
  return h;
}

// Returns the entry of `dir` named `name` (case-insensitively), if any.
std::optional<std::filesystem::path> find_in(const std::filesystem::path& dir, const std::string& name) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
    return std::nullopt;
  }
  const auto lower = [](std::string s) {
    for (char& c : s) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  };
  const std::string wanted = lower(name);
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && lower(entry.path().filename().string()) == wanted) {
      return entry.path();
    }
  }
  return std::nullopt;
}

}  // namespace

GameCoreSession::GameCoreSession(const GameFiles& files, std::filesystem::path save_dir, bool install_saves)
    : files_(files),
      save_dir_(std::move(save_dir)),
      install_saves_(install_saves),
      tables_(std::make_unique<game::GameTables>(game::load_game_tables(files))),
      session_(*tables_) {}

void GameCoreSession::start(std::uint32_t time_seed, const game::CosmosTxt* cosmos_txt) {
  // The variants draw differently from the start (`tests/oracle/corpus/MANIFEST.md`).
  session_.game->variant = new_game_variant();
  game::program_start(session_, time_seed, cosmos_txt, resume_exists());
}

std::string GameCoreSession::file_name(int slot) {
  return slot == kResumeSlot ? std::string("RESUME.GAM") : std::format("{:02d}.SAV", slot);
}

int GameCoreSession::day() const { return session_.game->cosmos.day; }

int GameCoreSession::race_count() const { return session_.game->cosmos.race_count; }

int GameCoreSession::player_race() const { return session_.player_race; }

int GameCoreSession::race_species(int race) const {
  return session_.game->cosmos.races.at(static_cast<std::size_t>(race)).species;
}

int GameCoreSession::race_color(int race) const {
  return session_.game->cosmos.races.at(static_cast<std::size_t>(race)).color;
}

std::optional<std::filesystem::path> GameCoreSession::find_save(int slot) const {
  const std::string name = file_name(slot);
  if (auto path = find_in(save_dir_, name)) {
    return path;
  }
  if (install_saves_) {
    return files_.install().find(name);
  }
  return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> GameCoreSession::read_save(int slot) const {
  const std::optional<std::filesystem::path> path = find_save(slot);
  if (!path) {
    return std::nullopt;
  }
  return read_file(*path);
}

std::vector<SaveSlot> GameCoreSession::list_saves() const {
  std::vector<SaveSlot> slots(kSaveSlots);
  for (int i = 0; i < kSaveSlots; ++i) {
    SaveSlot& s = slots[static_cast<std::size_t>(i)];
    s.slot = i + 1;
    const std::optional<std::filesystem::path> path = find_save(s.slot);
    if (!path) {
      continue;
    }
    try {
      const std::vector<std::uint8_t> data = read_file(*path);
      if (data.size() < kHeaderSize) {
        continue;
      }
      s.header = parse_header(std::span(data).first(kHeaderSize));
      // The file clock's epoch is unspecified: through the current times.
      const auto written = std::filesystem::last_write_time(*path) - std::filesystem::file_time_type::clock::now() +
                           std::chrono::system_clock::now();
      const std::time_t t = std::chrono::system_clock::to_time_t(
          std::chrono::time_point_cast<std::chrono::system_clock::duration>(written));
      std::tm local{};
#ifdef _WIN32
      localtime_s(&local, &t);
#else
      localtime_r(&t, &local);
#endif
      s.year = local.tm_year + 1900;
      s.month = local.tm_mon + 1;
      s.day = local.tm_mday;
      s.hour = local.tm_hour;
      s.minute = local.tm_min;
    } catch (const DataError& e) {
      log_warning("{}: {}", path->string(), e.what());
    } catch (const std::filesystem::filesystem_error& e) {
      log_warning("{}: {}", path->string(), e.what());
    }
  }
  return slots;
}

bool GameCoreSession::save(int slot) {
  if (save_dir_.empty()) {
    return false;
  }
  if (windows_ != nullptr) {
    windows_->store_window_state();
  }
  const std::vector<std::uint8_t> bytes = game::serialize_save(game::save_game(session_));
  // An existing file of another case is replaced.
  const std::filesystem::path path = find_in(save_dir_, file_name(slot)).value_or(save_dir_ / file_name(slot));
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    log_warning("cannot write {}", path.string());
    return false;
  }
  return true;
}

bool GameCoreSession::load(int slot) {
  const std::optional<std::vector<std::uint8_t>> data = read_save(slot);
  return data && load_bytes(*data, file_name(slot));
}

bool GameCoreSession::load_game_file(const std::string& name) {
  const std::optional<std::vector<std::uint8_t>> data = files_.try_load(name);
  return data && load_bytes(*data, name);
}

bool GameCoreSession::load_bytes(std::span<const std::uint8_t> data, const std::string& name) {
  game::SaveImage image;
  try {
    image = game::parse_save(data);
  } catch (const DataError& e) {
    log_warning("{}: {}", name, e.what());
    return false;
  }
  game::load_game(session_, image);
  has_game_ = true;
  start_mood_log(std::format("Spielstand {} geladen, Tag {}", name, session_.game->cosmos.day));
  if (windows_ != nullptr) {
    windows_->restore_window_state();
  }
  return true;
}

bool GameCoreSession::resume_exists() const { return find_save(kResumeSlot).has_value(); }

void GameCoreSession::open_new_game(const NewGameSettings& /*settings*/) {
  // The galaxy generation applies the variant's attitude shifts.
  session_.game->variant = new_game_variant();
  setup_.activate(session_);
}

void GameCoreSession::generate_galaxy(int stars, int races, int species) {
  game::cosmos_generate(session_, static_cast<std::int16_t>(stars), static_cast<std::int16_t>(races),
                        static_cast<std::uint8_t>(species));
}

void GameCoreSession::new_galaxy(int races, int species) {
  game::cosmos_new_galaxy(session_, static_cast<std::int16_t>(races), static_cast<std::uint8_t>(species));
}

void GameCoreSession::assign_colors(int color) {
  game::cosmos_assign_colors(session_, static_cast<std::uint8_t>(color));
}

void GameCoreSession::begin_game(const NewGameSettings& settings) {
  game::cosmos_generate_planets(session_);
  game::cosmos_compute_hops(session_.game->cosmos);
  session_.game->cosmos.atmosphere = static_cast<game::Atmosphere>(settings.atmosphere);
  has_game_ = true;
  start_mood_log("Neues Spiel");
}

int GameCoreSession::atmosphere() const { return static_cast<int>(session_.game->cosmos.atmosphere); }

int GameCoreSession::next_contested_star() { return game::cosmos_next_contested_star(session_); }

bool GameCoreSession::end_day() {
  const bool playing = session_.game->cosmos.ending == 0;
  game::cosmos_end_day(session_);
  game::mood_log_note(session_, {"sonstige"});
  write_mood_log({});
  return playing && session_.game->cosmos.ending != 0;
}

void GameCoreSession::start_mood_log(const std::string& marker) {
  // The game before's changes since its last day are dropped (they would be
  // named after this game's races).
  session_.mood_log.changes.clear();
  if (save_dir_.empty() || session_.game->variant != game::Variant::kPeCustom) {
    session_.mood_log.on = false;
    return;
  }
  game::mood_log_start(session_);
  write_mood_log(marker);
}

std::string GameCoreSession::mood_race_name(int race) {
  if (race == session_.player_race && !session_.autoplay) {
    return "du";
  }
  if (species_names_.empty()) {
    species_names_ = parse_static_strings(files_.load("static.txt"));
    for (std::string& name : species_names_) {
      name = files_.text("static.txt", name);
    }
  }
  const auto species = static_cast<std::size_t>(kStringSpecies + race_species(race));
  return species < species_names_.size() ? species_names_[species] : "?";
}

void GameCoreSession::write_mood_log(const std::string& marker) {
  std::vector<game::MoodChange>& changes = session_.mood_log.changes;
  if (marker.empty() && changes.empty()) {
    return;
  }
  const std::filesystem::path path = find_in(save_dir_, kMoodLogName).value_or(save_dir_ / kMoodLogName);
  const bool fresh = !std::filesystem::exists(path);
  std::ofstream out(path, std::ios::app);
  if (fresh) {
    out << "Tag\tVolk\tgegenüber\tBeziehung\tÄnderung\tneu\tUrsache\n";
  }
  if (!marker.empty()) {
    out << "# " << marker << '\n';
  }
  constexpr std::array<const char*, 4> kRelations = {"unbekannt", "Frieden", "Krieg", "Bündnis"};
  for (const game::MoodChange& c : changes) {
    std::string cause = c.cause.text;
    if (c.cause.actor != game::MoodCause::kNoRace) {
      cause += std::format(" ({} an {})", mood_race_name(c.cause.actor), mood_race_name(c.cause.target));
    }
    out << std::format("{}\t{}\t{}\t{}\t{:+}\t{}\t{}\n", c.day, mood_race_name(c.race), mood_race_name(c.other),
                       c.relation < kRelations.size() ? kRelations[c.relation] : "?", c.after - c.before, c.after,
                       cause);
  }
  changes.clear();
  if (!out) {
    log_warning("cannot write {}", path.string());
  }
}

}  // namespace oa
