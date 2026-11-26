#include "adapters/config/RequestsConfig.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace gw::adapters {

namespace {

// 共通仕様 第3章の op（common.schema.json の op）
constexpr std::array<std::string_view, 5> kOps{"set_actuator", "release", "set_mode", "set_param",
                                               "get_status"};

bool takes_actuator(const std::string& op) {
  return op == "set_actuator" || op == "release";
}

}  // namespace

core::command::RequestRules parse_requests_config(const std::string& yaml_text,
                                                  const std::string& source,
                                                  const std::vector<core::uart::Profile>& devices) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    throw ConfigError(source, {std::string{"YAML として読めない: "} + e.what()});
  }
  if (!root.IsMap() || !root["allow"] || !root["allow"].IsSequence()) {
    throw ConfigError(source, {"/allow: 配列がない"});
  }
  std::vector<std::string> errors;
  for (const auto& item : root) {
    if (item.first.as<std::string>() != "allow") {
      errors.push_back("/" + item.first.as<std::string>() + ": 知らない項目");
    }
  }
  core::command::RequestRules rules;
  const auto allow = root["allow"];
  for (std::size_t i = 0; i < allow.size(); ++i) {
    const auto ptr = "/allow/" + std::to_string(i);
    const auto node = allow[i];
    if (!node.IsMap()) {
      errors.push_back(ptr + ": object でない");
      continue;
    }
    bool ok = true;
    for (const auto& item : node) {
      const auto key = item.first.as<std::string>();
      if (key != "target" && key != "op" && key != "actuators") {
        std::string error = ptr;
        error += "/";
        error += key;
        error += ": 知らない項目";
        errors.push_back(std::move(error));
        ok = false;
      }
    }
    core::command::RequestRule rule;
    if (!node["target"] || !node["target"].IsScalar() || node["target"].as<std::string>().empty()) {
      errors.push_back(ptr + "/target: 文字列がない");
      ok = false;
    } else {
      rule.target = node["target"].as<std::string>();
    }
    if (!node["op"] || !node["op"].IsScalar()) {
      errors.push_back(ptr + "/op: 文字列がない");
      ok = false;
    } else {
      rule.op = node["op"].as<std::string>();
      if (std::find(kOps.begin(), kOps.end(), rule.op) == kOps.end()) {
        errors.push_back(ptr + "/op: 共通仕様にない op '" + rule.op + "'");
        ok = false;
      }
    }
    if (const auto acts = node["actuators"]) {
      if (!takes_actuator(rule.op)) {
        errors.push_back(ptr + "/actuators: set_actuator・release のときだけ書ける");
        ok = false;
      } else if (!acts.IsSequence() || acts.size() == 0) {
        errors.push_back(ptr + "/actuators: 空でない配列でない");
        ok = false;
      } else {
        std::set<std::string> ids;
        const auto profile = std::find_if(devices.begin(), devices.end(), [&](const auto& d) {
          return d.device_id == rule.target;
        });
        for (std::size_t j = 0; j < acts.size(); ++j) {
          const auto id = acts[j].as<std::string>();
          if (profile != devices.end() &&
              std::none_of(profile->actuators.begin(), profile->actuators.end(),
                           [&](const auto& a) { return a.id == id; })) {
            std::string error = ptr;
            error += "/actuators/" + std::to_string(j);
            error += ": " + rule.target;
            error += " のプロファイルにない '" + id + "'";
            errors.push_back(std::move(error));
            ok = false;
          }
          ids.insert(id);
        }
        rule.actuators = std::move(ids);
      }
    }
    if (ok) {
      rules.allow.push_back(std::move(rule));
    }
  }
  if (!errors.empty()) {
    throw ConfigError(source, errors);
  }
  return rules;
}

core::command::RequestRules load_requests_config(const std::filesystem::path& path,
                                                 const std::vector<core::uart::Profile>& devices) {
  const std::ifstream in(path);
  if (!in) {
    throw ConfigError(path.string(), {"ファイルを開けない"});
  }
  std::stringstream text;
  text << in.rdbuf();
  return parse_requests_config(text.str(), path.string(), devices);
}

}  // namespace gw::adapters
