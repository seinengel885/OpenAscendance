#include "game/ai.h"

#include "game/antag_ai.h"
#include "game/antag_battle.h"
#include "game/race.h"

namespace oa::game {

namespace {

// `PATCH.EXE` v1.6.5: `Race_turn_update` 0x39b50 and `Race_battle_turn`
// 0x3c6d8 with the standard AI.
class StandardAi final : public StrategicAi {
 public:
  void race_turn_update(Session& session, Race& race) const override { oa::game::race_turn_update(session, race); }
  bool battle_turn(Session& session, Race& race, Star& star, BattleAction& action) const override {
    return race_battle_turn(session, race, star, action);
  }
};

// `ANTAG.EXE` v1.6.10: `Race_turn_update` 0x3b220 and `Race_battle_turn`
// 0x406c4.
class AntagonizerAi final : public StrategicAi {
 public:
  void race_turn_update(Session& session, Race& race) const override { antag_race_turn_update(session, race); }
  bool battle_turn(Session& session, Race& race, Star& star, BattleAction& action) const override {
    return antag_race_battle_turn(session, race, star, action);
  }
};

}  // namespace

const StrategicAi& strategic_ai(Variant variant) {
  static const StandardAi standard;
  static const AntagonizerAi antagonizer;
  return antagonizer_rules(variant) ? static_cast<const StrategicAi&>(antagonizer) : standard;
}

}  // namespace oa::game
