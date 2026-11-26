// UART 上の値 ⇔ 共通仕様上の値（第7章「値の対応表」）。対応する Python：iotspec.uart.mapping
// 対応表にない値は UnknownValueError。変換する側は「値の形式不正」として破棄する（第5章）。
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gw::core::uart {

class UnknownValueError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// 共通仕様側の値が null のこともある（reason 0 → null）
using CommonValue = std::optional<std::string_view>;

class ValueTable {
 public:
  ValueTable(std::string_view name, std::vector<std::pair<int, CommonValue>> pairs)
      : name_(name), pairs_(std::move(pairs)) {}

  // UART の値 → 共通仕様の値（mapping.ValueTable.to_common）
  [[nodiscard]] CommonValue to_common(int value) const {
    for (const auto& [uart, common] : pairs_) {
      if (uart == value) {
        return common;
      }
    }
    throw UnknownValueError(std::string{name_} + ": 対応表にない UART の値 " +
                            std::to_string(value));
  }

  // 共通仕様の値 → UART の値（mapping.ValueTable.to_uart）
  [[nodiscard]] int to_uart(CommonValue value) const {
    for (const auto& [uart, common] : pairs_) {
      if (common == value) {
        return uart;
      }
    }
    throw UnknownValueError(std::string{name_} + ": 対応表にない値 " +
                            std::string{value.value_or("null")});
  }

  [[nodiscard]] std::string_view name() const { return name_; }
  [[nodiscard]] const std::vector<std::pair<int, CommonValue>>& pairs() const { return pairs_; }

 private:
  std::string_view name_;
  std::vector<std::pair<int, CommonValue>> pairs_;
};

[[nodiscard]] const ValueTable& quality_table();
[[nodiscard]] const ValueTable& status_table();
[[nodiscard]] const ValueTable& reason_table();
[[nodiscard]] const ValueTable& event_code_table();
[[nodiscard]] const ValueTable& severity_table();
[[nodiscard]] const ValueTable& reset_reason_table();

// 必ず文字列になる表（quality など）の変換
[[nodiscard]] std::string to_common_string(const ValueTable& table, int value);

}  // namespace gw::core::uart
