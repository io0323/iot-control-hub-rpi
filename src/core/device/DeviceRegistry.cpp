#include "core/device/DeviceRegistry.h"

namespace gw::core::device {

std::string_view to_string(DeviceStatus status) {
  switch (status) {
    case DeviceStatus::kOnline:
      return "online";
    case DeviceStatus::kSuspect:
      return "suspect";
    case DeviceStatus::kOffline:
      return "offline";
    case DeviceStatus::kUnknown:
      return "unknown";
  }
  return "unknown";
}

DeviceInfo& DeviceRegistry::entry(const std::string& device_id) {
  auto& info = devices_[device_id];
  info.device_id = device_id;
  return info;
}

void DeviceRegistry::register_static(const uart::Profile& profile) {
  auto& info = entry(profile.device_id);
  info.transport = profile.transport;
  info.is_static = true;
  for (const auto& c : profile.channels) {
    auto& ch = info.channels[c.channel];
    ch.channel = c.channel;
    ch.unit = c.unit;
    ch.period_ms = c.period_ms;
  }
  info.actuators = Json::array();
  for (const auto& a : profile.actuators) {
    info.actuators.push_back(
        Json{{"id", a.id}, {"min", a.min}, {"max", a.max}, {"max_on_s", a.max_on_s}});
  }
}

bool DeviceRegistry::apply_state(const std::string& device_id, const std::string& boot_id,
                                 const Json& payload) {
  const bool created = !devices_.contains(device_id);
  auto& info = entry(device_id);
  if (info.transport.empty()) {
    info.transport = "mqtt";
  }
  info.boot_id = boot_id;
  info.bridged_by = payload.contains("bridged_by") && payload["bridged_by"].is_string()
                        ? std::optional<std::string>(payload["bridged_by"].get<std::string>())
                        : std::nullopt;
  if (payload.contains("fw_version") && payload["fw_version"].is_string()) {
    info.fw_version = payload["fw_version"].get<std::string>();
  }
  if (payload.contains("mode") && payload["mode"].is_string()) {
    info.mode = payload["mode"].get<std::string>();
  }
  if (payload.contains("status") && payload["status"] == "offline") {
    return created;  // offline の state では channels を作り直さない
  }
  if (payload.contains("channels") && payload["channels"].is_array()) {
    std::map<std::string, ChannelState> channels;
    for (const auto& c : payload["channels"]) {
      const auto name = c.value("channel", "");
      auto ch = info.channels.contains(name) ? info.channels.at(name) : ChannelState{};
      ch.channel = name;
      ch.unit = c.value("unit", "");
      ch.period_ms = c.value("period_ms", std::int64_t{0});
      channels[name] = std::move(ch);
    }
    info.channels = std::move(channels);
  }
  if (payload.contains("actuators") && payload["actuators"].is_array()) {
    info.actuators = payload["actuators"];
  }
  return created;
}

bool DeviceRegistry::update_reading(const std::string& device_id, const std::string& channel,
                                    double value, const std::string& quality,
                                    SteadyTime received_at) {
  auto* info = find(device_id);
  if (info == nullptr) {
    return false;
  }
  const auto it = info->channels.find(channel);
  if (it == info->channels.end()) {
    return false;
  }
  it->second.value = value;
  it->second.quality = quality;
  it->second.last_update = received_at;
  it->second.stale = false;
  return true;
}

DeviceInfo* DeviceRegistry::find(const std::string& device_id) {
  const auto it = devices_.find(device_id);
  return it == devices_.end() ? nullptr : &it->second;
}

const DeviceInfo* DeviceRegistry::find(const std::string& device_id) const {
  const auto it = devices_.find(device_id);
  return it == devices_.end() ? nullptr : &it->second;
}

std::optional<DeviceStatus> DeviceRegistry::effective_status(const std::string& device_id) const {
  const auto* info = find(device_id);
  if (info == nullptr) {
    return std::nullopt;
  }
  if (info->bridged_by && *info->bridged_by != device_id) {
    const auto* parent = find(*info->bridged_by);
    if (parent != nullptr && parent->status == DeviceStatus::kOffline) {
      return DeviceStatus::kUnknown;
    }
  }
  return info->status;
}

}  // namespace gw::core::device
