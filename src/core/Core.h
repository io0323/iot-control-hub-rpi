// core の処理（詳細設計書 第4章）。スレッドを持たない。
// 「入力1件または時刻を受け、状態を更新し、出力を返す」だけを行う。
// スレッドの組み立ては app の CoreRunner。
//
// Phase 1：
// - UART の受信フレーム → UartBridge → mqtt-out。DIAG は not_converted で数えるだけ（履歴は Phase 5）
// - request_state → GET_STATUS（cmd_id を採番する普通の指令、priority auto）。
//   毎秒1件の上限だけを守る最小の送信器で送る（再送と未完了4件は Phase 3 の CommandManager）
// - HEARTBEAT を1秒ごとに送る。受信前に GET_STATUS は送らない
// - gw-01 自身の state（online、retain）。MQTT につながるたびに発行する。LWT と終了時は offline
// Phase 2：
// - MQTT の受信（mqtt-loop で検証済み）：重複・欠番・時刻ずれ・遅延到着（第5章）、DeviceRegistry の更新
// - LivenessMonitor：UART は2秒で suspect・3秒で offline（offline の state を bridged_by 付きで発行）。
//   MQTT は state と、チャネルの stale（1秒周期）。online ⇔ offline で DEVICE_ONLINE／DEVICE_OFFLINE
// Phase 3：
// - Arbiter（調停）、CommandManager（指令のライフサイクル・再送・UART の毎秒1件と未完了4件）、
//   RequestHandler（request → 指令、request/ack）。Phase 1 の GET_STATUS の送信器は CommandManager に
//   置き換えた。COMMAND_FAILED は FAILED・TIMED_OUT のとき1件
// Phase 4：
// - RuleEngine（core/rules）：値の更新・生存状態・モード・1秒の時刻帯と stale・for と cooldown で評価し、
//   効果（発火・解除・不明・再開）を Core がアクションにする（CoreRules.cpp）
// - SystemModeController：NORMAL・ALERT・EMERGENCY。SYSTEM_MODE_CHANGED と gw-01 の state の mode
// - 承認の request（v0.2.3 #46）、EMERGENCY の schedule の拒否、SIGHUP の再読込（Reload）
// - gw-01 の telemetry：品質の件数を60秒ごと（v0.2.3 #45）
// Phase 5：
// - 履歴の行（Outputs::history）：イベント、デバイスの状態、ルールの発火と解除、指令と状態遷移、要求と
//   結果、モードの変化、DIAG。telemetry は記録しない。書くのは history スレッド（core は DB を読まない：D5）
// - 起動時の復元（restore）、gwctl の管理要求（AdminRequest、CoreHistory.cpp）、DATA_DROPPED
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/command/Arbiter.h"
#include "core/command/CommandManager.h"
#include "core/command/RequestHandler.h"
#include "core/common/Clock.h"
#include "core/common/Envelope.h"
#include "core/common/Ids.h"
#include "core/common/TimerService.h"
#include "core/device/DeviceRegistry.h"
#include "core/device/LivenessMonitor.h"
#include "core/history/HistoryRecord.h"
#include "core/mode/SystemModeController.h"
#include "core/quality/Quality.h"
#include "core/rules/RuleEngine.h"
#include "core/uart/Frames.h"
#include "core/uart/Profile.h"
#include "core/uart/UartBridge.h"

