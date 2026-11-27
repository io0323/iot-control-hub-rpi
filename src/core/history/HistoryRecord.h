// 履歴の1件（基本設計 第7章「履歴DB」、FR-13）。core が作り、history スレッド（adapters/history）が
// SQLite に書く。core は SQL を知らない。時刻はすべて UTC のミリ秒。
//
// 捨てる順（区分。open-questions #41）：キューや書き込み失敗時の保持（1万件）があふれたら、
// 区分の小さいものから、同じ区分では古いものから捨てる。
//   info のイベント → ルールの発火 → 指令の途中の状態遷移 → DIAG → デバイスの状態 → warning のイベント
//   → 要求の途中の行 → critical のイベント → モードの変化 → 指令と要求の最終状態
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace gw::core::history {

enum class Tier : std::uint8_t {
  kInfoEvent = 0,
  kRuleFiring,
  kCommandTransition,  // 指令の途中の状態遷移
  kDiag,
  kDeviceStatus,
  kWarningEvent,
  kRequestProgress,  // 要求の途中の行（accepted）
  kCriticalEvent,
  kModeChange,
  kFinal,  // 指令と要求の最終状態
};
inline constexpr int kTierCount = 10;

// events（受信・発行したイベント）
struct EventRow {
  std::string msg_id;
  std::string device_id;
  std::string code;
  std::string severity;
  std::string payload;  // JSON
  std::int64_t ts_ms = 0;
};
// devices（台帳。再起動後の初期状態）
struct DeviceRow {
  std::string device_id;
  std::string transport;
  std::string channels;   // JSON（[{channel, unit, period_ms}]）
  std::string actuators;  // JSON
  std::string status;     // online・suspect・offline・unknown・unseen
  std::int64_t seen_ms = 0;
};
// device_status_log
struct DeviceStatusRow {
  std::string device_id;
  std::string from_status;
  std::string to_status;
  std::string reason;
  std::int64_t ts_ms = 0;
};
// rule_firings（発火と解除。open-questions #45）
struct RuleFiringRow {
  std::string rule_id;
  std::string state_from;
  std::string state_to;
  std::string trigger;  // JSON
  std::int64_t ts_ms = 0;
};
// commands（指令とその状態。最新で上書き）
struct CommandRow {
  std::string cmd_id;
  std::string device_id;
  std::string op;
  std::string args;    // JSON
  std::string origin;  // JSON
  std::string priority;
  std::string state;
  int retries = 0;
  std::int64_t created_ms = 0;
  std::int64_t updated_ms = 0;
};
// command_transitions（監査用）
struct CommandTransitionRow {
  std::string cmd_id;
  std::string from_state;
  std::string to_state;
  std::string detail;
  std::int64_t ts_ms = 0;
};
// requests（作品04 と gwctl の要求とその結果。最新で上書き）
struct RequestRow {
  std::string request_id;
  std::string source;  // 要求元のデバイス（app-01）
  std::string kind;    // manual・schedule
  std::string body;    // JSON（request の payload）
  std::string result;  // accepted・rejected・done・failed
  std::string reason;
  std::string cmd_id;
  std::int64_t ts_ms = 0;  // 受け付けた時刻
};
// system_mode_log
struct ModeRow {
  std::string from_mode;
  std::string to_mode;
  std::string reason;
  std::int64_t ts_ms = 0;
};
// uart_diag（open-questions #1、v0.2.2 #37 の15バイト）
struct DiagRow {
  std::string device_id;
  std::string boot_id;
  std::int64_t received_ms = 0;
  std::int64_t uptime_ms = 0;
  int reset_reason = 0;
  int reset_count = 0;
  int rx_crc_errors = 0;
  int rx_frame_errors = 0;
  int max_loop_us = 0;
  int overruns = 0;
};

using Row = std::variant<EventRow, DeviceRow, DeviceStatusRow, RuleFiringRow, CommandRow,
                         CommandTransitionRow, RequestRow, ModeRow, DiagRow>;

struct HistoryRecord {
  Row row;
  Tier tier = Tier::kFinal;
};

[[nodiscard]] Tier event_tier(std::string_view severity);
[[nodiscard]] bool is_final_state(std::string_view state);  // 指令・要求の終端
[[nodiscard]] HistoryRecord record(Row row);  // 行の種類と内容から区分を決める

}  // namespace gw::core::history
