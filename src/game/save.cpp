#include "game/save.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "core/byte_reader.h"
#include "core/error.h"
#include "core/fixed_width.h"
#include "game/race.h"

namespace oa::game {

namespace {

// Record sizes written first and asserted by `Cosmos_load`: research, star,
// planet, race, lane, cluster, backdrop, cosmos.
constexpr std::array<std::int32_t, 8> kRecordSizes = {0x1d72, 0x60, 0x7b, 0x1ee, 0x27, 0x19e, 0xd, 0x354ef};
constexpr std::array<char, 4> kVariantTag = {'O', 'A', 'v', 't'};

// A cosmos array the raw pointers point into: offset in the cosmos object,
// element size and element count.
struct ArrayLayout {
  std::uint32_t offset;
  std::uint32_t stride;
  std::size_t count;
  std::string_view name;
};
constexpr ArrayLayout kStars{0xdb5, 0x60, kMaxStars, "star"};
constexpr ArrayLayout kLanes{0x3337, 0x27, kMaxLanes, "lane"};
constexpr ArrayLayout kSquares{0x49c5, 4, kMaxSquares, "square"};
constexpr ArrayLayout kPlanets{0x18247, 0x7b, kMaxPlanets, "planet"};

// Writes the records of a save, converting indices to the writer's runtime
// addresses. Methods mirror `Reader`'s, so one function per record
// describes both directions.
class Writer {
 public:
  static constexpr bool kReading = false;

  explicit Writer(RawAddress cosmos) : cosmos_(cosmos) {}

  std::vector<std::uint8_t>& bytes() { return out_; }
  std::size_t pos() const { return out_.size(); }
  void expect(std::size_t start, std::size_t size) const {
    if (out_.size() != start + size) {
      throw std::logic_error(std::format("record of {} bytes written as {}", size, out_.size() - start));
    }
  }

  void u8(const std::uint8_t& v) { out_.push_back(v); }
  void i8(const std::int8_t& v) { out_.push_back(static_cast<std::uint8_t>(v)); }
  void u16(const std::uint16_t& v) {
    out_.push_back(static_cast<std::uint8_t>(v));
    out_.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void i16(const std::int16_t& v) { u16(static_cast<std::uint16_t>(v)); }
  void u32(const std::uint32_t& v) {
    for (int shift = 0; shift < 32; shift += 8) {
      out_.push_back(static_cast<std::uint8_t>(v >> shift));
    }
  }
  void i32(const std::int32_t& v) { u32(static_cast<std::uint32_t>(v)); }
  void f32(const float& v) { u32(std::bit_cast<std::uint32_t>(v)); }
  void vec3(const Vec3& v) {
    for (const float& c : v) {
      f32(c);
    }
  }
  template <std::size_t N>
  void chars(const std::array<char, N>& s) {
    for (const char c : s) {
      out_.push_back(static_cast<std::uint8_t>(c));
    }
  }
  template <typename E>
  void enum8(const E& e) {
    out_.push_back(static_cast<std::uint8_t>(e));
  }
  // A pointer into `array`, written as the runtime address (0 for null).
  void pointer(const std::int16_t& index, const ArrayLayout& array) { u32(address(index, array)); }
  // A pointer written as the element's index (`i32`).
  void index32(const std::int16_t& index) { i32(index); }

  RawAddress address(std::int16_t index, const ArrayLayout& array) const {
    return index == kNone ? 0 : cosmos_ + array.offset + static_cast<std::uint32_t>(index) * array.stride;
  }

 private:
  RawAddress cosmos_;
  std::vector<std::uint8_t> out_;
};

// Reads the records of a save, converting the writer's runtime addresses to
// indices.
class Reader {
 public:
  static constexpr bool kReading = true;

  explicit Reader(std::span<const std::uint8_t> data) : in_(data, "save") {}