namespace gw::core {

struct CoreConfig {
  std::string site;
  std::string device_id;       // gw-01
  uart::Profile self_profile;  // profiles/gw-01.yaml
  uart::Profile uart_profile;  // devices.yaml の UART のデバイス（profiles/mcu-c-01.yaml）
  std::string fw_version;      // gw-01 の state の fw_version
  std::string boot_id;         // gw-01 の boot_id（16進小文字8桁）
  std::vector<uart::Profile> static_devices;  // devices.yaml（uart_profile を含んでよい）
  command::RequestRules requests;             // requests.yaml（open-questions #20）
  rules::RuleSet rules;                       // rules.yaml（Phase 4）
};

// core の外（adapters・app）で数える品質の件数（v0.2.3 #45 の mqtt_rejected・queue_dropped）
struct ExternalCounts {
  std::uint64_t mqtt_rejected = 0;  // スキーマ違反・JSON 不正・未知の schema で捨てた電文
  std::uint64_t queue_dropped = 0;  // キュー・切断中の保持で捨てた電文
};
using ExternalCountsFn = std::function<ExternalCounts()>;

inline constexpr std::string_view kGwctlRequester = "gwctl";  // gwctl cmd の要求元

// ---- 入力
struct UartReceived {
  uart::Decoded decoded;
  SystemTime received_at;  // uart-io が付けた受信時刻
};
struct UartLinkUp {};
struct UartLinkDown {};
struct MqttConnected {};
struct MqttDisconnected {};
// mqtt-loop でパースとスキーマ検証を済ませた電文（詳細設計書 第6章「受信」）
struct MqttReceived {
  std::string topic;
  Json message;
  std::string kind;  // schema から求めたトピックの kind
  SystemTime received_at;
  bool retained = false;
};
// gwctl の管理要求（admin スレッドが受けて入れる。応答は Outputs::admin）
struct AdminRequest {  // NOLINT(clang-analyzer-core.uninitialized.Assign)：既定値は全部ある
  std::uint64_t id = 0;  // admin スレッドが付ける（応答の宛先）
  std::string cmd;       // devices・rules・stats・approve・cmd・diag
  Json args = Json::object();
};
// history スレッドが上限で捨てた件数（DATA_DROPPED で知らせる）
struct HistoryDropped {
  std::uint64_t count = 0;
  std::string source = "history";
};
// SIGHUP の再読込（app が3ファイルを読み、すべて検証に通ったときだけ入れる）
struct Reload {
  rules::RuleSet rules;
  std::vector<uart::Profile> devices;
  command::RequestRules requests;
};
using Inbound = std::variant<UartReceived, UartLinkUp, UartLinkDown, MqttConnected,
                             MqttDisconnected, MqttReceived, Reload, AdminRequest, HistoryDropped>;

// ---- 出力
struct UartTx {
  uart::Frame frame;  // Heartbeat か Cmd
};
struct MqttTx {
  std::string topic;
  std::string payload;  // JSON
  int qos = 0;
  bool retain = false;
  std::string device_id;  // エンベロープの device_id（送信制限の単位）
  std::string kind;
};
struct AdminReply {
  std::uint64_t id = 0;
  Json body;  // {ok, result} または {ok: false, error}
};
struct Outputs {
  std::vector<UartTx> uart;
  std::vector<MqttTx> mqtt;
  std::vector<history::HistoryRecord> history;  // history キューへ（Phase 5）
  std::vector<AdminReply> admin;                // gwctl への応答
  std::vector<std::string> logs;  // CoreRunner が info で出すログ（key=value）
  // ルールの reconnect（open-questions #32）：バックオフを待たずに開き直す
  bool reconnect_uart = false;
  bool reconnect_mqtt = false;
};

// 数えるもの（gw-01 の telemetry での公開は共通仕様の変更案待ち：open-questions #15）
struct CoreStats {
  std::uint64_t uart_frames = 0;
  std::uint64_t get_status_requested = 0;
  std::uint64_t uart_offline_published = 0;
  std::uint64_t mqtt_received = 0;
  std::uint64_t mqtt_duplicates = 0;
  std::uint64_t mqtt_gaps = 0;
  std::uint64_t mqtt_clock_skew = 0;
  std::uint64_t mqtt_late = 0;
  std::uint64_t mqtt_unregistered = 0;  // 登録されていないデバイス・チャネルの値
  std::uint64_t stale_transitions = 0;  // チャネルが stale になった回数
  std::uint64_t devices_registered = 0;
  std::uint64_t device_events = 0;
  std::uint64_t command_failed_events = 0;
  std::uint64_t rules_fired = 0;
  std::uint64_t rules_cleared = 0;
  std::uint64_t rule_commands_rejected = 0;  // 調停で負けて送らなかったルールの指令
  std::uint64_t mode_changes = 0;
  std::uint64_t reloads = 0;
};

// 起動時に履歴から戻すもの（詳細設計書 第4章「起動の手順」、D5）。app が HistoryStore から作る
struct CoreRestore {
  std::vector<command::RestoredRequest> requests;  // 直近1時間
  std::optional<std::string> last_mode;            // system_mode_log の最後の to_mode
  std::string last_mode_reason;
  std::vector<history::DeviceRow> devices;
};

// 現地時刻の UTC からの差（分）。app が gateway.yaml の timezone で答える（open-questions #37）
using UtcOffsetFn = std::function<int(SystemTime)>;

class Core {
 public:
  Core(CoreConfig config, IClock& clock, IIdGenerator& ids);
  ~Core();
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  Core(Core&&) = delete;
  Core& operator=(Core&&) = delete;

