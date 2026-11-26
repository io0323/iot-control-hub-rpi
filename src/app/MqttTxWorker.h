// mqtt-tx スレッド（詳細設計書 第3章・第6章「送信制限」）。
// - 発信元（device_id）× kind ごとのトークンバケット：平均毎秒10件、容量50（open-questions #5）
//   空のとき telemetry は捨て（数える）、それ以外はトークンが貯まるまで待つ
// - QoS 1 の確認待ちは32件まで。超えたら PUBACK を待つ
// - 切断中は state をトピックごとの最新1件、event・cmd・ack を最大 hold 件
//   （古いものから捨てる）保持し、telemetry は捨てる（数える）。
//   接続が戻ったら保持していたものを先に送る
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "adapters/mqtt/IMqttClient.h"
#include "core/Core.h"
#include "core/common/BoundedQueue.h"
#include "core/common/Clock.h"
#include "core/common/TokenBucket.h"

namespace gw::app {

struct MqttTxStats {
  std::uint64_t published = 0;
  std::uint64_t dropped_telemetry_rate = 0;          // バケットが空で捨てた telemetry
  std::uint64_t dropped_telemetry_disconnected = 0;  // 切断中に捨てた telemetry
  std::uint64_t dropped_telemetry_before_connect = 0;  // 最初の接続まで保持できず捨てた telemetry
  std::uint64_t dropped_held = 0;  // 保持の上限を超えて捨てた event・cmd・ack
  std::uint64_t publish_errors = 0;
};

class MqttTxWorker {
 public:
  MqttTxWorker(adapters::IMqttClient& client, core::BoundedQueue<core::MqttTx>& queue,
               const core::IClock& clock, std::size_t hold_capacity);

  // mqtt-out が閉じられて空になるまで送る（スレッドの本体）
  void run();
  // IMqttClient のコールバックから呼ぶ（mqtt-loop のスレッド）
  void on_connection(adapters::ConnectionEvent event);
  void on_published(int mid);

  [[nodiscard]] MqttTxStats stats() const;
  [[nodiscard]] bool connected() const { return connected_; }

 private:
  void send(core::MqttTx item);
  void flush_held();
  void hold(core::MqttTx item);
  // トークンを使う。telemetry で空なら false。待っている間に切断したら false（hold 済み）
  bool take_token(const core::MqttTx& item);
  bool wait_inflight();

  adapters::IMqttClient& client_;
  core::BoundedQueue<core::MqttTx>& queue_;
  const core::IClock& clock_;
  const std::size_t hold_capacity_;

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::atomic<bool> connected_{false};
  bool ever_connected_ = false;  // 最初の接続が成立した（open-questions #12）
  std::set<int> inflight_;
  std::set<int> early_acks_;  // publish の戻りより先に届いた PUBACK
  std::map<std::string, core::MqttTx> held_states_;
  std::deque<core::MqttTx> held_others_;
  std::map<std::pair<std::string, std::string>, core::TokenBucket> buckets_;
  MqttTxStats stats_;
};

}  // namespace gw::app
