#include "adapters/config/RulesConfig.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "core/common/Constants.h"
#include "core/mode/SystemModeController.h"

namespace gw::adapters {

namespace {

namespace r = core::rules;
using core::Json;

const std::regex kRuleIdRe("^[a-z0-9][a-z0-9-]*$");
const std::regex kDurationRe("^([0-9]+)(ms|s|m|h)$");
const std::regex kClockRe("^([01][0-9]|2[0-3]):([0-5][0-9])$");
constexpr std::array<std::string_view, 5> kOps{"set_actuator", "release", "set_mode", "set_param",
                                               "get_status"};
constexpr std::array<std::string_view, 3> kLevels{"info", "warning", "critical"};
constexpr int kLevelMax = 1000;
constexpr int kMinutesPerHour = 60;

struct Reader {
  std::vector<std::string> errors;
  const std::vector<core::uart::Profile>& devices;

  void error(const std::string& ptr, const std::string& what) {
    std::string e = ptr;
    e += ": ";
    e += what;
    errors.push_back(std::move(e));
  }

  const core::uart::Profile* profile(const std::string& device) const {
    const auto it = std::find_if(devices.begin(), devices.end(),
                                 [&](const auto& d) { return d.device_id == device; });
    return it == devices.end() ? nullptr : &*it;
  }

  void only(const YAML::Node& node, std::initializer_list<std::string_view> keys,
            const std::string& ptr) {
    for (const auto& item : node) {
      const auto key = item.first.as<std::string>();
      if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        error(ptr + "/" + key, "知らない項目");
      }
    }
  }

  std::optional<std::string> string(const YAML::Node& node, const std::string& ptr) {
    if (!node || !node.IsScalar() || node.as<std::string>().empty()) {
      error(ptr, "文字列がない");
      return std::nullopt;
    }
    return node.as<std::string>();
  }

  std::optional<double> number(const YAML::Node& node, const std::string& ptr) {
    try {
      if (node && node.IsScalar()) {
        return node.as<double>();
      }
    } catch (const YAML::Exception&) {
    }
    error(ptr, "数値でない");
    return std::nullopt;
  }

  std::optional<std::chrono::milliseconds> duration(const YAML::Node& node,
                                                    const std::string& ptr) {
    if (!node) {
      return std::chrono::milliseconds{0};
    }
    const auto d = node.IsScalar() ? parse_duration(node.as<std::string>()) : std::nullopt;
    if (!d) {
      error(ptr, "時間の形でない（例：10s・5m・1h・500ms）");
    }
    return d;
  }

  std::optional<int> clock(const YAML::Node& node, const std::string& ptr) {
    std::smatch m;
    const auto text = node && node.IsScalar() ? node.as<std::string>() : "";
    if (!std::regex_match(text, m, kClockRe)) {
      error(ptr, "時刻の形でない（HH:MM）");
      return std::nullopt;
    }
    return std::stoi(m[1].str()) * kMinutesPerHour + std::stoi(m[2].str());
  }