  // 起動：HEARTBEAT と stale 判定のタイマーを始める（最初の HEARTBEAT をすぐ送る）
  [[nodiscard]] Outputs start();
  [[nodiscard]] Outputs step(const Inbound& msg);
  // 期限の来たタイマーをすべて処理する
  [[nodiscard]] Outputs on_timer(SteadyTime now);
  [[nodiscard]] std::optional<SteadyTime> next_deadline();
  // 終了：gw-01 の offline の state（seq を進めない）
  [[nodiscard]] Outputs shutdown();

  // 起動時の復元（start の前に呼ぶ）：要求の記憶、台帳、EMERGENCY の引き継ぎ（open-questions #42〜#44）
  void restore(const CoreRestore& r);

  // 品質の件数のうち core の外で数えるもの（app が設定する。core のスレッドから呼ぶ）
  void set_external_counts(ExternalCountsFn fn) { external_counts_ = std::move(fn); }
  // gw-01 の telemetry：品質の件数（unit count、起動からの累積。v0.2.3 #45）
  [[nodiscard]] Json quality_counts() const;

  // LWT に使う gw-01 の offline の state（seq を進めない）
  [[nodiscard]] MqttTx gw_offline_state();
  // uart-out に入らなかった CMD を送信待ちの先頭に戻す（第3章「入らない CMD は送信待ちに残す」）
  void requeue_uart_cmd(const uart::Cmd& cmd);

  [[nodiscard]] const uart::UartBridge& bridge() const { return bridge_; }
  [[nodiscard]] const CoreStats& stats() const { return stats_; }
  [[nodiscard]] const device::DeviceRegistry& registry() const { return registry_; }
  [[nodiscard]] const command::Arbiter& arbiter() const { return arbiter_; }
  [[nodiscard]] const command::CommandManager& commands() const { return commands_; }
  [[nodiscard]] const command::RequestHandler& requests() const { return requests_; }
  [[nodiscard]] const rules::RuleEngine& rules() const { return rules_; }
  [[nodiscard]] const mode::SystemModeController& system_mode() const { return mode_; }
  // 時刻帯の判定に使う UTC からの差（既定は Asia/Tokyo の +9:00）
  void set_utc_offset(UtcOffsetFn fn) { utc_offset_ = std::move(fn); }
  // 宛先が online か（UART は online・suspect、MQTT のデバイスも online・suspect）
  [[nodiscard]] bool device_online(const std::string& device_id) const;
  [[nodiscard]] bool uart_online() const;

