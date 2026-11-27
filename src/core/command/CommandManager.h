// 指令管理（基本設計 第6章「指令のライフサイクル」「送信の制限」、FR-09）。
// 対応する Python：iotspec.sim.devices.gw_01_lite（_send_uart_cmds・_transmit・_on_ack）
//
// - 状態：ISSUED → SENT → ACCEPTED → DONE。ほかに REJECTED・FAILED・TIMED_OUT（終端）
// - ACK を2秒待ち、同じ cmd_id（UART は同じ cmd_ref）で最大3回再送する。上限で TIMED_OUT
// - UART（作品01）は毎秒1件（再送を含み、再送を優先）・未完了4件まで。超えた分は送信待ち
// - 送信待ちの置き換えは、同じ所有者（owner）・同じ対象どうしだけ（open-questions #21）
// - expires_at は最初の送信の前だけ検査する（#43）。過ぎていたら送らず REJECTED（expired）。
//   UART では mcu-c-01 名義の cmd/ack を作らない（#40：呼び出し側は cmd/ack を発行しない）
// - offline のデバイス宛はその場で FAILED（offline）。復帰後にまとめて送らない
// - リースの更新（keep）：lease_s の3分の1ごとに新しい cmd_id で送り直す（Phase 4 のルールが使う）
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/common/Clock.h"
#include "core/common/Envelope.h"
#include "core/common/Ids.h"
#include "core/uart/Frames.h"
#include "core/uart/UartBridge.h"

namespace gw::core::command {

enum class CmdState : std::uint8_t {
  kIssued,
  kSent,
  kAccepted,
  kDone,
  kRejected,
  kFailed,
  kTimedOut,
};
[[nodiscard]] std::string_view to_string(CmdState s);
[[nodiscard]] bool is_terminal(CmdState s);

enum class Route : std::uint8_t { kUart, kMqtt };

// COMMAND_FAILED の detail.reason（open-questions #25）
inline constexpr std::string_view kReasonOffline = "offline";
inline constexpr std::string_view kReasonTimeout = "timeout";
inline constexpr std::string_view kReasonExpired = "expired";

struct CommandRecord {  // NOLINT(bugprone-exception-escape)：JSON の既定値の構築は投げない
  std::string cmd_id;
  std::string target;
  Json payload;  // cmd の payload（cmd_id を含む）
  Route route = Route::kMqtt;
  CmdState state = CmdState::kIssued;
  int attempts = 0;  // 送信回数（初回を含む）
  std::optional<SteadyTime> sent_at;
  std::optional<std::uint16_t> cmd_ref;
  std::string owner;   // 置き換えの単位（空なら置き換えない）
  std::string reason;  // REJECTED・FAILED・TIMED_OUT の理由
  bool limited = false;
  std::optional<std::uint8_t>
      internal_op;  // GET_DIAG など作品03 の内部の操作（ACK は cmd/ack にしない）
};

// 指令の状態の変化（履歴の commands・command_transitions。Phase 5）
struct CommandEvent {  // NOLINT(bugprone-exception-escape)：JSON の既定値の構築は投げない
  std::string cmd_id;
  std::string target;
  Json payload;
  std::string from;  // 作られたときは空
  std::string to;
  std::string detail;
  int retries = 0;
};

struct CommandOutputs {
  struct Publish {
    std::string target;
    Json payload;
  };
  std::vector<uart::Cmd> uart;
  std::vector<Publish> mqtt;            // 発行する cmd（宛先と payload）
  std::vector<CommandRecord> finished;  // 終端に達した指令（終わった順）
  std::vector<CommandRecord> replaced;  // 送信待ちで置き換えられた指令（#21）
  std::vector<CommandEvent> events;     // 状態の変化（履歴）
};

struct CommandStats {
  std::uint64_t submitted = 0;
  std::uint64_t uart_sent = 0;  // 再送を含む
  std::uint64_t uart_retries = 0;
  std::uint64_t mqtt_sent = 0;  // 再送を含む
  std::uint64_t mqtt_retries = 0;
  std::uint64_t done = 0;
  std::uint64_t rejected = 0;
  std::uint64_t failed = 0;
  std::uint64_t timed_out = 0;
  std::uint64_t offline = 0;
  std::uint64_t expired = 0;
  std::uint64_t replaced = 0;
  std::uint64_t renewals = 0;
  std::uint64_t renewals_skipped = 0;  // 更新の判定（RenewGuard）で送らなかった回
  std::uint64_t unknown_ack = 0;
  std::uint64_t duplicate_ack = 0;
  std::uint64_t uart_requeued = 0;
};

class CommandManager {
 public:
  using OnlineFn = std::function<bool(const std::string&)>;

