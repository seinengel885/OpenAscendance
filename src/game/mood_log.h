// peCustom, temporary (balancing): every change of an attitude with its
// cause, which the user interface writes next to the saves. Off unless the
// user interface turns it on (`mood_log_start`); the day does not read it.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/state.h"

namespace oa::game {

struct Session;

// Why attitudes changed: a text, for the actions between two races also
// the acting race and its target (kNoRace when none).
struct MoodCause {
  static constexpr std::uint8_t kNoRace = 0xff;
  std::string text;
  std::uint8_t actor = kNoRace;
  std::uint8_t target = kNoRace;
};

struct MoodChange {
  std::int32_t day = 0;
  std::uint8_t race = 0;      // whose attitude
  std::uint8_t other = 0;     // toward whom
  std::uint8_t relation = 0;  // `RaceRelation` then
  std::int16_t before = 0;
  std::int16_t after = 0;
  MoodCause cause;
};

struct MoodLog {
  bool on = false;
  // The attitudes as last noted.
  std::array<std::array<std::int16_t, kMaxRaces>, kMaxRaces> seen{};
  // The causes of the open `MoodLogScope`s, innermost last.
  std::vector<MoodCause> causes;
  // The changes not yet taken by the user interface.
  std::vector<MoodChange> changes;
};

// Turns the log on: the current attitudes are the starting point.
void mood_log_start(Session& session);

// Notes the changes since the last note with `cause` (peCustom only).
void mood_log_note(Session& session, const MoodCause& cause);

// Notes the changes made while it is open with its cause; changes made
// before it opens go to the enclosing scope (else "sonstige").
class MoodLogScope {
 public:
  explicit MoodLogScope(Session& session, std::string cause = {});
  ~MoodLogScope();
  MoodLogScope(const MoodLogScope&) = delete;
  MoodLogScope& operator=(const MoodLogScope&) = delete;

  // Whether the log is on: only then is a computed cause worth setting.
  bool active() const { return active_; }
  void set_cause(MoodCause cause);

 private:
  Session& session_;
  bool active_;
};

}  // namespace oa::game