 private:
  void on_uart(const UartReceived& msg, Outputs& out);
  void on_mqtt(const MqttReceived& msg, Outputs& out);
  void apply_message(const Json& message, const std::string& kind, SystemTime received_at,
                     bool check_late, Outputs& out);
  void apply_liveness(const std::optional<device::LivenessChange>& change, Outputs& out);
  void schedule_uart_liveness();
  void publish(const Envelope& env, Outputs& out) const;
  void publish_gw_state(const std::string& status, Outputs& out);
  void publish_quality_counts(Outputs& out);
  // gw-01 のプロファイルの unit count のチャネルの周期（無ければ発行しない）
  [[nodiscard]] std::optional<Millis> quality_counts_period() const;
  void publish_gw_event(const std::string& code, const std::string& severity, Json detail,
                        Outputs& out);
  void publish_uart_offline(Outputs& out);
  void queue_get_status(Outputs& out);
  // 指令の出力（UART・MQTT の cmd・終端）と request/ack を Outputs に移す
  void flush(command::CommandOutputs& cmd_out, std::vector<command::RequestAck>& acks,
             Outputs& out);
  // 所有者の変化：新しい所有者がいれば残りのリースで引き継ぎの指令を送る（open-questions #24）
  std::optional<std::string> apply_owner_change(const command::OwnerChange& change,
                                                command::CommandOutputs& cmd_out);
  void publish_gw(std::string_view kind, Json payload, const std::string& topic_device,
                  Outputs& out);
  [[nodiscard]] Envelope gw_envelope(std::string_view kind, std::uint32_t seq, Json payload);
  [[nodiscard]] Json gw_state_payload(const std::string& status) const;
  [[nodiscard]] std::uint32_t uptime_ms() const;

  // ---- ルールとモード（CoreRules.cpp）
  class RuleView;  // RuleContext の実装
  void setup_rules();
  void apply_effects(std::vector<rules::RuleEffect>& effects, Outputs& out);
  void on_rule_fired(std::size_t i, command::CommandOutputs& cmd_out, Outputs& out);
  void on_rule_cleared(std::size_t i, command::CommandOutputs& cmd_out, Outputs& out);
  // ルールの command を送る（hold_only：hold の指令だけ。不明からの再開・EMERGENCY の解除）
  void send_rule_commands(std::size_t i, bool hold_only, command::CommandOutputs& cmd_out,
                          Outputs& out);
  void stop_rule_commands(std::size_t i);
  void apply_mode_change(const mode::ModeChange& change, command::CommandOutputs& cmd_out,
                         Outputs& out);
  [[nodiscard]] bool renew_allowed(const std::string& owner, const std::string& target,
                                   const Json& payload);
  [[nodiscard]] std::optional<std::string> approve_emergency_release(std::string_view reason);
  // ---- 履歴と管理要求（CoreHistory.cpp）
  [[nodiscard]] std::int64_t now_ms() const;
  void record(history::Row row, Outputs& out) const;
  void record_event(const Json& message, Outputs& out) const;
  void record_commands(const command::CommandOutputs& cmd_out, Outputs& out) const;
  void record_request(const std::string& request_id, Outputs& out) const;
  [[nodiscard]] Json rule_trigger(std::size_t i, std::string_view what) const;
  void on_admin(const AdminRequest& msg, Outputs& out);
  [[nodiscard]] Json admin_devices() const;
  [[nodiscard]] Json admin_rules() const;
  [[nodiscard]] Json admin_stats() const;
  void on_reload(const Reload& msg, Outputs& out);
  [[nodiscard]] bool rule_active(const std::string& rule_id) const;
  [[nodiscard]] bool rule_idle(const std::string& rule_id) const;
  [[nodiscard]] int local_minutes() const;

  CoreConfig config_;
  IClock& clock_;
  IIdGenerator& ids_;
  uart::UartBridge bridge_;
  TimerService timers_;
  SeqCounter gw_seq_;
  SteadyTime started_;
  CoreStats stats_;
  device::DeviceRegistry registry_;
  device::LivenessMonitor liveness_;
  quality::DedupWindow dedup_;
  quality::GapCounter gaps_;
  quality::ClockSkewLimiter skew_limiter_;
  ExternalCountsFn external_counts_;
  command::Arbiter arbiter_;
  command::CommandManager commands_;
  command::RequestHandler requests_;
  rules::RuleEngine rules_;
  mode::SystemModeController mode_;
  std::unique_ptr<rules::RuleContext> rule_view_;  // RuleView（CoreRules.cpp）
  UtcOffsetFn utc_offset_;
  bool mqtt_connected_ = false;
  std::vector<mode::ModeChange> pending_mode_changes_;  // 承認の hook が積み、flush が発行する
  std::map<std::string, std::uint64_t> admin_requests_;  // gwctl cmd の request_id → 管理要求の id
};

}  // namespace gw::core
