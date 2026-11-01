// One run of the original program: the game state plus the session globals
// the game code reads besides it (the random generator, the player's race,
// the position grid, the research-tree flag). Galaxies generated in the same
// session reuse the same records, so their stale fields carry over, as in
// the original.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/watcom_random.h"
#include "game/events.h"
#include "game/state.h"
#include "game/tables.h"

namespace oa::game {

struct Session {
  // `tables` must outlive the session.
  explicit Session(const GameTables& tables) : tables(&tables), game(std::make_unique<Game>()) {}

  // Sets the player's race slot and its bit mask (`Cosmos_set_player_race`
  // 0x1cf14); 0xff selects slot 0 with every bit (autoplay).
  void set_player_race(std::uint8_t race) {
    player_mask = race == 0xff ? 0xff : static_cast<std::uint8_t>(1u << race);
    player_race = race == 0xff ? 0 : race;
  }

  // Posts a game event (`WinMgr_post_game_event` 0x50efc): nothing in
  // autoplay, dropped when the queue is full; returns whether it was queued.
  bool post_event(EventCode code, std::int32_t a = 0, std::int32_t b = 0) {
    return !autoplay && events.post(code, a, b);
  }

  const GameTables* tables;
  std::unique_ptr<Game> game;         // the global objects (cosmos, research, ...)
  WatcomRandom rng;                   // the CRT generator (state 0x89718)
  std::uint8_t player_race = 0;       // 0xf4576
  std::uint8_t player_mask = 1;       // 0x8675c
  std::int32_t grid = 5;              // 0x86a14, position snap grid
  bool autoplay = false;              // 0x90b8c, the `flash.pop` demo mode
  bool research_tree_loaded = false;  // 0xf4728

  // The day's simulation state outside the saved records.
  EventQueue events;  // the window manager's game events (UI drains them)
  // 0xc82ec: per star, bit 0 "visited by `Cosmos_next_contested_star` /
  // arrival event shown today", bit `1 << race` "race offended here today"
  // (`Race_offended_by`); cleared by `Cosmos_end_day`.
  std::array<std::uint8_t, kMaxStars> star_day_flags{};
  // peCustom: per ship slot the star it is at in space or orbit and the day
  // it was first seen there (`cosmos_note_ship_arrivals`); not saved, so
  // after loading each ship's day there starts anew.
  std::array<StarIndex, kMaxShips> ship_arrival_star = null_indices<kMaxShips>();
  std::array<std::int32_t, kMaxShips> ship_arrival_day{};
  std::int32_t player_mood = 0;  // 0xf4574, the player's bio-rhythm mood (UI)
  // 0xfe56c (high half of the dword 0xfe56a): the current screen, which some
  // day code tests (16 the System Display: the tactical AI also plays the
  // player's ships and some events are not posted; 17: other events are not
  // posted). The day normally ends from the star map (12).
  std::int16_t screen = 12;
  // A stack slot the original reads uninitialised: the argument local of
  // `Race_diplomacy_turn` (0x3fb00, esp+4), which only the third-party
  // actions write. It holds what the calls before left at that address: the
  // race loop counter of `Race_update_attitude_drift` (low word, the race
  // count), on bio-rhythm days `Race_bio_rhythm`'s double (the whole slot),
  // `Race_list_ships`'s and the ship orders' counts (high word 0); the
  // user interface's frames are not modelled (`docs/09-simulation.md`). The
  // AI-to-AI answers store it in `Race::pending_action_arg`.
  std::uint32_t stale_dip_arg = 0;
  // The shared work buffer 0xc8c2c (ANTAG 0xd8da0, both 0x35e34 bytes after
  // the cosmos), kScratchSize bytes: `main` 0x1c424 loads `logo.gif` into
  // it at startup (`program_start`); later users are the screen under each
  // modal window (`save_screen_under` 0x4f618, `WinMgr::do_modal`), and
  // `Cosmos_compute_hops` (its first n(n-1) bytes) and the species history
  // and end-of-game texts, not modelled (below the offsets the day reads).
  // The day reads it only through the out-of-bounds square of a
  // planet-wide project (`planet_daily_update`).
  static constexpr std::size_t kScratchSize = 160000;
  std::vector<std::uint8_t> scratch;
  // The runtime address of the cosmos object, which the saves' raw pointers
  // and the battle scratch fields hold (`game/address.h`). It depends on
  // where DOS/4GW loaded the program (the original's runs differ by build
  // and environment); 0 selects the variant's usual address (`game/save.h`).
  std::uint32_t cosmos_address = 0;
  // Antagonizer: the cache of `Race_research_complete` (ANTAG 0x40590,
  // globals 0x10509c valid flag / 0x1050a0 result), shared by every race:
  // `Race_turn_update` invalidates it before each race's AI turn.
  bool research_complete_valid = false;
  bool research_complete = false;
  // Antagonizer: the strategic AI's work arrays (ANTAG 0x1046e8..0x105038),
  // rebuilt by each AI race's turn but read stale in between (the battle
  // turn's ship orders use the last race's values).
  struct AntagAiWork {
    std::array<std::int32_t, kMaxStars> colonise{};         // 0x104bec
    std::array<std::int32_t, kMaxStars> defence{};          // 0x104d7c
    std::array<std::int8_t, kMaxStars> war_ships{};         // 0x104f0c
    std::array<std::int8_t, kMaxStars> invasion_ships{};    // 0x104f70
    std::array<std::int8_t, kMaxStars> colony_ships{};      // 0x104fd4
    std::array<std::uint8_t, kMaxStars> frontier{};         // 0x105038
    std::array<std::int32_t, kMaxStars> colony_demand{};    // 0x1046e8
    std::array<std::int32_t, kMaxStars> war_demand{};       // 0x104878
    std::array<std::int32_t, kMaxStars> invasion_demand{};  // 0x104a08
    std::array<std::int32_t, 10> yard_planet{};             // 0x104b98
    std::array<std::int32_t, 10> yard_score{};              // 0x104bc0, never cleared
  };
  AntagAiWork antag_ai;
  // What the next save writes besides the game: the stale header bytes
  // (entries past the race count, `stale`) and the window state, owned by
  // the user interface (`WinMgr_serialize_windows` 0x556a4).
  SaveHeader stale_header;
  std::vector<std::uint8_t> window_state;
};

}  // namespace oa::game
