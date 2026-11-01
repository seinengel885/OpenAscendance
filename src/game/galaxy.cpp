#include "game/galaxy.h"

#include <cstring>
#include <deque>
#include <format>
#include <stdexcept>
#include <string_view>

#include "core/fixed_width.h"
#include "game/float_math.h"
#include "game/planet.h"
#include "game/race.h"
#include "game/tables.h"

namespace oa::game {

namespace {

// Throws for a failed `assert` of the original (an internal invariant).
void require(bool ok, std::string_view what) {
  if (!ok) {
    throw std::logic_error(std::format("original assertion failed: {}", what));
  }
}

std::uint8_t race_bit(std::size_t race) { return static_cast<std::uint8_t>(1u << race); }

std::int16_t max_lanes(const Star& star) { return kStarMaxLanes.at(static_cast<std::size_t>(star.type)); }

// Copies `text` and its NUL into `field` (`strcpy`: the tail keeps its bytes).
template <std::size_t N>
void copy_name(std::array<char, N>& field, std::string_view text) {
  require(text.size() < N, "name fits its field");
  std::memcpy(field.data(), text.data(), text.size());
  field[text.size()] = '\0';
}

// Appends `text` to the NUL-terminated `name` (`strcat`).
void append(std::span<char> name, std::string_view text) {
  std::size_t len = 0;
  while (len < name.size() && name[len] != '\0') {
    ++len;
  }
  require(len + text.size() < name.size(), "strcat fits its field");
  std::memcpy(name.data() + len, text.data(), text.size());
  name[len + text.size()] = '\0';
}

Lane* lane_at(Cosmos& cosmos, LaneIndex lane) { return &cosmos.lanes.at(static_cast<std::size_t>(lane)); }
Star& star_at(Cosmos& cosmos, StarIndex star) { return cosmos.stars.at(static_cast<std::size_t>(star)); }

// `Cosmos_add_lane` 0x1fcc4: appends a lane unless 148 exist or the pair is
// linked (the hop matrix doubles as the "linked" matrix).
LaneIndex add_lane(Cosmos& cosmos, StarIndex a, StarIndex b) {
  const std::int16_t ia = star_at(cosmos, a).index;
  const std::int16_t ib = star_at(cosmos, b).index;
  auto& linked_ab = cosmos.hops.at(static_cast<std::size_t>(ia)).at(static_cast<std::size_t>(ib));
  if (cosmos.lane_count == static_cast<std::int16_t>(kMaxLanes) || linked_ab != 0) {
    return kNone;
  }
  const LaneIndex index = cosmos.lane_count++;
  Lane& lane = cosmos.lanes[static_cast<std::size_t>(index)];
  lane.known_mask = 0;
  lane.star_a = a;
  lane.star_b = b;
  linked_ab = 1;
  cosmos.hops[static_cast<std::size_t>(ib)][static_cast<std::size_t>(ia)] = 1;
  return index;
}

// `Cosmos_link_nearest_stars` 0x1f290: each star with a free lane slot, in
// index order, links to its nearest unlinked, non-full star (strict `<` from
// 5000: the lowest index wins ties).
void link_nearest_stars(Cosmos& cosmos) {
  for (auto& row : cosmos.hops) {
    row.fill(0);
  }
  const std::size_t n = static_cast<std::size_t>(cosmos.star_count);
  for (std::size_t i = 0; i < n; ++i) {
    cosmos.hops[i][i] = 1;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Star& star = cosmos.stars[i];
    if (star.lane_count == max_lanes(star)) {
      continue;
    }
    std::int32_t best = 5000;
    StarIndex nearest = kNone;
    for (std::size_t j = 0; j < n; ++j) {
      const Star& other = cosmos.stars[j];
      if (cosmos.hops[i][j] == 0 && other.lane_count != max_lanes(other)) {
        const std::int32_t d = chopped_distance(star.position, other.position);
        if (d < best) {
          best = d;
          nearest = static_cast<StarIndex>(j);
        }
      }
    }
    if (nearest != kNone) {
      require(cosmos_connect_stars(cosmos, static_cast<StarIndex>(i), nearest) != kNone,
              "Cosmos_link_nearest_stars connects");
    }
  }
}

// `Cosmos_flood_cluster` 0x1f68c + `StarCluster_add_star` 0x1f890: adds the
// star to the current cluster, then its lanes' ends depth first.
void flood_cluster(Cosmos& cosmos, StarIndex s) {
  Star& star = star_at(cosmos, s);
  star.cluster = cosmos.cluster_count;
  require(cosmos.cluster_count <= 23, "at most 24 clusters");
  Cluster& cluster = cosmos.clusters[static_cast<std::size_t>(cosmos.cluster_count)];
  require(cluster.star_count <= 99, "at most 100 stars per cluster");
  cluster.stars[static_cast<std::size_t>(cluster.star_count++)] = s;
  for (std::int16_t k = 0; k < star.lane_count; ++k) {
    const Lane& lane = *lane_at(cosmos, star.lanes[static_cast<std::size_t>(k)]);
    if (star_at(cosmos, lane.star_a).cluster == -1) {
      flood_cluster(cosmos, lane.star_a);
    }
    if (star_at(cosmos, lane.star_b).cluster == -1) {
      flood_cluster(cosmos, lane.star_b);
    }
  }
}

// `Cosmos_build_star_clusters` 0x1f4a8: the connected components of the
// lane graph and their centroids.
void build_star_clusters(Cosmos& cosmos) {
  cosmos.cluster_count = 0;
  cosmos.clusters.fill(Cluster{});
  for (std::int16_t i = 0; i < cosmos.star_count; ++i) {
    if (star_at(cosmos, i).cluster == -1) {
      flood_cluster(cosmos, i);
      ++cosmos.cluster_count;
    }
  }
  std::int16_t total = 0;
  for (std::int16_t c = 0; c < cosmos.cluster_count; ++c) {
    Cluster& cluster = cosmos.clusters[static_cast<std::size_t>(c)];
    for (std::int16_t k = 0; k < cluster.star_count; ++k) {
      const Vec3& p = star_at(cosmos, cluster.stars[static_cast<std::size_t>(k)]).position;
      for (std::size_t a = 0; a < 3; ++a) {
        cluster.centroid[a] = f32(static_cast<double>(cluster.centroid[a]) + p[a]);
      }
    }
    const double inv = 1.0 / cluster.star_count;
    for (float& v : cluster.centroid) {
      v = f32(v * inv);
    }
    total = static_cast<std::int16_t>(total + cluster.star_count);
  }
  cosmos.mean_cluster_size = wrap_i16(chop(static_cast<double>(total) / cosmos.cluster_count));
}

// `Cosmos_link_clusters` 0x1f8fc: each cluster's star with the most free
// lane slots links (red link) to the nearest non-full, unlinked star of the
// cluster whose centroid is nearest. Clusters are not merged in the pass.
void link_clusters(Cosmos& cosmos) {
  require(cosmos.cluster_count != 0, "Cosmos_link_clusters has clusters");
  if (cosmos.cluster_count == 1) {
    return;
  }
  for (std::int16_t c = 0; c < cosmos.cluster_count; ++c) {
    const Cluster& cluster = cosmos.clusters[static_cast<std::size_t>(c)];
    StarIndex src = kNone;
    std::int16_t most_free = 0;
    for (std::int16_t k = 0; k < cluster.star_count; ++k) {
      const StarIndex s = cluster.stars[static_cast<std::size_t>(k)];
      const Star& star = star_at(cosmos, s);
      const std::int32_t free = max_lanes(star) - star.lane_count;
      if (most_free < free) {
        most_free = static_cast<std::int16_t>(free);
        src = s;
      }
    }
    require(src != kNone, "a cluster star has a free lane slot");
    const Star& from = star_at(cosmos, src);
    const Cluster* target = nullptr;
    std::int32_t best = 99999;
    for (std::int16_t o = 0; o < cosmos.cluster_count; ++o) {
      if (o == c) {
        continue;
      }
      const Cluster& other = cosmos.clusters[static_cast<std::size_t>(o)];
      const std::int32_t d = chop(length(sub(from.position, other.centroid)));
      if (d < best) {
        target = &other;
        best = d;
      }
    }
    require(target != nullptr, "a nearest cluster");
    StarIndex dst = kNone;
    best = 99999;
    for (std::int16_t k = 0; k < target->star_count; ++k) {
      const StarIndex s = target->stars[static_cast<std::size_t>(k)];
      const Star& star = star_at(cosmos, s);
      if (cosmos.hops.at(static_cast<std::size_t>(from.index)).at(static_cast<std::size_t>(star.index)) == 0) {
        const std::int32_t d = chopped_distance(from.position, star.position);
        if (max_lanes(star) != star.lane_count && d < best) {
          dst = s;
          best = d;
        }
      }
    }
    require(dst != kNone, "a star to link in the nearest cluster");
    const LaneIndex lane = cosmos_connect_stars(cosmos, src, dst);
    require(lane != kNone, "Cosmos_link_clusters connects");
    lane_at(cosmos, lane)->flags |= 1;
    star_at(cosmos, src).flags |= 1;
    star_at(cosmos, dst).flags |= 1;
  }
}

}  // namespace

void cosmos_generate(Session& session, std::int16_t stars, std::int16_t races, std::uint8_t species) {
  session.game->cosmos.battle_cursor = 0;
  session.grid = 32;
  cosmos_generate_stars(session, stars);
  cosmos_generate_backdrops(session, 9);
  cosmos_generate_lanes(session.game->cosmos);
  cosmos_new_galaxy(session, races, species);
}

void cosmos_generate_stars(Session& session, std::int16_t count) {
  require(count >= 1 && count <= static_cast<std::int16_t>(kMaxStars), "1..100 stars");
  Cosmos& cosmos = session.game->cosmos;
  std::array<StarName, kStarNameCount> names = session.tables->star_names;
  cosmos.star_count = count;
  std::int32_t threshold = count / 2;
  std::int32_t total_weight = 0;
  for (const std::int16_t w : kStarTypeWeights) {
    total_weight += w;
  }
  cosmos.planet_count = 0;
  cosmos.square_count = 0;
  cosmos.day = 0;
  cosmos.selected_star = 0;
  cosmos.selected_planet = 0;
  float radius = 440.0f;
  std::int32_t remaining = static_cast<std::int32_t>(kStarNameCount);
  for (std::int16_t i = 0; i < count; ++i) {
    // Not cleared: `flags` and `battle_done` (stale from earlier galaxies).
    Star& star = cosmos.stars[static_cast<std::size_t>(i)];
    star.owner_mask = 0;
    star.ships_mask = 0;
    star.lanes_known_mask = 0;
    star.explored_mask = 0;
    star.lane_count = 0;
    star.cluster = -1;
    star.planet_count = 0;
    star.index = i;
    const std::size_t k = static_cast<std::size_t>(session.rng.rand() % remaining);
    copy_name(star.name, names[k]);
    names[k] = names[static_cast<std::size_t>(remaining - 1)];
    if (threshold < i) {
      radius = f32(static_cast<double>(radius) + -75.0f);
      if (radius < 50.0f) {
        radius = 50.0f;
      }
      threshold += (count - i) / 2;
    }
    star.type = 12;
    --remaining;
    const std::int32_t pick = session.rng.rand() % total_weight + 1;
    std::int16_t cumulative = 0;
    for (std::size_t t = 0; t < kStarTypes; ++t) {
      cumulative = static_cast<std::int16_t>(cumulative + kStarTypeWeights[t]);
      if (pick <= cumulative) {
        star.type = static_cast<std::int32_t>(t);
        break;
      }
    }
    Vec3 p{};
    for (float& c : p) {
      c = static_cast<float>(session.rng.rand() % 1000 - 500);
    }
    normalize(p);
    for (std::size_t a = 0; a < 3; ++a) {
      star.position[a] = f32(static_cast<double>(p[a]) * radius);
    }
  }
}

void cosmos_generate_backdrops(Session& session, std::int16_t count) {
  Cosmos& cosmos = session.game->cosmos;
  if (count > static_cast<std::int16_t>(kMaxBackdrops)) {
    count = static_cast<std::int16_t>(kMaxBackdrops);
  }
  cosmos.backdrop_count = 0;
  const std::int32_t step = 360 / count;
  const std::int32_t spread = step * 3 / 4;
  const std::int32_t half = spread / 2;
  for (std::int32_t i = 0; i < count; ++i) {
    Backdrop& b = cosmos.backdrops[static_cast<std::size_t>(i)];
    b.frame = static_cast<std::uint8_t>(i);
    Vec3 p = {1000.0f, 300.0f, 0.0f};
    const std::int32_t angle = session.rng.rand() % spread + i * step - half;
    rotate_y(p, static_cast<float>(angle));
    const std::int32_t height = session.rng.rand() % 600 - 300;
    b.position = {p[0], f32(static_cast<double>(height) + p[1]), p[2]};
    ++cosmos.backdrop_count;
  }
}

void cosmos_generate_lanes(Cosmos& cosmos) {
  cosmos.lanes.fill(Lane{});
  cosmos.lane_count = 0;
  link_nearest_stars(cosmos);
  cosmos.cluster_count = 2;
  while (cosmos.cluster_count > 1) {
    for (std::int16_t i = 0; i < cosmos.star_count; ++i) {
      star_at(cosmos, i).cluster = -1;
    }
    build_star_clusters(cosmos);
    link_clusters(cosmos);
  }
}

LaneIndex cosmos_connect_stars(Cosmos& cosmos, StarIndex a, StarIndex b) {
  Star& sa = star_at(cosmos, a);
  Star& sb = star_at(cosmos, b);
  require(sa.lane_count >= 0 && sa.lane_count <= 5 && sb.lane_count >= 0 && sb.lane_count <= 5,
          "Cosmos_connect_stars lane counts");
  if (sa.lane_count == max_lanes(sa) || sb.lane_count == max_lanes(sb)) {
    return kNone;
  }
  const LaneIndex index = add_lane(cosmos, a, b);
  if (index == kNone) {
    return kNone;
  }
  sa.lanes[static_cast<std::size_t>(sa.lane_count++)] = index;
  sb.lanes[static_cast<std::size_t>(sb.lane_count++)] = index;
  // The end offset: the direction scaled by |d| / 880 * 1000 + 2000.
  const Vec3 d = sub(sb.position, sa.position);
  const double len = length(d);
  const double inv = 1.0 / len;
  const Vec3 u = {f32(d[0] * inv), f32(d[1] * inv), f32(d[2] * inv)};
  const float scaled = f32(len * 0.0011363636f);
  const float end = f32(static_cast<double>(scaled) * 1000.0f + 2000.0f);
  Lane& lane = cosmos.lanes[static_cast<std::size_t>(index)];
  for (std::size_t c = 0; c < 3; ++c) {
    lane.end_a[c] = f32(static_cast<double>(u[c]) * end);
    lane.end_b[c] = -lane.end_a[c];
  }
  return index;
}

void cosmos_new_galaxy(Session& session, std::int16_t races, std::uint8_t species) {
  require(races >= 2 && races <= static_cast<std::int16_t>(kMaxRaces), "2..7 races");
  Cosmos& cosmos = session.game->cosmos;
  const std::size_t race_count = static_cast<std::size_t>(races);
  cosmos.score = -1;
  cosmos.ending = 0;
  cosmos.race_count = races;
  for (std::int16_t i = 0; i < cosmos.star_count; ++i) {
    Star& star = star_at(cosmos, i);
    star.owner_mask = 0;
    star.ships_mask = 0;
    star.lanes_known_mask = 0;
    star.explored_mask = 0;
  }
  for (std::int16_t i = 0; i < cosmos.lane_count; ++i) {
    lane_at(cosmos, i)->known_mask = 0;
  }
  research_load(session);
  // 0xf647a / 0xf6488: the research projects and progress, cleared again.
  session.game->research.project.fill(-1);
  session.game->research.progress.fill(0);

  // Home stars, redrawn as a whole until they are spread enough.
  const std::int16_t min_spread = cosmos.star_count < 25 ? 450 : 600;
  cosmos.mean_home_distance = 0;
  for (int pass = 0; cosmos.mean_home_distance < min_spread; ++pass) {
    if (pass != 0) {
      for (std::size_t r = 0; r < race_count; ++r) {
        Star& home = star_at(cosmos, cosmos.races[r].home);
        home.owner_mask = 0;
        home.explored_mask = 0;
        for (std::int16_t k = 0; k < home.lane_count; ++k) {
          Lane& lane = *lane_at(cosmos, home.lanes[static_cast<std::size_t>(k)]);
          lane.known_mask = 0;
          star_at(cosmos, lane.star_a).lanes_known_mask = 0;
          star_at(cosmos, lane.star_b).lanes_known_mask = 0;
        }
      }
    }
    for (std::size_t r = 0; r < race_count;) {
      Race& race = cosmos.races[r];
      race.home = static_cast<StarIndex>(session.rng.rand() % cosmos.star_count);
      Star& home = star_at(cosmos, race.home);
      if (home.owner_mask != 0) {
        continue;  // taken: redraw for the same race
      }
      const std::uint8_t bit = race_bit(r);
      home.owner_mask = static_cast<std::uint8_t>(0x80 | bit);
      race.index = static_cast<std::uint8_t>(r);
      home.explored_mask |= bit;
      for (std::int16_t k = 0; k < home.lane_count; ++k) {
        Lane& lane = *lane_at(cosmos, home.lanes[static_cast<std::size_t>(k)]);
        lane.known_mask |= bit;
        star_at(cosmos, lane.star_a).lanes_known_mask |= bit;
        star_at(cosmos, lane.star_b).lanes_known_mask |= bit;
      }
      ++r;
    }
    cosmos_mean_home_distance(cosmos);
  }

  // Species, drawn and initialised race by race.
  for (std::size_t i = 0; i < race_count; ++i) {
    std::uint8_t sp = species;
    if (i != 0) {
      bool taken = true;
      while (taken) {
        sp = static_cast<std::uint8_t>(session.rng.rand() % static_cast<int>(kSpeciesCount));
        taken = false;
        for (std::size_t j = 0; j < i; ++j) {
          if (cosmos.races[j].species == sp) {
            taken = true;
            break;
          }
        }
      }
    }
    race_init(session, cosmos.races[i], static_cast<std::uint8_t>(i), sp);
  }
  cosmos_assign_colors(session, cosmos.races[0].color);

  // The atmosphere's shift of DECLARE_WAR..PROPOSE_ALLIANCE and
  // START_ATTITUDE (see `docs/09`: always the Neutral block in a normal new
  // game). Antagonizer: ANTAG 0x1e70c.
  using Shift = std::array<std::int16_t, 7>;
  static constexpr std::array<Shift, 3> kStandardShift = {
      Shift{-30, -20, -20, -20, -30, -20, 30}, Shift{0, 0, 0, 0, 0, 0, 0}, Shift{20, 20, 20, 20, 20, 30, -20}};
  static constexpr std::array<Shift, 3> kAntagonizerShift = {
      Shift{20, 20, 20, 20, 20, 30, -20}, Shift{30, 30, 30, 30, 30, 40, -30}, Shift{20, 20, 20, 20, 20, 30, -20}};
  // peCustom: Hostile twice the Antagonizer's (which equals its Peaceful).
  static constexpr std::array<Shift, 3> kPeCustomShift = {
      Shift{20, 20, 20, 20, 20, 30, -20}, Shift{30, 30, 30, 30, 30, 40, -30}, Shift{40, 40, 40, 40, 40, 50, -40}};
  const std::size_t atmosphere = static_cast<std::size_t>(cosmos.atmosphere);
  if (atmosphere < 3) {
    const Shift& shift = (session.game->variant == Variant::kPeCustom ? kPeCustomShift
                          : antagonizer_rules(session.game->variant) ? kAntagonizerShift
                                                                     : kStandardShift)[atmosphere];
    for (std::size_t i = 0; i < race_count; ++i) {
      Race& r = cosmos.races[i];
      std::int16_t* values[] = {&r.declare_war,     &r.accept_peace,     &r.propose_peace, &r.break_alliance,
                                &r.accept_alliance, &r.propose_alliance, &r.start_attitude};
      for (std::size_t k = 0; k < shift.size(); ++k) {
        *values[k] = wrap_i16(*values[k] + shift[k]);
      }
    }
  }

  cosmos.ship_count = 0;
  cosmos.selected_star = cosmos.races[0].home;
  for (Ship& ship : cosmos.ships) {
    ship.owner = -1;
    ship.location_kind = ShipLocation::kNone;
    ship.location = kNone;
  }

  // Abilities applied at the start.
  for (std::size_t i = 0; i < race_count; ++i) {
    Race& race = cosmos.races[i];
    const std::uint8_t bit = race_bit(i);
    if (race.ability == kSpeciesKambuchka && i == session.player_race) {
      for (std::size_t j = 0; j < race_count; ++j) {
        star_at(cosmos, cosmos.races[j].home).explored_mask |= bit;
      }
    } else if (race.ability == kSpeciesOculons) {
      for (std::int16_t l = 0; l < cosmos.lane_count; ++l) {
        lane_at(cosmos, l)->known_mask |= bit;
      }
    } else if (race.ability == kSpeciesHanshaks) {
      for (std::size_t j = 0; j < race_count; ++j) {
        if (j != i) {
          Race& other = cosmos.races[j];
          race.attitude[j] = race.start_attitude;
          race.relation[j] = kPeace;
          other.attitude[i] = other.start_attitude;
          other.relation[i] = kPeace;
        }
      }
    }
  }
}

std::int16_t cosmos_mean_home_distance(Cosmos& cosmos) {
  const std::size_t n = static_cast<std::size_t>(cosmos.race_count);
  std::array<double, kMaxRaces> mean{};
  cosmos.mean_home_distance = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Vec3& home = star_at(cosmos, cosmos.races[i].home).position;
    for (std::size_t j = 0; j < n; ++j) {
      if (j != i) {
        mean[i] = length(sub(home, star_at(cosmos, cosmos.races[j].home).position)) + mean[i];
      }
    }
    mean[i] = mean[i] / static_cast<double>(cosmos.race_count - 1);
    cosmos.mean_home_distance = wrap_i16(chop(static_cast<double>(cosmos.mean_home_distance) + mean[i]));
  }
  cosmos.mean_home_distance = static_cast<std::int16_t>(cosmos.mean_home_distance / cosmos.race_count);
  return cosmos.mean_home_distance;
}