  r::Condition condition(const YAML::Node& node, const std::string& ptr) {
    r::Condition c;
    if (!node || !node.IsMap()) {
      error(ptr, "object でない");
      return c;
    }
    const auto group = [&](const char* key, r::Condition::Kind kind) {
      only(node, {key}, ptr);
      c.kind = kind;
      const auto list = node[key];
      if (!list.IsSequence() || list.size() == 0) {
        error(ptr + "/" + key, "空でない配列でない");
        return;
      }
      for (std::size_t i = 0; i < list.size(); ++i) {
        c.children.push_back(condition(list[i], ptr + "/" + key + "/" + std::to_string(i)));
      }
    };
    if (node["all"]) {
      group("all", r::Condition::Kind::kAll);
    } else if (node["any"]) {
      group("any", r::Condition::Kind::kAny);
    } else if (node["not"]) {
      only(node, {"not"}, ptr);
      c.kind = r::Condition::Kind::kNot;
      c.children.push_back(condition(node["not"], ptr + "/not"));
    } else if (node["channel"]) {
      only(node, {"device", "channel", "op", "value"}, ptr);
      c.kind = r::Condition::Kind::kChannel;
      r::ChannelLeaf leaf;
      leaf.device = string(node["device"], ptr + "/device").value_or("");
      leaf.channel = string(node["channel"], ptr + "/channel").value_or("");
      const auto op = node["op"] && node["op"].IsScalar() ? r::parse_op(node["op"].as<std::string>())
                                                          : std::nullopt;
      if (!op) {
        error(ptr + "/op", ">=・>・<=・<・==・!= のどれかでない");
      }
      leaf.op = op.value_or(r::Op::kGe);
      leaf.value = number(node["value"], ptr + "/value").value_or(0.0);
      if (leaf.device == r::kAnyDevice) {
        error(ptr + "/device", "チャネルの条件には \"*\" を使えない");
      } else if (const auto* p = profile(leaf.device);
                 p != nullptr && std::none_of(p->channels.begin(), p->channels.end(),
                                              [&](const auto& ch) {
                                                return ch.channel == leaf.channel;
                                              })) {
        error(ptr + "/channel", leaf.device + " のプロファイルにない '" + leaf.channel + "'");
      }
      c.leaf = leaf;
    } else if (node["status"]) {
      only(node, {"device", "status"}, ptr);
      c.kind = r::Condition::Kind::kStatus;
      r::StatusLeaf leaf;
      leaf.device = string(node["device"], ptr + "/device").value_or("");
      const auto status = string(node["status"], ptr + "/status").value_or("");
      if (status == "online") {
        leaf.status = core::device::DeviceStatus::kOnline;
      } else if (status == "suspect") {
        leaf.status = core::device::DeviceStatus::kSuspect;
      } else if (status == "offline") {
        leaf.status = core::device::DeviceStatus::kOffline;
      } else if (status == "unknown") {
        leaf.status = core::device::DeviceStatus::kUnknown;
      } else {
        error(ptr + "/status", "online・suspect・offline・unknown のどれかでない");
      }
      c.leaf = leaf;
    } else if (node["system_mode"]) {
      only(node, {"system_mode"}, ptr);
      c.kind = r::Condition::Kind::kMode;
      const auto mode = string(node["system_mode"], ptr + "/system_mode").value_or("");
      if (!core::mode::is_mode(mode)) {
        error(ptr + "/system_mode", "NORMAL・ALERT・EMERGENCY のどれかでない");
      }
      c.leaf = r::ModeLeaf{mode};
    } else if (node["time"]) {
      only(node, {"time"}, ptr);
      c.kind = r::Condition::Kind::kTime;
      const auto t = node["time"];
      if (!t.IsMap()) {
        error(ptr + "/time", "object でない");
      } else {
        only(t, {"from", "to"}, ptr + "/time");
        c.leaf = r::TimeLeaf{clock(t["from"], ptr + "/time/from").value_or(0),
                             clock(t["to"], ptr + "/time/to").value_or(0)};
      }
    } else {
      error(ptr, "all・any・not・channel・status・system_mode・time のどれもない");
    }
    return c;
  }

