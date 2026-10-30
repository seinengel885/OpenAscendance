#include "game/tables.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

#include "assets/static_strings.h"
#include "assets/tokens.h"
#include "core/error.h"
#include "core/fixed_width.h"

namespace oa::game {

namespace {

// Returns the offset just past the next `fgets(buf, size)` line read from
// `pos`: up to and including a '\n', or `size - 1` bytes; `pos` itself at
// the end of the data.
std::size_t fgets_end(std::span<const std::uint8_t> data, std::size_t pos, std::size_t size) {
  std::size_t end = pos;
  while (end < data.size() && end - pos < size - 1) {
    if (data[end++] == '\n') {
      break;
    }
  }
  return end;
}

// Returns the offset past the first `fgets` line (buffer `size`) starting
// with '#': the end of the free comment of `gizmos.txt` and `planitem.txt`.
std::size_t skip_comment(std::span<const std::uint8_t> data, std::size_t size, std::string_view context) {
  std::size_t pos = 0;
  while (pos < data.size()) {
    const std::size_t end = fgets_end(data, pos, size);
    if (data[pos] == '#') {
      return end;
    }
    pos = end;
  }
  throw DataError(std::format("{}: no line starting with '#'", context));
}

// Returns `token` with '^' replaced by spaces, checked to fit a `size`-byte
// NUL-terminated field.
std::string caret_name(const std::string& token, std::size_t size, std::string_view context) {
  if (token.size() >= size) {
    throw DataError(std::format("{}: name '{}' longer than {} characters", context, token, size - 1));
  }
  return replace_with_spaces(token, '^');
}

}  // namespace

void set_research_name(ResearchItem& item, std::string_view name) {
  if (name.size() >= item.name.size()) {
    throw DataError(std::format("research item name '{}' longer than {} characters", name, item.name.size() - 1));
  }
  item.name.fill('\0');
  std::memcpy(item.name.data(), name.data(), name.size());
}

PlanItemTable parse_planitems(std::span<const std::uint8_t> data) {
  constexpr std::string_view kContext = "planitem.txt";
  // The comment is read with fgets(buf, 0xc3).
  TokenReader in(data.subspan(skip_comment(data, 0xc3, kContext)), kContext);
  PlanItemTable t;
  t.pop_growth = in.integer();
  for (auto& weights : t.type_weights) {
    for (std::uint8_t& w : weights) {
      w = wrap_u8(in.integer());
    }
  }
  for (auto* grid : {&t.column_start, &t.column_height}) {
    for (auto& row : *grid) {
      for (std::uint8_t& v : row) {
        v = wrap_u8(in.integer());
      }
    }
  }
  if (in.integer() != static_cast<int>(PlanItemTable::kItems)) {
    in.fail("the item count is not 39");
  }
  const int surface = in.integer();
  const int orbital = in.integer();
  t.surface_count = wrap_u16(surface);
  t.orbital_count = wrap_u16(orbital);
  if (t.surface_count + t.orbital_count > PlanItemTable::kItems) {
    in.fail("more surface and orbital items than items");
  }
  for (PlanItem& item : t.items) {
    item.name = caret_name(in.word(), 30, kContext);
    item.industry = wrap_i8(in.integer());
    item.research = wrap_i8(in.integer());
    item.prosperity = wrap_i8(in.integer());
    item.max_population = wrap_i8(in.integer());
    item.used_population = wrap_i8(in.integer());
    item.cost = wrap_u16(in.integer());
    item.research_requirement = wrap_u8(in.integer());
    // At most 8 flags; after 8 the terminator is not read.
    for (int n = 0; n < 8; ++n) {
      const int flag = in.integer();
      if (flag == 0xff) {
        break;
      }
      item.flags = static_cast<std::uint16_t>(item.flags | (1u << (flag & 0x1f)));
    }
  }
  return t;
}

std::array<Gizmo, kGizmoCount> parse_gizmos(std::span<const std::uint8_t> data) {
  constexpr std::string_view kContext = "gizmos.txt";
  TokenReader in(data.subspan(skip_comment(data, 200, kContext)), kContext);
  std::array<Gizmo, kGizmoCount> gizmos{};
  for (Gizmo& g : gizmos) {
    g.name = caret_name(in.word(), 0x32, kContext);
    g.category = wrap_u8(in.integer());
    g.power = in.integer();
    g.range = in.integer();
    g.level = in.integer();
    g.uses = in.integer();
    g.industry = in.integer();
    g.special1 = in.integer();
    g.special2 = in.integer();
    int value = in.integer();
    g.research_requirement = wrap_u8(value);
    // The flags follow until 255 (no count limit).
    for (value = in.integer(); value != 0xff; value = in.integer()) {
      g.flags = static_cast<std::uint16_t>(g.flags | (1u << (value & 0x1f)));
    }
    switch (g.category) {
      case 0:
        g.weapon_damage = g.level;
        break;
      case 1:
        g.shield_strength = g.level;
        break;
      case 2:
        g.drive_speed = g.level;
        break;
      case 3:
        g.scanner_level = g.level;
        break;
      case 4:
        g.generator_power = g.power;
        break;
      default:
        break;
    }
  }
  return gizmos;
}

ResearchTree parse_restree(std::span<const std::uint8_t> data) {
  constexpr std::string_view kContext = "restree.txt";
  TokenReader in(data, kContext);
  ResearchTree tree;
  for (ResearchItem& item : tree.items) {
    item.leads_to[0] = 0xff;
  }
  tree.count = std::min<std::uint16_t>(wrap_u16(in.integer()), kMaxResearchItems);
  // The cost multiplier of item i > 10 is 2i - 20.
  for (int i = 0; i < tree.count; ++i) {
    ResearchItem& item = tree.items[static_cast<std::size_t>(i)];
    set_research_name(item, caret_name(in.word(), item.name.size(), kContext));
    item.category = wrap_u8(in.integer());
    std::int32_t cost = in.integer();
    if (i > 10) {
      cost = wrap_i32(static_cast<std::int64_t>(cost) * (2 * i - 20));
    }
    item.cost = wrap_u16(std::min(cost, 65000));
    // At most 5 prerequisites; after 5 the terminator is not read.
    for (std::uint8_t& prereq : item.prereqs) {
      const int value = in.integer();
      prereq = wrap_u8(value);
      if (value == 0xff) {
        break;
      }
    }
  }
  // Each prerequisite lists the item among its first 4 `leads_to` slots.
  for (int i = 0; i < tree.count; ++i) {
    for (const std::uint8_t prereq : tree.items[static_cast<std::size_t>(i)].prereqs) {
      if (prereq == 0xff) {
        break;
      }
      if (prereq >= kMaxResearchItems) {
        in.fail(std::format("prerequisite {} out of range", prereq));
      }
      auto& leads_to = tree.items[prereq].leads_to;
      for (std::size_t k = 0; k < 4; ++k) {
        if (leads_to[k] == 0xff) {
          leads_to[k + 1] = 0xff;
          leads_to[k] = wrap_u8(i);
          break;
        }
      }
    }
  }
  return tree;
}

std::array<StarName, kStarNameCount> parse_star_names(std::span<const std::uint8_t> data) {
  constexpr std::string_view kContext = "names.txt";
  TokenReader in(data, kContext);
  std::array<StarName, kStarNameCount> names;
  for (std::size_t i = 0; i < names.size(); ++i) {
    // The original reads a token while the file position is before the end;
    // trailing blanks alone would leave stale stack bytes (not reproduced).
    names[i] = in.at_end() ? std::format("XM{:02}", i) : in.word();
    if (names[i].size() >= sizeof(Star::name)) {
      in.fail(std::format("star name '{}' longer than 15 characters", names[i]));
    }
  }
  return names;
}

DipValues parse_dip_values(std::span<const std::uint8_t> data) {
  constexpr std::string_view kContext = "RACExx.DIP";
  // `read_content_line` 0x2f578 (fgets of 200) until a line starting with
  // DIPVALUES (strncmp of 9); the values follow.
  std::size_t pos = 0;
  for (;;) {
    if (pos >= data.size()) {
      throw DataError(std::format("{}: no DIPVALUES line", kContext));
    }
    const std::size_t end = fgets_end(data, pos, 200);
    const std::string_view line(reinterpret_cast<const char*>(data.data() + pos), end - pos);
    pos = end;
    if (line.starts_with("DIPVALUES")) {
      break;
    }
  }
  TokenReader in(data.subspan(pos), kContext);
  const auto value = [&in] { return wrap_i16(in.keyword_integer()); };
  DipValues d;
  d.declare_war = value();
  d.accept_peace = value();
  d.propose_peace = value();
  d.break_alliance = value();
  d.accept_alliance = value();
  d.propose_alliance = value();
  if (!(d.declare_war < d.accept_peace && d.accept_peace < d.propose_peace && d.propose_peace < d.break_alliance &&
        d.break_alliance < d.accept_alliance && d.accept_alliance < d.propose_alliance)) {
    in.fail("the attitude thresholds do not increase");
  }
  d.start_attitude = value();
  d.ship_patience = value();
  d.bio_period = value();
  d.bio_max_positive = value();
  d.bio_max_negative = value();
  d.bio_sin_x = value();
  d.bio_sin_half_x = value();
  d.bio_sin_twice_x = value();
  d.neg_player_turns = value();
  return d;
}

GameTables load_game_tables(const GameFiles& files) {
  GameTables t;
  t.planitems = parse_planitems(files.load("planitem.txt"));
  for (PlanItem& item : t.planitems.items) {
    item.name = files.text("planitem.txt", item.name);
  }
  t.gizmos = parse_gizmos(files.load("gizmos.txt"));
  for (Gizmo& g : t.gizmos) {
    g.name = files.text("gizmos.txt", g.name);
  }
  t.research = parse_restree(files.load("restree.txt"));
  for (ResearchItem& item : t.research.items) {
    set_research_name(item, files.text("restree.txt", name_view(item.name)));
  }
  t.star_names = parse_star_names(files.load("names.txt"));
  for (std::size_t s = 0; s < t.dip.size(); ++s) {
    t.dip[s] = parse_dip_values(files.load(std::format("RACE{:02}.DIP", s)));
  }
  const std::vector<std::string> strings = parse_static_strings(files.load("static.txt"));
  if (strings.size() < kHullNameString + kHullCount) {
    throw DataError("static.txt: no hull names");
  }
  for (std::size_t h = 0; h < kHullCount; ++h) {
    t.hull_names[h] = files.text("static.txt", strings[kHullNameString + h]);
  }
  t.logo_gif = files.load("data\\logo.gif");
  return t;
}

}  // namespace oa::game