void cosmos_assign_colors(Session& session, std::uint8_t color) {
  Cosmos& cosmos = session.game->cosmos;
  cosmos.races[0].color = color;
  for (std::size_t i = 1; i < static_cast<std::size_t>(cosmos.race_count); ++i) {
    std::uint8_t c = 0;
    bool taken = true;
    while (taken) {
      c = static_cast<std::uint8_t>(session.rng.rand() % 7);
      taken = false;
      for (std::size_t j = 0; j < i; ++j) {
        if (cosmos.races[j].color == c) {
          taken = true;
          break;
        }
      }
    }
    cosmos.races[i].color = c;
  }
}

void cosmos_generate_planets(Session& session) {
  Cosmos& cosmos = session.game->cosmos;
  std::size_t p = 0;  // the planet being created (from the first record)
  for (std::int16_t s = 0; s < cosmos.star_count; ++s) {
    Star& star = star_at(cosmos, s);
    std::int32_t n = session.rng.rand() % 5 + 1;
    std::int32_t home_race = -1;
    for (std::int16_t r = 0; r < cosmos.race_count; ++r) {
      if (cosmos.races[static_cast<std::size_t>(r)].home == s) {
        home_race = r;
        break;
      }
    }
    std::int32_t home_orbit = 0;
    if (home_race >= 0) {
      home_orbit = session.rng.rand() % n;
    }
    star.planet_count = static_cast<std::int16_t>(n);
    const float star_radius = kStarRadius.at(static_cast<std::size_t>(star.type));
    const std::int32_t step = 360 / n;
    std::int32_t angle = 0;
    for (std::int32_t k = 0; k < n; ++k, ++p, angle += step) {
      Planet& planet = cosmos.planets.at(p);
      star.planets[static_cast<std::size_t>(k)] = static_cast<PlanetIndex>(p);
      auto size = static_cast<std::uint16_t>(session.rng.rand() % 5);
      auto type = static_cast<std::uint16_t>(session.rng.rand() % 11);
      const bool home = home_race >= 0 && home_orbit == k;
      if (home) {
        const std::uint8_t species = cosmos.races[static_cast<std::size_t>(home_race)].species;
        size = kHomeSize.at(species);
        type = kHomeType.at(species);
      }
      planet.square_count = 0;
      planet.first_square = kNone;
      planet.self_managed = 0;
      planet.star = s;
      planet.orbit = static_cast<std::int16_t>(k);
      cosmos.square_count = static_cast<std::int16_t>(
          cosmos.square_count + planet_init_surface(session, planet, size, type, cosmos.square_count));
      require(cosmos.square_count <= static_cast<std::int16_t>(kMaxSquares), "at most 20000 squares");
      copy_name(planet.name, std::string(name_view(star.name)) + " ");
      append_roman_numeral(planet.name, k + 1);
      const float r =
          f32((static_cast<double>(kOrbitRadius[static_cast<std::size_t>(k)]) + star_radius) * 2000.0f * 0.011111111f);
      const float zero = f32(0.0 * r);
      Vec3 position = {r, zero, zero};
      rotate_y(position, static_cast<float>(angle));
      planet.position = position;
      snap_to_grid(planet.position, session.grid);  // Planet_set_position 0x34f14
      if (home) {
        planet.owner = static_cast<std::uint8_t>(home_race);
        planet.population = 2;
        planet_auto_build_item(session, planet, kItemColonyBase);
      }
    }
    cosmos.planet_count = static_cast<std::int16_t>(cosmos.planet_count + n);
    require(cosmos.planet_count <= 499, "at most 499 planets");
  }
}