  std::optional<r::Action> action(const YAML::Node& node, const r::Rule& rule,
                                  const std::string& ptr) {
    if (!node || !node.IsMap()) {
      error(ptr, "object でない");
      return std::nullopt;
    }
    const auto kind = string(node["action"], ptr + "/action").value_or("");
    if (kind == "command") {
      only(node, {"action", "device", "op", "args", "lease", "hold"}, ptr);
      r::CommandAction a;
      a.device = string(node["device"], ptr + "/device").value_or("");
      a.op = string(node["op"], ptr + "/op").value_or("");
      if (std::find(kOps.begin(), kOps.end(), a.op) == kOps.end()) {
        error(ptr + "/op", "共通仕様にない op '" + a.op + "'");
      }
      if (a.device == r::kAnyDevice) {
        error(ptr + "/device", "指令の宛先に \"*\" を使えない");
      }
      if (const auto args = node["args"]) {
        if (!args.IsMap()) {
          error(ptr + "/args", "object でない");
        } else {
          for (const auto& item : args) {
            const auto key = item.first.as<std::string>();
            const auto& v = item.second;
            try {
              a.args[key] = (key == "level") ? Json(v.as<int>()) : Json(v.as<std::string>());
            } catch (const YAML::Exception&) {
              error(ptr + "/args/" + key, "値が読めない");
            }
          }
        }
      }
      const bool set_actuator = a.op == "set_actuator";
      if (set_actuator || a.op == "release") {
        const auto id = a.args.value("id", "");
        if (id.empty()) {
          error(ptr + "/args/id", "アクチュエーターがない");
        } else if (const auto* p = profile(a.device);
                   p != nullptr && std::none_of(p->actuators.begin(), p->actuators.end(),
                                                [&](const auto& act) { return act.id == id; })) {
          error(ptr + "/args/id", a.device + " のプロファイルにない '" + id + "'");
        }
      }
      if (set_actuator) {
        const auto level = a.args.contains("level") && a.args["level"].is_number_integer()
                               ? a.args["level"].get<int>()
                               : -1;
        if (level < 0 || level > kLevelMax) {
          error(ptr + "/args/level", "0〜1000 の整数でない");
        }
        const auto lease = duration(node["lease"], ptr + "/lease");
        const auto seconds =
            lease && lease->count() > 0
                ? std::chrono::duration_cast<std::chrono::seconds>(*lease).count()
                : core::constants::kLeaseDefaultS;
        if (lease && lease->count() > 0 &&
            (lease->count() % 1000 != 0 || seconds < core::constants::kLeaseMinS ||
             seconds > core::constants::kLeaseMaxS)) {
          error(ptr + "/lease", "秒の整数で 1〜600 でない");
        }
        a.lease_s = static_cast<int>(seconds);
        if (const auto hold = node["hold"]) {
          try {
            a.hold = hold.as<bool>();
          } catch (const YAML::Exception&) {
            error(ptr + "/hold", "true・false でない");
          }
        }
      } else if (node["lease"] || node["hold"]) {
        error(ptr, "lease・hold は set_actuator だけに書ける");
      }
      return a;
    }
    if (kind == "set_system_mode") {
      only(node, {"action", "mode"}, ptr);
      const auto mode = string(node["mode"], ptr + "/mode").value_or("");
      if (mode != core::mode::kAlert && mode != core::mode::kEmergency) {
        error(ptr + "/mode", "ALERT・EMERGENCY のどれかでない（NORMAL へは承認か自動で戻る）");
      } else if (mode == core::mode::kEmergency && rule.priority != core::command::Priority::kSafety) {
        error(ptr + "/mode", "EMERGENCY に入れられるのは priority safety のルールだけ");
      }
      return r::ModeAction{mode};
    }
    if (kind == "notify") {
      only(node, {"action", "level", "message"}, ptr);
      r::NotifyAction a;
      a.level = string(node["level"], ptr + "/level").value_or("info");
      if (std::find(kLevels.begin(), kLevels.end(), a.level) == kLevels.end()) {
        error(ptr + "/level", "info・warning・critical のどれかでない");
      }
      if (node["message"]) {
        a.message = string(node["message"], ptr + "/message");
      }
      return a;
    }
    if (kind == "reconnect") {
      only(node, {"action"}, ptr);
      return r::ReconnectAction{};
    }
    if (!kind.empty()) {
      error(ptr + "/action", "command・set_system_mode・notify・reconnect のどれかでない");
    }
    return std::nullopt;
  }
};

}  // namespace

