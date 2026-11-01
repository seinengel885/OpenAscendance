// The game session over the headless core (`src/game`): `main`'s start-up
// galaxy, the new-game set-up, the saves (`NN.SAV`, `RESUME.GAM`, byte-exact
// through `game/save.h`, the window state from the window manager) and the
// day.
//
// The saves are written to the session's save folder (the original's
// current directory) as `NN.SAV` and `RESUME.GAM`; they are read from it, or
// else (optionally) from the install, whose shipped `01.SAV` and
// `RESUME.GAM` the original sees in its directory.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "assets/cfile.h"
#include "game/new_game.h"
#include "game/session.h"
#include "game/tables.h"
#include "ui/game_session.h"

namespace oa {

class GameCoreSession final : public GameSession {
 public:
  // The slot of `RESUME.GAM` in the file names (`file_name`).
  static constexpr int kResumeSlot = 0;

  // Reads the game tables of `files` (which must outlive the session).
  // `save_dir` is the folder the saves are written to (empty: none
  // written); with `install_saves`, a save missing there is read from the
  // install. Throws `DataError` when a table is missing or malformed.
  GameCoreSession(const GameFiles& files, std::filesystem::path save_dir, bool install_saves);

  // `main` (0x1c424): the start-up galaxy of seed `time_seed`, or of
  // `cosmos_txt` (`cosmos.txt`) when given, under the rules of the new
  // game's variant; its hops only when no `RESUME.GAM` exists.
  void start(std::uint32_t time_seed, const game::CosmosTxt* cosmos_txt);

  // The file of save `slot` (1..20: `NN.SAV`, `kResumeSlot`: `RESUME.GAM`).
  static std::string file_name(int slot);

  bool has_game() const override { return has_game_; }
  int day() const override;
  int race_count() const override;
  int player_race() const override;
  int race_species(int race) const override;
  int race_color(int race) const override;

  std::vector<SaveSlot> list_saves() const override;
  bool save(int slot) override;
  bool load(int slot) override;
  bool resume_exists() const override;
  bool save_resume() override { return save(kResumeSlot); }
  bool load_resume() override { return load(kResumeSlot); }

  void open_new_game(const NewGameSettings& settings) override;
  void generate_galaxy(int stars, int races, int species) override;
  void new_galaxy(int races, int species) override;
  void assign_colors(int color) override;
  void begin_game(const NewGameSettings& settings) override;
  int atmosphere() const override;
  game::Variant variant() const override { return session_.game->variant; }

  game::Session* core() override { return &session_; }

  int next_contested_star() override;
  bool end_day() override;
  bool load_game_file(const std::string& name) override;
  void srand(std::uint32_t seed) override { session_.rng.srand(seed); }
  bool has_game_events() const override { return !session_.events.empty(); }
  void set_current_screen(int screen) override { session_.screen = static_cast<std::int16_t>(screen); }

 private:
  // The bytes of save `slot`, nothing when it does not exist.
  std::optional<std::vector<std::uint8_t>> read_save(int slot) const;
  // `Game_load` of the save bytes `data` (named `name` in the warnings);
  // returns false when they are not a save.
  bool load_bytes(std::span<const std::uint8_t> data, const std::string& name);
  // The host path save `slot` is read from, nothing when it does not exist.
  std::optional<std::filesystem::path> find_save(int slot) const;
  // peCustom, temporary: turns the attitude log on (with a save folder),
  // `marker` its first line; writes the changes noted so far to the log
  // file next to the saves (`marker` before them unless empty).
  void start_mood_log(const std::string& marker);
  void write_mood_log(const std::string& marker);
  // A race in the log: its species name, "du" for the player.
  std::string mood_race_name(int race);

  const GameFiles& files_;
  std::filesystem::path save_dir_;
  bool install_saves_;
  std::unique_ptr<game::GameTables> tables_;
  game::Session session_;
  game::NewGameSetup setup_;
  bool has_game_ = false;
  std::vector<std::string> species_names_;  // static.txt, for the mood log
};

}  // namespace oa