  ByteReader& in() { return in_; }
  void set_cosmos(RawAddress cosmos) { cosmos_ = cosmos; }
  void set_foreign_cosmos(std::optional<RawAddress> cosmos) { foreign_ = cosmos; }
  std::size_t pos() const { return in_.pos(); }
  void expect(std::size_t start, std::size_t size) const {
    if (in_.pos() != start + size) {
      in_.fail(std::format("record of {} bytes read as {}", size, in_.pos() - start));
    }
  }

  void u8(std::uint8_t& v) { v = in_.u8(); }
  void i8(std::int8_t& v) { v = in_.i8(); }
  void u16(std::uint16_t& v) { v = in_.u16(); }
  void i16(std::int16_t& v) { v = in_.i16(); }
  void u32(std::uint32_t& v) { v = in_.u32(); }
  void i32(std::int32_t& v) { v = in_.i32(); }
  void f32(float& v) { v = std::bit_cast<float>(in_.u32()); }
  void vec3(Vec3& v) {
    for (float& c : v) {
      f32(c);
    }
  }
  template <std::size_t N>
  void chars(std::array<char, N>& s) {
    const auto b = in_.bytes(N);
    std::memcpy(s.data(), b.data(), N);
  }
  template <typename E>
  void enum8(E& e) {
    e = static_cast<E>(in_.u8());
  }
  void pointer(std::int16_t& index, const ArrayLayout& array) { index = resolve(in_.u32(), array); }
  void index32(std::int16_t& index) { index = checked_index(in_.i32(), "index"); }

  // `stale`: a slot past its list's count, `kNone` when it points nowhere
  // in the cosmos.
  std::int16_t resolve(RawAddress raw, const ArrayLayout& array, bool stale = false) const {
    if (raw == 0) {
      return kNone;
    }
    const auto element = [&](RawAddress cosmos) -> std::optional<std::int16_t> {
      const std::int64_t off = static_cast<std::int64_t>(raw) - cosmos - array.offset;
      if (off < 0 || off % array.stride != 0 || off / array.stride >= static_cast<std::int64_t>(array.count)) {
        return std::nullopt;
      }
      return static_cast<std::int16_t>(off / array.stride);
    };
    if (const auto own = element(cosmos_)) {
      return *own;
    }
    if (foreign_) {
      if (const auto other = element(*foreign_)) {
        return *other;
      }
    }
    if (stale) {
      return kNone;
    }
    in_.fail(std::format("pointer 0x{:x} is not a {} of the cosmos at 0x{:x}", raw, array.name, cosmos_));
  }
  std::int16_t checked_index(std::int32_t v, std::string_view what) const {
    if (v < -1 || v > 0x7fff) {
      in_.fail(std::format("{} {} out of range", what, v));
    }
    return static_cast<std::int16_t>(v);
  }
  std::int32_t count(std::size_t limit, std::string_view what) {
    const std::int32_t n = in_.i32();
    if (n < 0 || static_cast<std::size_t>(n) > limit) {
      in_.fail(std::format("{} count {} out of 0..{}", what, n, limit));
    }
    return n;
  }