std::optional<std::chrono::milliseconds> parse_duration(const std::string& text) {
  std::smatch m;
  if (!std::regex_match(text, m, kDurationRe)) {
    return std::nullopt;
  }
  const auto n = std::stoll(m[1].str());
  const auto unit = m[2].str();
  if (unit == "ms") {
    return std::chrono::milliseconds{n};
  }
  if (unit == "s") {
    return std::chrono::seconds{n};
  }
  if (unit == "m") {
    return std::chrono::minutes{n};
  }
  return std::chrono::hours{n};
}

core::rules::RuleSet parse_rules_config(const std::string& yaml_text, const std::string& source,
                                        const std::vector<core::uart::Profile>& devices) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    throw ConfigError(source, {std::string{"YAML として読めない: "} + e.what()});
  }
  if (!root.IsMap() || !root["rules"] || !root["rules"].IsSequence()) {
    throw ConfigError(source, {"/rules: 配列がない"});
  }
  Reader rd{{}, devices};
  rd.only(root, {"rules"}, "");
  core::rules::RuleSet set;
  std::set<std::string> ids;
  const auto list = root["rules"];
  for (std::size_t i = 0; i < list.size(); ++i) {
    const auto ptr = "/rules/" + std::to_string(i);
    const auto node = list[i];
    if (!node.IsMap()) {
      rd.error(ptr, "object でない");
      continue;
    }
    rd.only(node, {"id", "priority", "when", "for", "hysteresis", "cooldown", "then"}, ptr);
    core::rules::Rule rule;
    rule.id = rd.string(node["id"], ptr + "/id").value_or("");
    if (!rule.id.empty() && !std::regex_match(rule.id, kRuleIdRe)) {
      rd.error(ptr + "/id", "英小文字・数字・ハイフンでない");
    } else if (!rule.id.empty() && !ids.insert(rule.id).second) {
      rd.error(ptr + "/id", "重複している '" + rule.id + "'");
    }
    const auto priority = node["priority"] && node["priority"].IsScalar()
                              ? core::command::parse_priority(node["priority"].as<std::string>())
                              : std::nullopt;
    if (!priority) {
      rd.error(ptr + "/priority", "safety・manual・auto・schedule のどれかでない");
    }
    rule.priority = priority.value_or(core::command::Priority::kAuto);
    rule.when = rd.condition(node["when"], ptr + "/when");
    rule.for_duration = rd.duration(node["for"], ptr + "/for").value_or(std::chrono::milliseconds{0});
    rule.cooldown =
        rd.duration(node["cooldown"], ptr + "/cooldown").value_or(std::chrono::milliseconds{0});
    if (node["hysteresis"]) {
      const auto h = rd.number(node["hysteresis"], ptr + "/hysteresis");
      if (h && *h <= 0.0) {
        rd.error(ptr + "/hysteresis", "正の数でない");
      }
      rule.hysteresis = h;
    }
    const auto then = node["then"];
    if (!then || !then.IsSequence() || then.size() == 0) {
      rd.error(ptr + "/then", "空でない配列でない");
    } else {
      int notifies = 0;
      for (std::size_t j = 0; j < then.size(); ++j) {
        if (auto a = rd.action(then[j], rule, ptr + "/then/" + std::to_string(j))) {
          notifies += std::holds_alternative<core::rules::NotifyAction>(*a) ? 1 : 0;
          rule.then.push_back(std::move(*a));
        }
      }
      if (notifies > 1) {
        rd.error(ptr + "/then", "notify は1つまで（RULE_FIRED の重大度は1つ）");
      }
    }
    set.rules.push_back(std::move(rule));
  }
  if (!rd.errors.empty()) {
    throw ConfigError(source, rd.errors);
  }
  return set;
}

core::rules::RuleSet load_rules_config(const std::filesystem::path& path,
                                       const std::vector<core::uart::Profile>& devices) {
  const std::ifstream in(path);
  if (!in) {
    throw ConfigError(path.string(), {"ファイルを開けない"});
  }
  std::stringstream text;
  text << in.rdbuf();
  return parse_rules_config(text.str(), path.string(), devices);
}

}  // namespace gw::adapters
