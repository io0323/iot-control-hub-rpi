#include "adapters/mqtt/FakeMqttClient.h"

#include <algorithm>

namespace gw::adapters {

void FakeMqttClient::connect(const MqttConnectOptions& /*options*/, const MqttWill& will) {
  const std::lock_guard lock(mutex_);
  will_ = will;
}

PublishResult FakeMqttClient::publish(std::string_view topic, std::string_view payload, int qos,
                                      bool retain) {
  PublishResult result;
  std::function<void(int)> handler;
  {
    const std::lock_guard lock(mutex_);
    if (!connected_) {
      result.not_connected = true;
      result.error = "not connected";
      return result;
    }
    result.ok = true;
    result.mid = next_mid_++;
    published_.push_back(
        FakePublished{std::string{topic}, std::string{payload}, qos, retain, result.mid});
    if (qos > 0) {
      if (auto_ack_) {
        handler = published_handler_;
      } else {
        unacked_.push_back(result.mid);
      }
    }
  }
  if (handler) {
    handler(result.mid);
  }
  return result;
}

void FakeMqttClient::subscribe(std::string_view filter, int /*qos*/) {
  const std::lock_guard lock(mutex_);
  subscriptions_.emplace_back(filter);
}

void FakeMqttClient::disconnect() {
  const std::lock_guard lock(mutex_);
  disconnected_ = true;
  connected_ = false;
}

void FakeMqttClient::reconnect() {
  const std::lock_guard lock(mutex_);
  ++reconnects_;
}

int FakeMqttClient::reconnects() const {
  const std::lock_guard lock(mutex_);
  return reconnects_;
}

void FakeMqttClient::on_message(std::function<void(MqttMessage)> handler) {
  const std::lock_guard lock(mutex_);
  message_handler_ = std::move(handler);
}

void FakeMqttClient::on_connection(std::function<void(ConnectionEvent)> handler) {
  const std::lock_guard lock(mutex_);
  connection_handler_ = std::move(handler);
}

void FakeMqttClient::on_published(std::function<void(int)> handler) {
  const std::lock_guard lock(mutex_);
  published_handler_ = std::move(handler);
}

void FakeMqttClient::set_connected(bool connected) {
  std::function<void(ConnectionEvent)> handler;
  {
    const std::lock_guard lock(mutex_);
    connected_ = connected;
    if (!connected) {
      unacked_.clear();
    }
    handler = connection_handler_;
  }
  if (handler) {
    handler(connected ? ConnectionEvent::kConnected : ConnectionEvent::kDisconnected);
  }
}

void FakeMqttClient::ack(int mid) {
  std::function<void(int)> handler;
  {
    const std::lock_guard lock(mutex_);
    const auto it = std::find(unacked_.begin(), unacked_.end(), mid);
    if (it == unacked_.end()) {
      return;
    }
    unacked_.erase(it);
    handler = published_handler_;
  }
  if (handler) {
    handler(mid);
  }
}

void FakeMqttClient::ack_all() {
  for (const int mid : unacked()) {
    ack(mid);
  }
}

std::vector<FakePublished> FakeMqttClient::published() const {
  const std::lock_guard lock(mutex_);
  return published_;
}

std::vector<int> FakeMqttClient::unacked() const {
  const std::lock_guard lock(mutex_);
  return unacked_;
}

std::optional<MqttWill> FakeMqttClient::will() const {
  const std::lock_guard lock(mutex_);
  return will_;
}

bool FakeMqttClient::disconnected() const {
  const std::lock_guard lock(mutex_);
  return disconnected_;
}

void FakeMqttClient::set_auto_ack(bool value) {
  const std::lock_guard lock(mutex_);
  auto_ack_ = value;
}

}  // namespace gw::adapters
