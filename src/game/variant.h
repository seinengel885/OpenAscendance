// Game variants (`docs/HANDOFF.md`): the rules and AI of `PATCH.EXE` v1.6.5
// (Standard) or of `ANTAG.EXE` (Antagonizer), or peCustom (this fork's own
// variant: the Antagonizer's rules, changed step by step), chosen for a new
// game and kept in its saves (`game/save.h`). The list is open-ended: a later
// variant gets an enum value and an entry of `kVariants`.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::game {

// The values are written to the saves: never change them. peCustom's is far
// from the small values a later upstream variant would take.
enum class Variant : std::uint32_t { kStandard = 0, kAntagonizer = 1, kPeCustom = 0x50450001 };

struct VariantInfo {
  Variant variant;
  // The name in the settings file and on the command line.
  std::string_view key;
  // The name shown to the player.
  std::string_view label;
};

// Every variant, in the order the options screen lists them.
inline constexpr std::array<VariantInfo, 3> kVariants{{
    {Variant::kStandard, "standard", "Standard"},
    {Variant::kAntagonizer, "antagonizer", "Antagonizer"},
    {Variant::kPeCustom, "pecustom", "peCustom"},
}};

// Whether `variant` plays by the Antagonizer's rules and AI: the Antagonizer
// and peCustom, which starts from them. A rule of peCustom's own checks
// `Variant::kPeCustom` instead.
constexpr bool antagonizer_rules(Variant variant) {
  return variant == Variant::kAntagonizer || variant == Variant::kPeCustom;
}

// Returns the entry of `variant` (the first one, Standard, for a value
// outside the list).
constexpr const VariantInfo& variant_info(Variant variant) {
  for (const VariantInfo& v : kVariants) {
    if (v.variant == variant) {
      return v;
    }
  }
  return kVariants.front();
}

// Returns the variant whose key is `key`, nothing for an unknown key.
constexpr std::optional<Variant> parse_variant(std::string_view key) {
  for (const VariantInfo& v : kVariants) {
    if (v.key == key) {
      return v.variant;
    }
  }
  return std::nullopt;
}

}  // namespace oa::game
