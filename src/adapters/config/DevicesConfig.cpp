#include "adapters/config/DevicesConfig.h"

#include <fstream>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace gw::adapters {

const core::uart::Profile& DevicesConfig::uart_device(const std::string& uart_device_id) const {
  for (const auto& d : devices) {
    if (d.device_id == uart_device_id) {
      return d;
    }
  }
  throw std::out_of_range("devices.yaml にない: " + uart_device_id);
}

DevicesConfig parse_devices_config(const std::string& yaml_text, const std::string& source,
                                   const std::filesystem::path& profiles_dir,
                                   const std::string& uart_device_id) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    throw ConfigError(source, {std::string{"YAML として読めない: "} + e.what()});
  }
  std::vector<std::string> errors;
  DevicesConfig config;
  if (!root.IsMap() || !root["devices"] || !root["devices"].IsSequence()) {
    throw ConfigError(source, {"/devices: 配列がない"});
  }
  for (const auto& item : root) {
    if (item.first.as<std::string>() != "devices") {
      errors.push_back("/" + item.first.as<std::string>() + ": 知らない項目");
    }
  }
  std::set<std::string> ids;
  const auto devices = root["devices"];
  for (std::size_t i = 0; i < devices.size(); ++i) {
    const auto ptr = "/devices/" + std::to_string(i);
    const auto node = devices[i];
    try {
      core::uart::Profile profile;
      if (node.IsMap() && node["profile"]) {
        if (node.size() != 1 || !node["profile"].IsScalar()) {
          errors.push_back(ptr + ": profile を使うときはほかの項目を書かない");
          continue;
        }
        profile = core::uart::load_profile(profiles_dir, node["profile"].as<std::string>());
      } else {
        YAML::Emitter out;
        out << node;
        profile = core::uart::parse_profile(out.c_str(), source + ptr);
      }
      if (!ids.insert(profile.device_id).second) {
        errors.push_back(ptr + "/device_id: 重複している '" + profile.device_id + "'");
        continue;
      }
      config.devices.push_back(std::move(profile));
    } catch (const core::uart::ProfileError& e) {
      errors.push_back(ptr + ": " + e.what());
    }
  }
  const bool has_uart = std::any_of(
      config.devices.begin(), config.devices.end(),
      [&](const auto& d) { return d.device_id == uart_device_id && d.transport == "uart"; });
  if (!has_uart) {
    errors.push_back("/devices: gateway.yaml の uart.device_id '" + uart_device_id +
                     "' が transport: uart のデバイスとして無い");
  }
  if (!errors.empty()) {
    throw ConfigError(source, errors);
  }
  return config;
}

DevicesConfig load_devices_config(const std::filesystem::path& path,
                                  const std::filesystem::path& profiles_dir,
                                  const std::string& uart_device_id) {
  const std::ifstream in(path);
  if (!in) {
    throw ConfigError(path.string(), {"ファイルを開けない"});
  }
  std::stringstream text;
  text << in.rdbuf();
  return parse_devices_config(text.str(), path.string(), profiles_dir, uart_device_id);
}

}  // namespace gw::adapters
