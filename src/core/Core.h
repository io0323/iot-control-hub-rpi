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
#pragma once

#include <cstdint>
#include <deque>
#include <map>
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
#include "core/quality/Quality.h"
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
};

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
using Inbound = std::variant<UartReceived, UartLinkUp, UartLinkDown, MqttConnected,
                             MqttDisconnected, MqttReceived>;

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
struct Outputs {
  std::vector<UartTx> uart;
  std::vector<MqttTx> mqtt;
  std::vector<std::string> logs;  // CoreRunner が info で出すログ（key=value）
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
};

class Core {
 public:
  Core(CoreConfig config, IClock& clock, IIdGenerator& ids);

  // 起動：HEARTBEAT と stale 判定のタイマーを始める（最初の HEARTBEAT をすぐ送る）
  [[nodiscard]] Outputs start();
  [[nodiscard]] Outputs step(const Inbound& msg);
  // 期限の来たタイマーをすべて処理する
  [[nodiscard]] Outputs on_timer(SteadyTime now);
  [[nodiscard]] std::optional<SteadyTime> next_deadline();
  // 終了：gw-01 の offline の state（seq を進めない）
  [[nodiscard]] Outputs shutdown();

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
  command::Arbiter arbiter_;
  command::CommandManager commands_;
  command::RequestHandler requests_;
};

}  // namespace gw::core
