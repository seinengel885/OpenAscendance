// peCustom's industry and prosperity with a tenth (`game/planet.h`): the
// output curve `sum ** 0.85` cut to tenths, the tenths carried day by day
// and the excess of a finished project or inhabitant carried over, and the
// days shown for a project or the growth the days it really takes.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include "game/planet.h"
#include "game/planet_plan_sim.h"
#include "game/race_ai.h"
#include "game/ship.h"
#include "game_oracle.h"
#include "plan_rig.h"

namespace oa::game {
namespace {

using plan_test::PlanRig;

constexpr std::uint8_t kFactory = 0;
constexpr std::uint8_t kAgriplot = 1;
constexpr std::uint8_t kRed = 2;
constexpr std::uint8_t kGreen = 3;

// A peCustom colony (seed 7, medium) the player manages by hand.
PlanRig& hand_colony(PlanRig& rig) {
  REQUIRE(rig.ready());
  rig.planet().self_managed = 0;
  return rig;
}

// The industry or prosperity of the built squares before the curve.
std::int32_t output_sum(PlanRig& rig, bool industry) {
  const GameTables& t = *rig.session().tables;
  std::int32_t sum = 0;
  for (const Square& sq : planet_squares(rig.session().game->cosmos, rig.planet())) {
    if (sq.item == kNoItem || (sq.flags & kSquareBuilt) == 0) {
      continue;
    }
    const PlanItem& it = t.planitems.items.at(sq.item);
    const std::int32_t v = industry ? it.industry : it.prosperity;
    sum += v != 0 ? v + (sq.color == (industry ? kRed : kGreen) ? 1 : 0) : 0;
  }
  return sum;
}

// An empty surface square of a colour without `item`'s bonus where `item`
// may go now.
std::uint16_t plain_square(PlanRig& rig, std::uint8_t item) {
  const std::span<const Square> sq = planet_squares(rig.session().game->cosmos, rig.planet());
  for (std::uint16_t i = 0; i < rig.planet().surface_squares; ++i) {
    const std::uint8_t color = sq[i].color & 7u;
    if (sq[i].item == kNoItem && color != kRed && color != kGreen && (sq[i].color & kSquareRuins) == 0 &&
        planet_can_place_item(rig.session(), rig.planet(), item, i)) {
      return i;
    }
  }
  return kNoSquare;
}

// Places `item` built on a plain square.
void build_now(PlanRig& rig, std::uint8_t item) {
  const std::uint16_t square = plain_square(rig, item);
  REQUIRE(square != kNoSquare);
  REQUIRE(planet_set_square_item(rig.session(), rig.planet(), square, item, SquareMode::kPlaceBuilt));
}

// Days until the project on `square` is built, at most `limit`.
int days_to_build(PlanRig& rig, std::uint16_t square, int limit = 400) {
  for (int d = 1; d <= limit; ++d) {
    rig.day();
    if ((planet_squares(rig.session().game->cosmos, rig.planet())[square].flags & kSquareBuilt) != 0) {
      return d;
    }
  }
  return -1;
}

TEST_CASE("peCustom's output curve: Colony Base 1.0, with a Factory 1.8", "[pecustom][output]") {
  CHECK(planet_output_tenths(0) == 0);
  CHECK(planet_output_tenths(1) == 10);
  CHECK(planet_output_tenths(2) == 18);
  CHECK(planet_output_tenths(3) == 25);
  CHECK(planet_output_tenths(10) == 70);
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  hand_colony(rig);
  build_now(rig, kFactory);
  const Planet& p = rig.planet();
  const std::int32_t sum = output_sum(rig, true);
  CAPTURE(sum);
  CHECK(planet_industry_tenths(p) == planet_output_tenths(sum));
  CHECK(p.industry == planet_output_tenths(sum) / 10);
  CHECK(p.industry_tenths == planet_output_tenths(sum) % 10);
}

TEST_CASE("a Factory at industry 1.8 takes 17 days, as shown and as the planner expects", "[pecustom][output]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  hand_colony(rig);
  if (output_sum(rig, true) < 2) {
    build_now(rig, kFactory);
  }
  REQUIRE(output_sum(rig, true) == 2);
  Planet& p = rig.planet();
  REQUIRE(planet_industry_tenths(p) == 18);
  const std::uint16_t square = plain_square(rig, kFactory);
  REQUIRE(square != kNoSquare);
  REQUIRE(planet_set_square_item(rig.session(), p, square, kFactory, SquareMode::kProject));
  // Shown: the planet screen's days.
  CHECK(planet_project_eta(rig.session(), p) == 17);
  CHECK(planet_build_days(p, t->planitems.items.at(kFactory).cost) == 17);
  // The planner's copy.
  const plan_detail::Neighbours n = plan_detail::surface_neighbours(t->planitems, p);
  plan_detail::Sim sim(rig.session(), p, n);
  int sim_days = 0;
  while (sim.planet.project_item != kNoItem && sim_days < 400) {
    sim.day();
    ++sim_days;
  }
  CHECK(sim_days == 17);
  // The game's.
  CHECK(days_to_build(rig, square) == 17);
}

TEST_CASE("a finished project's excess carries over, at most a day's output", "[pecustom][output]") {
  std::uint16_t counter = 29;
  std::uint16_t carry = 5;
  CHECK(planet_add_tenths(counter, carry, 18, 300));  // 313 of 300
  CHECK(counter == 0);
  CHECK(carry == 13);
  // Not done: whole units to the counter, the tenths kept.
  counter = 10;
  carry = 7;
  CHECK_FALSE(planet_add_tenths(counter, carry, 18, 300));
  CHECK(counter == 12);
  CHECK(carry == 5);
  // A project cheaper than the progress made: a day's output carried.
  counter = 100;
  carry = 0;
  CHECK(planet_add_tenths(counter, carry, 18, 300));
  CHECK(counter == 0);
  CHECK(carry == 18);
}

TEST_CASE("the population grows with the prosperity's tenths, as shown", "[pecustom][output]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  hand_colony(rig);
  build_now(rig, kAgriplot);
  Planet& p = rig.planet();
  REQUIRE(p.population < p.max_population);
  p.growth = 0;
  p.growth_carry = 0;
  const std::int32_t rate = planet_prosperity_tenths(p);
  CAPTURE(output_sum(rig, false), rate);
  REQUIRE(rate % 10 != 0);
  const std::int32_t expected = (10 * t->planitems.pop_growth + rate - 1) / rate;
  CHECK(planet_growth_days(rig.session(), p) == expected);
  const std::uint16_t population = p.population;
  int days = 0;
  while (p.population == population && days < 400) {
    rig.day();
    ++days;
  }
  CHECK(days == expected);
}

TEST_CASE("a ship takes the days the planet shows", "[pecustom][output]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  hand_colony(rig);
  // Room and workers for a Factory, the Shipyard and the crew.
  rig.planet().base_population = static_cast<std::uint16_t>(rig.planet().base_population + 6);
  planet_recalc_output(rig.session(), rig.planet());
  rig.planet().population = rig.planet().max_population;
  build_now(rig, kFactory);
  rig.learn(kItemShipyard);
  rig.learn(kItemShip);
  const std::uint16_t yard = planet_pick_random_free_square(rig.session(), rig.planet(), kItemShipyard);
  REQUIRE(yard != kNoSquare);
  // Placing it built is refused in orbit (an original bug): put there.
  Square& yard_square = planet_squares(rig.session().game->cosmos, rig.planet())[yard];
  yard_square.item = kItemShipyard;
  yard_square.flags = kSquareBuilt;
  planet_recalc_output(rig.session(), rig.planet());
  Session& s = rig.session();
  Planet& p = rig.planet();
  REQUIRE(planet_industry_tenths(p) % 10 != 0);
  // The start's ships scrapped: room for one more.
  for (Ship& ship : s.game->cosmos.ships) {
    if (ship.owner == static_cast<std::int8_t>(p.owner)) {
      ship_destroy(s, ship);
    }
  }
  const std::uint16_t square = planet_auto_place_item(s, p, kItemShip, 0);
  REQUIRE(square != kNoSquare);
  Ship* ship = race_ai_new_ship(s, s.game->cosmos.races.at(p.owner), p);
  REQUIRE(ship != nullptr);
  planet_set_square_ship(s, p, square, ship_index(s.game->cosmos, *ship));
  const std::int32_t cost = static_cast<std::int32_t>(planet_project_cost(s, p, square, kNoItem) & 0xffff);
  // The ship design window's days, and the planet screen's.
  const std::int32_t shown = planet_build_days(p, cost);
  CAPTURE(cost, planet_industry_tenths(p));
  CHECK(planet_project_eta(s, p) == shown);
  int days = 0;
  while (p.project_item == kItemShip && days < 2000) {
    rig.day();
    ++days;
  }
  CHECK(days == shown);
}

TEST_CASE("the AI's boost multiplies the industry's tenths", "[pecustom][output]") {
  OA_REQUIRE_TABLES(t);
  PlanRig rig(*t, 7, 2);
  hand_colony(rig);
  build_now(rig, kFactory);
  Session& s = rig.session();
  Planet& p = rig.planet();
  const std::int32_t plain = planet_output_tenths(output_sum(rig, true));
  p.owner = static_cast<std::uint8_t>((s.player_race + 1) % s.game->cosmos.race_count);
  s.game->cosmos.atmosphere = Atmosphere::kNeutral;
  planet_recalc_output(s, p);
  CAPTURE(plain);
  CHECK(planet_industry_tenths(p) == static_cast<std::int32_t>(plain * 1.3));
}

}  // namespace
}  // namespace oa::game
