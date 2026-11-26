#include "app/MqttTxWorker.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "core/common/Constants.h"
#include "core/common/Topics.h"

namespace gw::app {

namespace {
bool is_telemetry(const core::MqttTx& item) {
  return item.kind == core::kKindTelemetry;
}
bool is_state(const core::MqttTx& item) {
  return item.kind == core::kKindState;
}
}  // namespace

MqttTxWorker::MqttTxWorker(adapters::IMqttClient& client, core::BoundedQueue<core::MqttTx>& queue,
                           const core::IClock& clock, std::size_t hold_capacity)
    : client_(client), queue_(queue), clock_(clock), hold_capacity_(hold_capacity) {
}

void MqttTxWorker::run() {
  while (auto item = queue_.pop()) {
    if (connected_) {
      flush_held();
    }
    send(std::move(*item));
  }
}

void MqttTxWorker::on_connection(adapters::ConnectionEvent event) {
  {
    const std::lock_guard lock(mutex_);
    connected_ = event == adapters::ConnectionEvent::kConnected;
    if (connected_) {
      ever_connected_ = true;
    } else {
      // 確認待ちは数えない（再接続後は libmosquitto が再送する）
      inflight_.clear();
      early_acks_.clear();
    }
  }
  changed_.notify_all();
}

void MqttTxWorker::on_published(int mid) {
  {
    const std::lock_guard lock(mutex_);
    if (inflight_.erase(mid) == 0) {
      early_acks_.insert(mid);
    }
  }
  changed_.notify_all();
}

MqttTxStats MqttTxWorker::stats() const {
  const std::lock_guard lock(mutex_);
  return stats_;
}

void MqttTxWorker::hold(core::MqttTx item) {
  const std::lock_guard lock(mutex_);
  if (is_telemetry(item) && ever_connected_) {
    ++stats_.dropped_telemetry_disconnected;  // 2回目以降の切断は telemetry を捨てる（第3章）
    return;
  }
  if (is_state(item)) {
    held_states_.insert_or_assign(item.topic, std::move(item));
    return;
  }
  // 最初の接続まで（open-questions #12）は telemetry も保持する。上限を超えたら古い telemetry から捨てる
  if (held_others_.size() >= hold_capacity_) {
    const auto oldest_telemetry =
        std::find_if(held_others_.begin(), held_others_.end(), is_telemetry);
    if (oldest_telemetry != held_others_.end()) {
      held_others_.erase(oldest_telemetry);
      ++stats_.dropped_telemetry_before_connect;
    } else if (is_telemetry(item)) {
      ++stats_.dropped_telemetry_before_connect;
      return;
    } else {
      held_others_.pop_front();
      ++stats_.dropped_held;
    }
  }
  held_others_.push_back(std::move(item));
}

void MqttTxWorker::flush_held() {
  std::map<std::string, core::MqttTx> states;
  std::deque<core::MqttTx> others;
  {
    const std::lock_guard lock(mutex_);
    states.swap(held_states_);
    others.swap(held_others_);
  }
  for (auto& [topic, item] : states) {
    send(std::move(item));
  }
  for (auto& item : others) {
    send(std::move(item));
  }
}

bool MqttTxWorker::take_token(const core::MqttTx& item) {
  std::unique_lock lock(mutex_);
  const auto key = std::make_pair(item.device_id, item.kind);
  auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    it = buckets_
             .emplace(key,
                      core::TokenBucket(core::constants::kPublishRateBurstPerS,
                                        core::constants::kPublishRateAvgPerS, clock_.steady_now()))
             .first;
  }
  for (;;) {
    if (it->second.try_take(clock_.steady_now())) {
      return true;
    }
    if (is_telemetry(item)) {
      ++stats_.dropped_telemetry_rate;
      return false;
    }
    const auto wait = it->second.wait_time(clock_.steady_now());
    changed_.wait_for(lock, wait, [&] { return !connected_; });
    if (!connected_) {
      return false;
    }
  }
}

bool MqttTxWorker::wait_inflight() {
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] {
    return !connected_ ||
           inflight_.size() < static_cast<std::size_t>(core::constants::kMqttMaxInFlight);
  });
  return connected_;
}

void MqttTxWorker::send(core::MqttTx item) {
  if (!connected_) {
    hold(std::move(item));
    return;
  }
  if (!take_token(item)) {
    if (!is_telemetry(item)) {
      hold(std::move(item));
    }
    return;
  }
  if (item.qos > 0 && !wait_inflight()) {
    hold(std::move(item));
    return;
  }
  const auto result = client_.publish(item.topic, item.payload, item.qos, item.retain);
  if (!result.ok) {
    if (result.not_connected) {
      hold(std::move(item));
    } else {
      const std::lock_guard lock(mutex_);
      ++stats_.publish_errors;
      spdlog::warn("event=mqtt_publish_failed topic={} error={}", item.topic, result.error);
    }
    return;
  }
  const std::lock_guard lock(mutex_);
  ++stats_.published;
  if (item.qos > 0 && early_acks_.erase(result.mid) == 0) {
    inflight_.insert(result.mid);
  }
}

}  // namespace gw::app