 private:
  ByteReader in_;
  RawAddress cosmos_ = kPatchCosmosAddress;
  std::optional<RawAddress> foreign_;
};

// The records. `T` is the record type, const when writing.

template <typename IO, typename T>
void header_record(IO& io, T& h) {
  const std::size_t start = io.pos();
  io.i32(h.day);
  io.i32(h.race_count);
  io.i32(h.player_race);
  for (auto* a : {&h.species, &h.colors, &h.player_relations}) {
    for (auto& v : *a) {
      io.i32(v);
    }
  }
  io.i32(h.fleet);
  io.i32(h.colonies);
  io.i32(h.discoveries);
  for (auto& b : h.stale) {
    io.u8(b);
  }
  io.expect(start, 0x74);
}

template <typename IO, typename T>
void research_record(IO& io, T& r) {
  const std::size_t start = io.pos();
  io.u16(r.count);
  for (auto& item : r.items) {
    io.chars(item.name);
    io.u16(item.cost);
    for (auto& b : item.prereqs) {
      io.u8(b);
    }
    for (auto& b : item.leads_to) {
      io.u8(b);
    }
    io.u8(item.known_mask);
    io.u8(item.unseen_mask);
    io.u8(item.category);
  }
  for (auto& v : r.project) {
    io.i16(v);
  }
  for (auto& v : r.progress) {
    io.u16(v);
  }
  io.i32(r.self_managed);
  io.i32(r.basics_done);
  io.expect(start, 0x1d72);
}

// A list of pointers followed by its count (`i16`). The slots past the
// count keep whatever the saving process had there: pointers into the
// cosmos of another process (the tutorials' games) are read as none.
template <typename IO, typename L, typename C>
void pointer_list(IO& io, L& list, C& count, const ArrayLayout& array) {
  if constexpr (IO::kReading) {
    std::array<std::uint32_t, std::tuple_size_v<L>> raw{};
    for (auto& v : raw) {
      io.u32(v);
    }
    io.i16(count);
    for (std::size_t i = 0; i < raw.size(); ++i) {
      list[i] = io.resolve(raw[i], array, static_cast<std::int64_t>(i) >= count);
    }
  } else {
    for (auto& v : list) {
      io.pointer(v, array);
    }
    io.i16(count);
  }
}

template <typename IO, typename T>
void star_record(IO& io, T& s) {
  const std::size_t start = io.pos();
  io.i32(s.type);
  io.i16(s.index);
  io.i16(s.cluster);
  io.vec3(s.position);
  io.u8(s.owner_mask);
  io.u8(s.ships_mask);
  io.u8(s.lanes_known_mask);
  io.u8(s.explored_mask);
  io.u32(s.flags);
  io.chars(s.name);
  pointer_list(io, s.lanes, s.lane_count, kLanes);
  pointer_list(io, s.planets, s.planet_count, kPlanets);
  io.i32(s.battle_done);
  io.expect(start, 0x60);
}

template <typename IO, typename T>
void square_record(IO& io, T& s) {
  io.u8(s.color);
  io.u8(s.item);
  io.u16(s.flags);
}

template <typename IO, typename T>
void planet_record(IO& io, T& p) {
  const std::size_t start = io.pos();
  io.vec3(p.position);
  io.i16(p.star);
  io.i16(p.orbit);
  io.pointer(p.first_square, kSquares);
  for (auto* v : {&p.size, &p.type, &p.surface_squares, &p.square_count, &p.free_surface, &p.free_orbital,
                  &p.free_black, &p.unknown_22}) {
    io.u16(*v);
  }
  io.chars(p.name);
  io.u16(p.population);
  io.i16(p.industry);
  io.i16(p.research);
  io.i16(p.prosperity);
  for (auto* v : {&p.max_population, &p.used_population, &p.growth, &p.progress, &p.project_square}) {
    io.u16(*v);
  }
  io.u8(p.project_item);
  io.u16(p.base_population);
  io.u8(p.owner);
  io.u8(p.weapon_count);
  io.u8(p.shield_count);
  io.i32(p.self_managed);
  io.i32(p.unknown_5e);
  io.i32(p.protected_turn);
  io.u8(p.flags);
  io.i32(p.acquired_day);
  io.u32(p.fire_square);
  io.u32(p.fire_target);
  io.u32(p.fire_result);
  io.i32(p.ship_cost);
  io.expect(start, 0x7b);
}

// The ship's location, saved as an index by kind (`Ship_serialize`): a
// planet for 1..3, a star (its `index`) for 4, a lane for 5; the original
// refuses any other kind ("InVoid").
template <typename IO, typename T>
void ship_location(IO& io, T& ship) {
  switch (ship.location_kind) {
    case ShipLocation::kBuilding:
    case ShipLocation::kRefit:
    case ShipLocation::kOrbit:
    case ShipLocation::kSystem:
    case ShipLocation::kLane:
      io.index32(ship.location);
      break;
    default:
      if constexpr (IO::kReading) {
        io.in().fail(std::format("ship in no location (kind {})", static_cast<int>(ship.location_kind)));
      } else {
        throw std::logic_error(std::format("ship in no location (kind {})", static_cast<int>(ship.location_kind)));
      }
  }
}

// The ship's order target: a star index for 1, a planet index for 2 and 3,
// the raw pointer for 0 and 7 (`Ship::stale_destination`, cleared by
// `load_game`); 4..6 are battle-only.
template <typename IO, typename T>
void ship_destination(IO& io, T& ship) {
  switch (ship.destination_kind) {
    case ShipOrder::kNothing:
    case ShipOrder::kGoSpace:
      io.u32(ship.stale_destination);
      if constexpr (IO::kReading) {
        ship.destination = kNone;
      }
      break;
    case ShipOrder::kGoStar:
    case ShipOrder::kColonize:
    case ShipOrder::kOrbitPlanet:
      io.index32(ship.destination);
      break;
    default:
      if constexpr (IO::kReading) {
        io.in().fail(std::format("battle-only ship order {}", static_cast<int>(ship.destination_kind)));
      } else {
        throw std::logic_error(std::format("battle-only ship order {}", static_cast<int>(ship.destination_kind)));
      }
  }
}

template <typename IO, typename T>
void ship_record(IO& io, T& s) {
  const std::size_t start = io.pos();
  for (auto* v : {&s.weapon_damage, &s.shield_max, &s.lane_speed, &s.hyper_speed, &s.drive_speed, &s.drive_base,
                  &s.generator_power, &s.drive_power, &s.scanner_level, &s.cloakers, &s.has_colonizer}) {
    io.i32(*v);
  }
  for (auto& v : s.reserved_2c) {
    io.i32(v);
  }
  io.chars(s.name);
  io.i32(s.built_day);
  io.i16(s.owner);
  io.enum8(s.location_kind);
  ship_location(io, s);
  io.enum8(s.destination_kind);
  ship_destination(io, s);
  io.u8(s.action);
  io.u32(s.action_slot);
  io.u8(s.target_kind);
  io.u32(s.target);
  io.vec3(s.target_point);
  io.vec3(s.action_point);
  for (auto* v : {&s.action_result, &s.power, &s.hit_points, &s.shield, &s.moves, &s.max_hit_points}) {
    io.i32(*v);
  }
  io.u16(s.battle_flags);
  io.vec3(s.position);
  io.u8(s.hull_size);
  for (auto& slot : s.slots) {
    io.i8(slot.gizmo);
    io.i16(slot.uses_left);
    io.i32(slot.toggled);
  }
  io.i32(s.slot_count);
  io.i32(s.gizmo_count);
  io.expect(start, 0x162);
}

template <typename IO, typename T>
void race_record(IO& io, T& r) {
  const std::size_t start = io.pos();
  io.u8(r.index);
  io.u8(r.species);
  io.u8(r.color);
  io.i32(r.extinct);
  io.index32(r.home);
  pointer_list(io, r.targets, r.target_count, kStars);
  io.u8(r.ability);
  io.i32(r.ability_charge);
  for (auto& b : r.unused_1a2) {
    io.u8(b);
  }
  for (auto* v : {&r.ai_build_threshold_a, &r.ai_build_threshold_b, &r.ai_unknown_1ae, &r.ship_patience}) {
    io.i16(*v);
  }
  for (auto& v : r.attitude) {
    io.i16(v);
  }
  for (auto& v : r.relation) {
    io.u8(v);
  }
  for (auto* v :
       {&r.neg_player_turns, &r.declare_war, &r.accept_peace, &r.propose_peace, &r.break_alliance, &r.accept_alliance,
        &r.propose_alliance, &r.start_attitude, &r.bio_period, &r.bio_max_positive, &r.bio_max_negative}) {
    io.i16(*v);
  }
  io.f32(r.bio_sin_x);
  io.f32(r.bio_sin_half_x);
  io.f32(r.bio_sin_twice_x);
  io.u8(r.pending_action);
  io.u32(r.pending_action_arg);
  io.expect(start, 0x1ee);
}

template <typename IO, typename T>
void lane_record(IO& io, T& l) {
  const std::size_t start = io.pos();
  io.index32(l.star_a);
  io.index32(l.star_b);
  io.vec3(l.end_a);
  io.vec3(l.end_b);
  io.u8(l.known_mask);
  io.i16(l.visited);
  io.u32(l.flags);
  io.expect(start, 0x27);
}

// The first `star_count` star pointers are saved as indices, the others raw.
template <typename IO, typename T>
void cluster_record(IO& io, T& c) {
  const std::size_t start = io.pos();
  std::array<std::uint32_t, kMaxStars> raw{};
  if constexpr (!IO::kReading) {
    for (std::size_t k = 0; k < kMaxStars; ++k) {
      raw[k] = static_cast<std::int16_t>(k) < c.star_count ? static_cast<std::uint32_t>(std::int32_t{c.stars[k]})
                                                           : io.address(c.stars[k], kStars);
    }
  }
  for (auto& v : raw) {
    io.u32(v);
  }
  io.i16(c.star_count);
  io.vec3(c.centroid);
  if constexpr (IO::kReading) {
    for (std::size_t k = 0; k < kMaxStars; ++k) {
      c.stars[k] = static_cast<std::int16_t>(k) < c.star_count
                       ? io.checked_index(static_cast<std::int32_t>(raw[k]), "cluster star")
                       : io.resolve(raw[k], kStars);
    }
  }
  io.expect(start, 0x19e);
}

template <typename IO, typename T>
void backdrop_record(IO& io, T& b) {
  io.u8(b.frame);
  io.vec3(b.position);
}

}  // namespace

SaveImage parse_save(std::span<const std::uint8_t> data, std::optional<RawAddress> foreign_cosmos) {
  Reader r(data);
  r.set_foreign_cosmos(foreign_cosmos);
  ByteReader& in = r.in();
  SaveImage image;
  header_record(r, image.header);
  for (const std::int32_t size : kRecordSizes) {
    if (in.i32() != size) {
      in.fail("record sizes differ from the v1.6.5 ones");
    }
  }
  // The squares' runtime address is only known from planet 0's pointer:
  // find it ahead (stars and squares come before the planets).
  {
    ByteReader ahead = in;
    ahead.skip(4 * 8 + 0x1d72);
    const std::int32_t stars = ahead.i32();
    ahead.skip(static_cast<std::size_t>(std::max(stars, 0)) * 0x60);
    const std::int32_t squares = ahead.i32();
    ahead.skip(static_cast<std::size_t>(std::max(squares, 0)) * 4);
    if (ahead.i32() > 0) {
      ahead.skip(0x10);
      image.cosmos_address = ahead.u32() - kSquares.offset;
    }
    r.set_cosmos(image.cosmos_address);
  }
  image.day = in.i32();
  r.index32(image.selected_star);
  r.index32(image.selected_planet);
  image.ending = static_cast<std::uint8_t>(in.i32());
  image.score = in.i32();
  for (std::int32_t& d : image.last_trade_days) {
    d = in.i32();
  }
  research_record(r, image.research);
  image.stars.resize(static_cast<std::size_t>(r.count(kMaxStars, "star")));
  for (Star& s : image.stars) {
    star_record(r, s);
  }
  image.squares.resize(static_cast<std::size_t>(r.count(kMaxSquares, "square")));
  for (Square& s : image.squares) {
    square_record(r, s);
  }
  image.planets.resize(static_cast<std::size_t>(r.count(kMaxPlanets, "planet")));
  for (Planet& p : image.planets) {
    planet_record(r, p);
  }
  image.ships.resize(static_cast<std::size_t>(r.count(kMaxShips, "ship")));
  for (auto& [slot, ship] : image.ships) {
    slot = in.i32();
    if (slot < 0 || slot >= static_cast<std::int32_t>(kMaxShips)) {
      in.fail(std::format("ship slot {} out of range", slot));
    }
    ship_record(r, ship);
  }
  image.races.resize(static_cast<std::size_t>(r.count(kMaxRaces, "race")));
  for (Race& race : image.races) {
    race_record(r, race);
  }
  image.lanes.resize(static_cast<std::size_t>(r.count(kMaxLanes, "lane")));
  for (Lane& l : image.lanes) {
    lane_record(r, l);
  }
  for (auto& row : image.hops) {
    const auto b = in.bytes(row.size());
    std::memcpy(row.data(), b.data(), row.size());
  }
  image.clusters.resize(static_cast<std::size_t>(r.count(kMaxClusters, "cluster")));
  for (Cluster& c : image.clusters) {
    cluster_record(r, c);
  }
  image.backdrops.resize(static_cast<std::size_t>(r.count(kMaxBackdrops, "backdrop")));
  for (Backdrop& b : image.backdrops) {
    backdrop_record(r, b);
  }
  const auto windows = in.bytes(kWindowStateSize);
  image.window_state.assign(windows.begin(), windows.end());
  if (!in.at_end()) {
    const auto tag = in.bytes(kVariantTag.size());
    if (!std::equal(tag.begin(), tag.end(), kVariantTag.begin())) {
      in.fail("unknown data after the window state");
    }
    const std::uint32_t variant = in.u32();
    if (variant != static_cast<std::uint32_t>(Variant::kAntagonizer) &&
        variant != static_cast<std::uint32_t>(Variant::kPeCustom)) {
      in.fail(std::format("unknown game variant {}", variant));
    }
    image.variant = static_cast<Variant>(variant);
    if (image.variant == Variant::kPeCustom) {
      image.pe_custom_version = in.u32();
      if (image.pe_custom_version == 0 || image.pe_custom_version > kPeCustomSaveVersion) {
        in.fail(std::format("peCustom save version {} (this build loads up to {}): saved by a newer build?",
                            image.pe_custom_version, kPeCustomSaveVersion));
      }
      // Data appended by later versions is read here, by version.
    }
    if (!in.at_end()) {
      in.fail("data after the variant trailer");
    }
  }
  return image;
}

std::vector<std::uint8_t> serialize_save(const SaveImage& image) {
  if (image.window_state.size() != kWindowStateSize) {
    throw std::logic_error(
        std::format("window state of {} bytes, not {}", image.window_state.size(), kWindowStateSize));
  }
  Writer w(image.cosmos_address);
  header_record(w, image.header);
  for (const std::int32_t size : kRecordSizes) {
    w.i32(size);
  }
  w.i32(image.day);
  w.index32(image.selected_star);
  w.index32(image.selected_planet);
  w.i32(static_cast<std::int8_t>(image.ending));  // the byte, sign-extended
  w.i32(image.score);
  for (const std::int32_t d : image.last_trade_days) {
    w.i32(d);
  }
  research_record(w, image.research);
  const auto count = [&w](std::size_t n) { w.i32(static_cast<std::int32_t>(n)); };
  count(image.stars.size());
  for (const Star& s : image.stars) {
    star_record(w, s);
  }
  count(image.squares.size());
  for (const Square& s : image.squares) {
    square_record(w, s);
  }
  count(image.planets.size());
  for (const Planet& p : image.planets) {
    planet_record(w, p);
  }
  count(image.ships.size());
  for (const auto& [slot, ship] : image.ships) {
    w.i32(slot);
    ship_record(w, ship);
  }
  count(image.races.size());
  for (const Race& r : image.races) {
    race_record(w, r);
  }
  count(image.lanes.size());
  for (const Lane& l : image.lanes) {
    lane_record(w, l);
  }
  for (const auto& row : image.hops) {
    for (const std::uint8_t h : row) {
      w.u8(h);
    }
  }
  count(image.clusters.size());
  for (const Cluster& c : image.clusters) {
    cluster_record(w, c);
  }
  count(image.backdrops.size());
  for (const Backdrop& b : image.backdrops) {
    backdrop_record(w, b);
  }
  std::vector<std::uint8_t>& out = w.bytes();
  out.insert(out.end(), image.window_state.begin(), image.window_state.end());
  if (image.variant != Variant::kStandard) {
    out.insert(out.end(), kVariantTag.begin(), kVariantTag.end());
    w.u32(static_cast<std::uint32_t>(image.variant));
  }
  if (image.variant == Variant::kPeCustom) {
    w.u32(kPeCustomSaveVersion);
  }
  return std::move(out);
}

SaveImage save_game(const Session& session) {
  const Game& game = *session.game;
  const Cosmos& cosmos = game.cosmos;
  SaveImage image;
  image.cosmos_address = session.cosmos_address != 0       ? session.cosmos_address
                         : antagonizer_rules(game.variant) ? kAntagCosmosAddress
                                                           : kPatchCosmosAddress;
  // The header: the game's summary over the stale stack bytes.
  image.header = session.stale_header;
  image.header.day = cosmos.day;
  image.header.race_count = cosmos.race_count;
  image.header.player_race = session.player_race;
  const Race& player = cosmos.races.at(session.player_race);
  for (std::size_t i = 0; i < static_cast<std::size_t>(cosmos.race_count); ++i) {
    image.header.species[i] = cosmos.races[i].species;
    image.header.colors[i] = cosmos.races[i].color;
    image.header.player_relations[i] = static_cast<std::int8_t>(player.relation[i]);
  }
  image.header.fleet = race_ship_count(cosmos, player);
  image.header.colonies = race_count_planets(cosmos, player);
  image.header.discoveries = race_count_discoveries(game.research, player);

  image.day = cosmos.day;
  image.selected_star = cosmos.stars.at(static_cast<std::size_t>(cosmos.selected_star)).index;
  image.selected_planet = cosmos.selected_planet;
  image.ending = cosmos.ending;
  image.score = cosmos.score;
  image.last_trade_days = game.last_trade_days;
  image.research = game.research;
  const auto first = [](const auto& array, std::int16_t n) { return std::vector(array.begin(), array.begin() + n); };
  image.stars = first(cosmos.stars, cosmos.star_count);
  image.squares = first(cosmos.squares, cosmos.square_count);
  image.planets = first(cosmos.planets, cosmos.planet_count);
  for (std::size_t slot = 0; slot < kMaxShips; ++slot) {
    if (cosmos.ships[slot].owner != -1) {
      image.ships.emplace_back(static_cast<std::int32_t>(slot), cosmos.ships[slot]);
    }
  }
  if (image.ships.size() != static_cast<std::size_t>(cosmos.ship_count)) {
    throw std::logic_error("Cosmos_save: the used ship slots differ from the ship count");
  }
  image.races = first(cosmos.races, cosmos.race_count);
  image.lanes = first(cosmos.lanes, cosmos.lane_count);
  // The lane ends and cluster stars are written as the stars' own `index`.
  for (Lane& lane : image.lanes) {
    lane.star_a = cosmos.stars.at(static_cast<std::size_t>(lane.star_a)).index;
    lane.star_b = cosmos.stars.at(static_cast<std::size_t>(lane.star_b)).index;
  }
  for (Race& race : image.races) {
    race.home = cosmos.stars.at(static_cast<std::size_t>(race.home)).index;
  }
  image.hops = cosmos.hops;
  image.clusters = first(cosmos.clusters, cosmos.cluster_count);
  for (Cluster& cluster : image.clusters) {
    for (std::int16_t k = 0; k < cluster.star_count; ++k) {
      StarIndex& s = cluster.stars[static_cast<std::size_t>(k)];
      s = cosmos.stars.at(static_cast<std::size_t>(s)).index;
    }
  }
  image.backdrops = first(cosmos.backdrops, cosmos.backdrop_count);
  image.window_state = session.window_state;
  image.variant = game.variant;
  if (game.variant == Variant::kPeCustom) {
    image.pe_custom_version = kPeCustomSaveVersion;
  }
  return image;
}

void load_game(Session& session, const SaveImage& image) {
  Game& game = *session.game;
  Cosmos& cosmos = game.cosmos;
  const auto require = [](bool ok, std::string_view what) {
    if (!ok) {
      throw DataError(std::format("save: {}", what));
    }
  };
  cosmos.day = image.day;
  cosmos.selected_star = image.selected_star;
  cosmos.selected_planet = image.selected_planet;
  cosmos.ending = image.ending;
  cosmos.score = image.score;
  game.last_trade_days = image.last_trade_days;
  // Research_serialize reads the tree once through Research_load first.
  game.research = image.research;
  session.research_tree_loaded = true;
  // The names shown are the tables', in the language played: a save keeps
  // those of the language it was written in.
  for (std::size_t i = 0; i < game.research.items.size(); ++i) {
    game.research.items[i].name = session.tables->research.items[i].name;
  }

  cosmos.star_count = static_cast<std::int16_t>(image.stars.size());
  for (std::size_t i = 0; i < image.stars.size(); ++i) {
    Star& star = cosmos.stars[i];
    star = image.stars[i];
    star.lane_count = 0;
    star.planet_count = 0;
  }
  cosmos.square_count = static_cast<std::int16_t>(image.squares.size());
  std::copy(image.squares.begin(), image.squares.end(), cosmos.squares.begin());
  cosmos.planet_count = static_cast<std::int16_t>(image.planets.size());
  std::int32_t first_square = 0;
  for (std::size_t i = 0; i < image.planets.size(); ++i) {
    Planet& planet = cosmos.planets[i];
    planet = image.planets[i];
    require(planet.star >= 0 && planet.star < cosmos.star_count, "planet star out of range");
    require(planet.orbit >= 0 && planet.orbit <= 4, "planet orbit out of range");
    Star& star = cosmos.stars[static_cast<std::size_t>(planet.star)];
    star.planets[static_cast<std::size_t>(planet.orbit)] = static_cast<PlanetIndex>(i);
    ++star.planet_count;
    planet.first_square = static_cast<SquareIndex>(first_square);
    first_square += planet.square_count;
  }
  for (Ship& ship : cosmos.ships) {
    ship.owner = -1;
    ship.location_kind = ShipLocation::kNone;
  }
  cosmos.ship_count = static_cast<std::int16_t>(image.ships.size());
  for (const auto& [slot, ship] : image.ships) {
    Ship& loaded = cosmos.ships[static_cast<std::size_t>(slot)];
    loaded = ship;
    // `Ship_serialize` 0x46294 clears the pointer of orders 0 and 7.
    loaded.stale_destination = 0;
  }
  cosmos.race_count = static_cast<std::int16_t>(image.races.size());
  for (std::size_t i = 0; i < image.races.size(); ++i) {
    cosmos.races[i] = image.races[i];
    cosmos.races[i].target_count = 0;
  }
  cosmos.lane_count = static_cast<std::int16_t>(image.lanes.size());
  for (std::size_t i = 0; i < image.lanes.size(); ++i) {
    Lane& lane = cosmos.lanes[i];
    lane = image.lanes[i];
    for (const StarIndex s : {lane.star_a, lane.star_b}) {
      require(s >= 0 && static_cast<std::size_t>(s) < kMaxStars, "lane star out of range");
      Star& star = cosmos.stars[static_cast<std::size_t>(s)];
      require(star.lane_count < static_cast<std::int16_t>(kLanesPerStar), "too many lanes at a star");
      star.lanes[static_cast<std::size_t>(star.lane_count++)] = static_cast<LaneIndex>(i);
    }
  }
  cosmos.hops = image.hops;
  // Cosmos_load reads the clusters into a local buffer only.
  cosmos.cluster_count = static_cast<std::int16_t>(image.clusters.size());
  cosmos.backdrop_count = static_cast<std::int16_t>(image.backdrops.size());
  std::copy(image.backdrops.begin(), image.backdrops.end(), cosmos.backdrops.begin());

  // An original file has no variant trailer: `ANTAG.EXE`'s saves are known
  // by its cosmos address and continue as Antagonizer games.
  game.variant = image.variant == Variant::kStandard && image.cosmos_address == kAntagCosmosAddress
                     ? Variant::kAntagonizer
                     : image.variant;
  session.stale_header = image.header;
  session.window_state = image.window_state;
}

}  // namespace oa::game