  CommandManager(uart::UartBridge& bridge, IClock& clock, IIdGenerator& ids,
                 std::string uart_device_id, OnlineFn online);

  struct Submit {
    std::string target;
    Json payload;  // op・args・priority など。cmd_id はここで採番する
    std::string owner;
    bool check_online = true;  // false：GET_STATUS など、STATE 待ちの間にも送る内部の指令
  };
  // 指令を受け付けて cmd_id を返す。宛先が offline ならその場で FAILED（out.finished）
  std::string submit(Submit s, CommandOutputs& out);
  // UART の内部の操作（GET_DIAG）。汎用指令との対応がなく、ACK は cmd/ack にしない（v0.2.2 #37）
  std::string submit_internal(Submit s, std::uint8_t uart_op, CommandOutputs& out);
  // 内部の操作の ACK（UartBridge の internal_ack：cmd_ref・status。cmd/ack にはならない）
  void on_internal_ack(const Json& ack, CommandOutputs& out);
  // cmd/ack の payload（MQTT のデバイスの cmd/ack、UART の ACK を変換した cmd/ack）
  void on_ack(const Json& ack, CommandOutputs& out);
  // 再送・タイムアウト・UART の送信枠・リースの更新
  void on_timer(SteadyTime now, CommandOutputs& out);
  // uart-out に入らなかった CMD を、次の送信枠で先に送る（詳細設計書 第3章）
  void requeue_uart_frame(const uart::Cmd& frame);

  // 制御を続ける set_actuator：今送り、以後 lease_s の3分の1ごとに新しい cmd_id で送り直す
  std::string keep(const std::string& owner, const std::string& target, Json payload,
                   CommandOutputs& out);
  void stop_keeping(const std::string& owner, const std::string& target,
                    const std::string& actuator);
  // owner の更新をすべて止める
  void stop_keeping(const std::string& owner);
  [[nodiscard]] bool keeping(const std::string& owner) const;
  // 更新の各回に送ってよいかを決める（open-questions #33：Core が調停の所有者と EMERGENCY を見る）。
  // false の回は送らずに次の回を待つ
  using RenewGuard =
      std::function<bool(const std::string& owner, const std::string& target, const Json& payload)>;
  void set_renew_guard(RenewGuard guard) { renew_guard_ = std::move(guard); }

  [[nodiscard]] std::optional<SteadyTime> next_deadline() const;
  [[nodiscard]] const CommandRecord* find(const std::string& cmd_id) const;
  [[nodiscard]] std::size_t uart_in_flight() const { return uart_in_flight_.size(); }
  [[nodiscard]] std::size_t uart_queued() const { return uart_queue_.size(); }
  [[nodiscard]] const CommandStats& stats() const { return stats_; }

 private:
  struct Keep {
    std::string owner;
    std::string target;
    std::string actuator;
    Json payload;
    SteadyTime next_at;
  };

  std::string submit_impl(Submit s, std::optional<std::uint8_t> internal_op, CommandOutputs& out);
  void finish(const std::string& cmd_id, CmdState state, std::string reason, CommandOutputs& out);
  static void note(const CommandRecord& r, std::string from, std::string detail,
                   CommandOutputs& out);
  [[nodiscard]] bool expired(const CommandRecord& r) const;
  [[nodiscard]] bool slot_free(SteadyTime now) const;
  void pump_uart(SteadyTime now, CommandOutputs& out);
  bool transmit_uart(CommandRecord& r, SteadyTime now, CommandOutputs& out);
  void transmit_mqtt(CommandRecord& r, SteadyTime now, CommandOutputs& out);
  void replace_queued(const CommandRecord& incoming, CommandOutputs& out);
  void renew_due(SteadyTime now, CommandOutputs& out);
  [[nodiscard]] static std::string actuator_of(const Json& payload);
  [[nodiscard]] static SteadyTime::duration renew_interval(const Json& payload);

  uart::UartBridge& bridge_;
  IClock& clock_;
  IIdGenerator& ids_;
  std::string uart_device_id_;
  OnlineFn online_;
  CommandStats stats_;

  std::map<std::string, CommandRecord> active_;          // 終端に達していない指令
  std::deque<std::string> uart_queue_;                   // 送信待ち（cmd_id）
  std::map<std::uint16_t, std::string> uart_in_flight_;  // cmd_ref → cmd_id（ACK 待ち）
  std::deque<uart::Cmd> uart_requeued_;
  std::optional<SteadyTime> last_uart_sent_;
  std::uint16_t next_cmd_ref_ = 0;
  std::vector<Keep> keeps_;
  RenewGuard renew_guard_;
};

}  // namespace gw::core::command
