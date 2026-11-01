#include "game/mood_log.h"

#include <utility>

#include "game/session.h"

namespace oa::game {

namespace {

bool logging(const Session& session) { return session.mood_log.on && session.game->variant == Variant::kPeCustom; }

}  // namespace

void mood_log_start(Session& session) {
  MoodLog& log = session.mood_log;
  log.on = true;
  log.causes.clear();
  for (std::size_t r = 0; r < kMaxRaces; ++r) {
    log.seen[r] = session.game->cosmos.races[r].attitude;
  }
}

void mood_log_note(Session& session, std::string_view cause) {
  if (!logging(session)) {
    return;
  }
  MoodLog& log = session.mood_log;
  const Cosmos& cosmos = session.game->cosmos;
  for (std::size_t r = 0; r < kMaxRaces; ++r) {
    // The player's own attitudes are not used.
    const bool player = r == session.player_race && !session.autoplay;
    for (std::size_t o = 0; o < kMaxRaces; ++o) {
      const std::int16_t now = cosmos.races[r].attitude[o];
      std::int16_t& seen = log.seen[r][o];
      if (now != seen) {
        if (!player) {
          log.changes.push_back({cosmos.day, static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(o),
                                 cosmos.races[r].relation[o], seen, now, std::string(cause)});
        }
        seen = now;
      }
    }
  }
}

MoodLogScope::MoodLogScope(Session& session, std::string cause) : session_(session), active_(logging(session)) {
  if (active_) {
    std::vector<std::string>& causes = session_.mood_log.causes;
    mood_log_note(session_, causes.empty() ? std::string_view("sonstige") : std::string_view(causes.back()));
    causes.push_back(std::move(cause));
  }
}

MoodLogScope::~MoodLogScope() {
  if (active_) {
    std::vector<std::string>& causes = session_.mood_log.causes;
    mood_log_note(session_, causes.back());
    causes.pop_back();
  }
}

void MoodLogScope::set_cause(std::string cause) {
  if (active_) {
    session_.mood_log.causes.back() = std::move(cause);
  }
}

}  // namespace oa::game
