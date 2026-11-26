// UART フレーム ⇔ 共通エンベロープの変換（第7章）。
// 対応する Python：iotspec.uart.bridge.UartBridge
//
// - 受信（01 → 03）：TELEMETRY・EVENT・STATE・ACK を telemetry・event・state・cmd/ack に変換する。
//   ts は受信時刻（呼び出し側が渡す）、ts_source は gateway、uptime_ms はエンベロープに残す。
//   seq は UART の16ビット連番（方向・種別ごと、#25）を種別ごとの SeqExtender で32ビットに広げた値
// - 送信（03 → 01）：cmd の payload をプロファイルの commands で CMD に変換する
//   （cmd_ref は呼び出し側が渡す）
// - uptime_ms が減ったら再起動とみなし、新しい STATE を受けるまで変換せずに数える
// - DIAG は電文にせず diag に入れて not_converted で数える。
//   内部の操作（internal_frame で送った GET_DIAG など）の ACK も cmd/ack にしない（v0.2.2 #37）
// - BOOT の EVENT は from を reset_reason とし、detail を {"reset_reason": ...} だけにする（#38）
//
// Python との違い：
// - 時計を持たず、受信時刻を receive の引数で受ける
//   （詳細設計書 第5章：uart-io が受信時刻を付けて inbound に入れる）
// - JSON Schema の検証（_emit）は、UART の値から起こりうる違反だけを確かめる
//   （fan_pm が1000超、rejected・failed なのに reason が null。open-questions #9）
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/common/Clock.h"
#include "core/common/Envelope.h"
#include "core/common/Ids.h"
#include "core/uart/Frames.h"
#include "core/uart/Profile.h"
#include "core/uart/SeqExtender.h"

namespace gw::core::uart {

// 受信したフレーム1件の処理結果（bridge.BridgeResult）
struct BridgeResult {
  std::optional<Envelope> envelope;      // 発行する電文（0件か1件）
  std::optional<std::string> discarded;  // 破棄した理由
  std::string detail;
  bool request_state = false;        // GET_STATUS で STATE を求めるべき
  std::optional<Json> diag;          // DIAG の項目＋received_at・boot_id（#37）
  std::optional<Json> internal_ack;  // 内部の操作の ACK の内容
};

// cmd を CMD に変換できなかった（bridge.CommandRejected）。reason は cmd/ack の reason
// にそのまま使える
struct CommandRejected {
  std::string reason;
  std::string detail;
};

inline constexpr std::string_view kNotConverted = "not_converted";

class UartBridge {
 public:
  UartBridge(Profile profile, IIdGenerator& ids, std::optional<std::string> bridged_by);

  // ------------------------------------------------------------ 受信（01 → 03）
  // 復号の結果1件を処理する（receive_bytes と receive）。at は受信時刻
  [[nodiscard]] BridgeResult receive(const Decoded& decoded, SystemTime at);
  [[nodiscard]] BridgeResult receive(const Frame& frame, SystemTime at);

  // UART デバイスが途絶えたときの offline の state（#36）。seq は進めない。STATE 未受信なら nullopt
  [[nodiscard]] std::optional<Envelope> offline_state(SystemTime at);

  // ------------------------------------------------------------ 送信（03 → 01）
  // 03 → 01 の向きの、種別ごとの UART の連番（u16、一周したら0）
  [[nodiscard]] std::uint16_t next_tx_seq(std::uint8_t frame_type);
  // cmd の payload を CMD に変換し、cmd_ref ⇔ cmd_id の対応を記録する
  [[nodiscard]] std::variant<Cmd, CommandRejected> command_to_frame(const Json& cmd,
                                                                    std::uint16_t cmd_ref);
  // 内部の操作（GET_DIAG・SET_THRESHOLD）の CMD。ACK は cmd/ack にしない。ttl_s は0。
  // リースを持つ op・args の数違いは std::invalid_argument
  [[nodiscard]] Cmd internal_frame(std::uint8_t op, std::uint16_t cmd_ref,
                                   std::vector<std::int32_t> args = {});
  [[nodiscard]] Heartbeat heartbeat_frame(std::uint32_t uptime_ms);

  // ------------------------------------------------------------ 状態
  [[nodiscard]] const Profile& profile() const { return profile_; }
  [[nodiscard]] const std::optional<std::string>& boot_id() const { return boot_id_; }
  [[nodiscard]] const std::string& mode() const { return mode_; }
  [[nodiscard]] bool awaiting_state() const { return awaiting_state_; }
  [[nodiscard]] std::uint64_t dropped_telemetry() const { return dropped_telemetry_; }
  // 破棄の理由ごとの件数（第5章：破棄や置き換えの件数は状態として公開する）
  [[nodiscard]] const std::map<std::string, std::uint64_t, std::less<>>& discards() const {
    return discards_;
  }
  // 受信した UART フレームの種別ごとの欠番の数（HEARTBEAT を含む）
  [[nodiscard]] const std::map<std::string, std::uint64_t, std::less<>>& uart_gaps() const {
    return uart_gaps_;
  }

 private:
  BridgeResult on_state(const State& frame, SystemTime at);
  [[nodiscard]] Json state_payload(const std::string& status) const;
  Envelope telemetry(const Telemetry& frame, std::uint32_t seq, SystemTime at);
  Envelope event(const Event& frame, std::uint32_t seq, SystemTime at);
  [[nodiscard]] Json diag(const Diag& frame, SystemTime at) const;
  BridgeResult internal_ack(const Ack& frame, bool request);
  Envelope ack(const Ack& frame, std::uint32_t seq, const std::string& cmd_id, std::uint8_t uart_op,
               SystemTime at);
  Envelope envelope(std::string_view kind, std::uint32_t seq, Json payload,
                    std::optional<std::uint32_t> uptime_ms, SystemTime at);
  std::uint32_t extend(std::uint8_t frame_type, const std::string& name, std::uint16_t seq16);
  BridgeResult emit(Envelope env, bool request);
  BridgeResult discard(const std::string& reason, std::string detail, bool request = false);

  Profile profile_;
  IIdGenerator& ids_;
  std::optional<std::string> bridged_by_;

  std::optional<std::string> boot_id_;
  std::string mode_;
  bool awaiting_state_ = true;
  std::uint64_t dropped_telemetry_ = 0;
  std::map<std::string, std::uint64_t, std::less<>> discards_;
  std::map<std::uint8_t, SeqExtender> rx_;
  std::map<std::uint8_t, std::uint32_t> rx_last_;
  std::map<std::string, std::uint64_t, std::less<>> uart_gaps_;
  std::optional<std::uint32_t> last_uptime_;
  bool reboot_detected_ = false;
  bool state_requested_ = false;
  std::map<std::uint8_t, std::uint16_t> tx_seq_;
  std::optional<std::uint16_t> period_ms_;
  std::string fw_version_ = "0.0";
  std::map<std::uint16_t, std::pair<std::string, std::uint8_t>> cmd_table_;
  std::map<std::uint16_t, std::uint8_t> internal_refs_;
};

// fw_version:u16 → "{major}.{minor}"（#18）
[[nodiscard]] std::string format_fw_version(std::uint16_t fw_version);

}  // namespace gw::core::uart