void cosmos_compute_hops(Cosmos& cosmos) {
  require(cosmos.cluster_count == 1, "Cosmos_compute_hops: one cluster");
  for (auto& row : cosmos.hops) {
    row.fill(0xff);
  }
  const std::size_t n = static_cast<std::size_t>(cosmos.star_count);
  // Neighbour lists in lane order (at most 9 per star, asserted).
  std::array<std::array<std::uint8_t, 10>, kMaxStars> neighbours{};
  std::array<std::uint8_t, kMaxStars> degree{};
  for (std::int16_t l = 0; l < cosmos.lane_count; ++l) {
    const Lane& lane = cosmos.lanes[static_cast<std::size_t>(l)];
    const auto a = static_cast<std::size_t>(star_at(cosmos, lane.star_a).index & 0xff);
    const auto b = static_cast<std::size_t>(star_at(cosmos, lane.star_b).index & 0xff);
    neighbours.at(a).at(degree[a]++) = static_cast<std::uint8_t>(b);
    neighbours.at(b).at(degree[b]++) = static_cast<std::uint8_t>(a);
    require(degree[a] <= 9 && degree[b] <= 9, "at most 9 lanes per star");
  }
  for (std::size_t from = 0; from < n; ++from) {
    auto& row = cosmos.hops[from];
    row[from] = 0;
    std::deque<std::size_t> queue = {from};
    while (!queue.empty()) {
      const std::size_t s = queue.front();
      queue.pop_front();
      for (std::size_t k = 0; k < degree[s]; ++k) {
        const std::size_t t = neighbours[s][k];
        if (row[t] == 0xff) {
          row[t] = static_cast<std::uint8_t>(row[s] + 1);
          queue.push_back(t);
        }
      }
    }
    for (std::size_t to = 0; to < n; ++to) {
      require(row[to] != 0xff, "every star reachable");
    }
  }
}

void append_roman_numeral(std::span<char> name, std::int32_t n) {
  static constexpr std::array<std::string_view, 10> kHundreds = {"",  "C",  "CC",  "CCC",  "CD",
                                                                 "D", "DC", "DCC", "DCCC", "CM"};
  static constexpr std::array<std::string_view, 10> kTens = {"",  "X",  "XX",  "XXX",  "XL",
                                                             "L", "LX", "LXX", "LXXX", "XC"};
  static constexpr std::array<std::string_view, 10> kOnes = {"",  "I",  "II",  "III",  "IV",
                                                             "V", "VI", "VII", "VIII", "IX"};
  require(n >= 0, "append_roman_numeral of a non-negative number");
  // One M per started thousand above the first (so none for 1000, one for
  // 2000): the original's loop.
  if (n > 1000) {
    for (std::int32_t m = 1000; m <= 3999; m += 1000) {
      append(name, "M");
      if (m + 1000 >= n) {
        break;
      }
    }
  }
  append(name, kHundreds[static_cast<std::size_t>(n / 100 % 10)]);
  append(name, kTens[static_cast<std::size_t>(n / 10 % 10)]);
  append(name, kOnes[static_cast<std::size_t>(n % 10)]);
}

}  // namespace oa::game
