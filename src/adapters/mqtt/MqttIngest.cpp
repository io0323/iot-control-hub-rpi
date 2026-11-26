#include "adapters/mqtt/MqttIngest.h"

#include <spdlog/spdlog.h>

#include "core/common/Constants.h"
#include "core/common/Topics.h"

namespace gw::adapters {

namespace {

// iot/v1/{site}/{device_id}/{kind} の device_id（トピックの形でなければ空）
std::string topic_device(const std::string& topic) {
  std::size_t pos = 0;
  for (int i = 0; i < 3; ++i) {
    pos = topic.find('/', pos);
    if (pos == std::string::npos) {
      return {};
    }
    ++pos;
  }
  const auto end = topic.find('/', pos);
  return end == std::string::npos ? std::string{} : topic.substr(pos, end - pos);
}

bool ends_with(const std::string& text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

MqttIngest::MqttIngest(const SchemaValidator& validator, std::set<std::string> own_devices,
                       const core::IClock& clock, std::function<bool(core::Inbound)> push)
    : validator_(validator),
      own_devices_(std::move(own_devices)),
      clock_(clock),
      push_(std::move(push)) {
}

std::vector<std::string> MqttIngest::subscriptions(const std::string& site) {
  std::vector<std::string> topics;
  for (const auto kind :
       {core::kKindTelemetry, core::kKindEvent, core::kKindState, core::kKindCmdAck}) {
    topics.push_back(core::topic_for(site, "+", kind));
  }
  topics.push_back(core::topic_for(site, core::constants::kDeviceApp01, core::kKindRequest));
  return topics;
}

IngestStats MqttIngest::stats() const {
  const std::lock_guard lock(mutex_);
  return stats_;
}

void MqttIngest::log_rejected(const std::string& topic, const std::string& reason,
                              const std::string& detail) {
  const auto sender = topic_device(topic);
  const auto now = clock_.steady_now();
  {
    const std::lock_guard lock(mutex_);
    const auto it = last_log_.find(sender);
    if (it != last_log_.end() && now - it->second < kLogInterval) {
      return;  // 同じ発信元のログは10秒に1回
    }
    last_log_[sender] = now;
  }
  spdlog::warn("event=mqtt_rejected sender={} topic={} reason={} detail=\"{}\"", sender, topic,
               reason, detail);
}

void MqttIngest::handle(const MqttMessage& message) {
  {
    const std::lock_guard lock(mutex_);
    ++stats_.received;
  }
  // 自分の発行が戻ってきたもの（トピックの device_id で先に見分ける：パースを省ける）
  const auto device = topic_device(message.topic);
  if (own_devices_.contains(device)) {
    const std::lock_guard lock(mutex_);
    ++stats_.own_echo;
    return;
  }
  if (ends_with(message.topic, "/request")) {
    const std::lock_guard lock(mutex_);
    ++stats_.requests;  // 検証して core（RequestHandler）に渡す
  }
  const auto received_at = clock_.system_now();
  const auto started = std::chrono::steady_clock::now();
  auto result = validator_.validate(message.payload);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  {
    const std::lock_guard lock(mutex_);
    stats_.validate_time += elapsed;
    ++stats_.validated;
    if (!result.ok) {
      ++stats_.rejected[result.reason];
    }
  }
  if (!result.ok || !result.message) {
    log_rejected(message.topic, result.reason, result.detail);
    return;
  }
  auto& parsed = *result.message;
  if (own_devices_.contains(parsed.value("device_id", ""))) {
    const std::lock_guard lock(mutex_);
    ++stats_.own_echo;
    return;
  }
  core::Inbound inbound{core::MqttReceived{message.topic, std::move(parsed), result.kind,
                                           received_at, message.retained}};
  const bool pushed = push_(std::move(inbound));
  const std::lock_guard lock(mutex_);
  ++(pushed ? stats_.accepted : stats_.inbound_dropped);
}

}  // namespace gw::adapters
