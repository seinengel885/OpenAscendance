// The game events the simulation posts for the user interface
// (`WinMgr_post_game_event` 0x50efc): the window manager's queue of at most
// 32 records (`i16` code, two `u32` arguments at WinMgr +0x475e), shown one
// by one by `EventWnd_set_event` 0x25f94 with the `help.txt` topic
// `event%02d`. The original's arguments are pointers or values; here every
// object is given by its index (the meaning of `a` and `b` is listed per
// code).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace oa::game {

enum class EventCode : std::int16_t {
  kResearchDone = 0,         // a = research item
  kBuildingComplete = 1,     // a = planet, b = planet item
  kRaceWantsToTalk = 2,      // a = race, b = its pending action
  kShipArrived = 3,          // a = ship slot, b = 0
  kFirstContact = 4,         // a = ship slot, b = race met
  kHostileShips = 5,         // a = ship slot, b = race at war
  kHostileColony = 6,        // a = ship slot, b = race at war
  kOthersAtWar = 7,          // a = ship slot, b = a race at war
  kUnknownShipEntered = 8,   // a = ship slot, b = race
  kHostileShipEntered = 9,   // a = ship slot, b = race
  kRaceExtinct = 10,         // a = race
  kShipScrapped = 11,        // a = the ship's hull size (as the original)
  kShipLost = 12,            // a = the ship's hull size, b = star
  kColonyInvaded = 13,       // a = planet, b = invading race
  kFreePopulation = 15,      // a = planet
  kEnteredOrbit = 16,        // a = ship slot, b = planet
  kShipBuilt = 17,           // a = ship slot, b = planet
  kShipRefitted = 18,        // a = ship slot, b = planet
  kRemainingMoves = 19,      // a = star, b = command to re-post (7 or 8)
  kSystemDiscovered = 20,    // a = ship slot, b = 0
  kBasicsKnown = 21,         // no argument
  kXenoDigDiscovery = 22,    // a = research item, b = planet
  kLaneTransmission = 23,    // a, b: see the poster
  kAbilityCharged = 24,      // no argument
  kLabWithoutResearch = 25,  // a = planet
  // peCustom: a managed planet built up for its strategy (a = planet).
  kPlanetBuiltUp = 26,
};

struct GameEvent {
  EventCode code{};
  std::int32_t a = 0;
  std::int32_t b = 0;
};

// The event queue (WinMgr +0x475a count, +0x475c read index, +0x475e
// records): the window manager shows the event at the read index, one per
// main-loop pass; the queue empties when the read index reaches the count,
// so the events already shown still take room until then.
class EventQueue {
 public:
  static constexpr std::size_t kCapacity = 32;

  // Appends an event; returns false (dropped) when the queue is full.
  bool post(EventCode code, std::int32_t a = 0, std::int32_t b = 0) {
    if (events_.size() >= kCapacity) {
      return false;
    }
    events_.push_back({code, a, b});
    return true;
  }
  // The queued events, those already shown included.
  const std::vector<GameEvent>& events() const { return events_; }
  // Whether no event waits to be shown (the original's count is 0).
  bool empty() const { return read_ == events_.size(); }
  // The next event to show; the queue must not be empty.
  const GameEvent& front() const { return events_.at(read_); }
  // Marks the next event shown (`WinMgr_show_game_events` 0x50f84): the
  // queue empties once every event is.
  void pop() {
    if (++read_ >= events_.size()) {
      clear();
    }
  }
  // Drops every event (the count set to 0 by `Cosmos_check_game_over`
  // 0x1fec4 and the loads).
  void clear() {
    events_.clear();
    read_ = 0;
  }
  // Removes and returns the events not shown yet, oldest first.
  std::vector<GameEvent> take() {
    std::vector<GameEvent> rest(events_.begin() + static_cast<std::ptrdiff_t>(read_), events_.end());
    clear();
    return rest;
  }

 private:
  std::vector<GameEvent> events_;
  std::size_t read_ = 0;
};

}  // namespace oa::game
