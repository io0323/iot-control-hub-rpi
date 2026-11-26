// 優先度（共通仕様 第4章「優先度」、基本設計 第6章「調停」）。
// safety ＞ manual ＞ auto ＞ schedule。値が小さいほど優先する。
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace gw::core::command {

enum class Priority : std::uint8_t {
  kSafety = 0,
  kManual = 1,
  kAuto = 2,
  kSchedule = 3,
};

inline constexpr std::array<std::pair<Priority, std::string_view>, 4> kPriorityNames{{
    {Priority::kSafety, "safety"},
    {Priority::kManual, "manual"},
    {Priority::kAuto, "auto"},
    {Priority::kSchedule, "schedule"},
}};

[[nodiscard]] constexpr std::string_view to_string(Priority p) {
  for (const auto& [value, name] : kPriorityNames) {
    if (value == p) {
      return name;
    }
  }
  return "auto";
}

[[nodiscard]] constexpr std::optional<Priority> parse_priority(std::string_view name) {
  for (const auto& [value, n] : kPriorityNames) {
    if (n == name) {
      return value;
    }
  }
  return std::nullopt;
}

// a が b 以上に優先する（同じ優先度を含む。同じ優先度では新しい方が勝つ）
[[nodiscard]] constexpr bool at_least(Priority a, Priority b) {
  return static_cast<std::uint8_t>(a) <= static_cast<std::uint8_t>(b);
}

}  // namespace gw::core::command
