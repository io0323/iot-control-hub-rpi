#include "core/uart/Profile.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "core/common/Constants.h"
#include "core/uart/FrameCodec.h"
#include "core/uart/Mapping.h"

namespace gw::core::uart {

namespace {

// profile.schema.json と common.schema.json の値（v0.2.3：unit に count、mode に大文字）
const std::set<std::string, std::less<>> kUnits{"degC", "pct_rh", "pct", "level_pm", "hPa",
                                                "lux",  "bytes",  "rpm", "count"};
const std::set<std::string, std::less<>> kOps{"set_actuator", "release", "set_mode", "set_param",
                                              "get_status"};
const std::regex kDeviceIdRe("^[a-z][a-z0-9]*(-[a-z][a-z0-9]*)?-[0-9]{2,}$");
const std::regex kSnakeRe("^[a-z][a-z0-9]*(_[a-z0-9]+)*$");
const std::regex kStateNameRe("^[A-Z][A-Z0-9_]*$");
const std::regex kModeRe("^[A-Za-z][A-Za-z0-9_]*$");
constexpr int kMaxStateNumber = 255;

std::string join(const std::vector<std::string>& errors) {
  std::string out;
  for (const auto& e : errors) {
    out += out.empty() ? "" : "; ";
    out += e;
  }
  return out;
}

std::optional<std::uint8_t> uart_op_by_name(const std::string& name) {
  for (const auto& [code, op_name] : constants::kUartOpNames) {
    if (op_name == name) {
      return code;
    }
  }
  return std::nullopt;
}

bool is_number(const YAML::Node& node) {
  if (!node.IsScalar()) {
    return false;
  }
  double value = 0;
  return YAML::convert<double>::decode(node, value) && node.Tag() != "!";
}

nlohmann::ordered_json number_json(const YAML::Node& node) {
  std::int64_t integer = 0;
  if (YAML::convert<std::int64_t>::decode(node, integer)) {
    return integer;
  }
  return node.as<double>();
}

std::optional<std::string> string_of(const YAML::Node& node) {
  if (!node || !node.IsScalar()) {
    return std::nullopt;
  }
  return node.as<std::string>();
}

class Checker {
 public:
  void need(bool ok, const std::string& pointer, const std::string& message) {
    if (!ok) {
      errors.push_back(pointer + ": " + message);
    }
  }
  void only(const YAML::Node& map, const std::set<std::string, std::less<>>& allowed,
            const std::string& pointer) {
    for (const auto& item : map) {
      const auto key = item.first.as<std::string>();
      need(allowed.contains(key), pointer, "知らない項目 '" + key + "'");
    }
  }
  std::vector<std::string> errors;
};

ChannelSpec parse_channel(const YAML::Node& node, const std::string& ptr, Checker& c) {
  ChannelSpec spec;
  c.need(node.IsMap(), ptr, "object でない");
  if (!node.IsMap()) {
    return spec;
  }
  c.only(node, {"channel", "unit", "period_ms"}, ptr);
  const auto channel = string_of(node["channel"]);
  c.need(channel && std::regex_match(*channel, kSnakeRe), ptr + "/channel", "チャネル名でない");
  const auto unit = string_of(node["unit"]);
  c.need(unit && kUnits.contains(*unit), ptr + "/unit", "知らない単位");
  std::int64_t period = 0;
  const bool period_ok = node["period_ms"] && node["period_ms"].IsScalar() &&
                         YAML::convert<std::int64_t>::decode(node["period_ms"], period) &&
                         period >= 1;
  c.need(period_ok, ptr + "/period_ms", "1 以上の整数でない");
  spec.channel = channel.value_or("");
  spec.unit = unit.value_or("");
  spec.period_ms = period;
  return spec;
}

ActuatorSpec parse_actuator(const YAML::Node& node, const std::string& ptr, Checker& c) {
  ActuatorSpec spec;
  c.need(node.IsMap(), ptr, "object でない");
  if (!node.IsMap()) {
    return spec;
  }
  c.only(node, {"id", "min", "max", "max_on_s"}, ptr);
  const auto id = string_of(node["id"]);
  c.need(id && std::regex_match(*id, kSnakeRe), ptr + "/id", "アクチュエーター名でない");
  spec.id = id.value_or("");
  for (const auto* key : {"min", "max"}) {
    const auto value = node[key];
    c.need(value && is_number(value), ptr + "/" + key, "数値でない");
    if (value && is_number(value)) {
      (std::string{key} == "min" ? spec.min : spec.max) = number_json(value);
    }
  }
  spec.max_on_s = nullptr;
  if (const auto value = node["max_on_s"]) {
    const bool ok = is_number(value) && value.as<double>() >= 0;
    c.need(ok, ptr + "/max_on_s", "0 以上の数値でない");
    if (ok) {
      spec.max_on_s = number_json(value);
    }
  }
  return spec;
}

CommandSpec parse_command(const YAML::Node& node, const std::string& ptr, Checker& c) {
  CommandSpec spec;
  c.need(node.IsMap(), ptr, "object でない");
  if (!node.IsMap()) {
    return spec;
  }
  c.only(node, {"op", "target", "uart_op", "args", "ttl_s"}, ptr);
  const auto op = string_of(node["op"]);
  c.need(op && kOps.contains(*op), ptr + "/op", "知らない op");
  spec.op = op.value_or("");
  if (node["target"]) {
    const auto target = string_of(node["target"]);
    c.need(target && std::regex_match(*target, kSnakeRe), ptr + "/target",
           "アクチュエーター名でない");
    spec.target = target;
  }
  const auto uart_op_name = string_of(node["uart_op"]);
  const auto uart_op = uart_op_name ? uart_op_by_name(*uart_op_name) : std::nullopt;
  c.need(uart_op.has_value(), ptr + "/uart_op", "知らない UART の op");
  spec.uart_op = uart_op.value_or(0);
  if (const auto args = node["args"]) {
    c.need(args.IsMap(), ptr + "/args", "object でない");
    if (args.IsMap()) {
      for (const auto& item : args) {
        const auto field = string_of(item.second);
        c.need(field.has_value(), ptr + "/args", "値が文字列でない");
        spec.args.emplace_back(item.first.as<std::string>(), field.value_or(""));
      }
    }
  }
  if (const auto ttl = node["ttl_s"]) {
    const auto text = string_of(ttl);
    c.need(text == "lease_s", ptr + "/ttl_s", "'lease_s' でない");
    spec.ttl_from_lease = text == "lease_s";
  }
  return spec;
}

}  // namespace

ProfileError::ProfileError(const std::string& source, std::vector<std::string> errors)
    : std::runtime_error(source + ": " + join(errors)), errors_(std::move(errors)) {
}

const std::string& Profile::state_name(int number) const {
  const auto it = states.find(number);
  if (it == states.end()) {
    throw UnknownValueError("states: プロファイルにない状態番号 " + std::to_string(number));
  }
  return it->second;
}

const CommandSpec* Profile::find_command(const std::string& op,
                                         const std::optional<std::string>& target) const {
  for (const auto& spec : commands) {
    if (spec.op == op && (!spec.target || spec.target == target)) {
      return &spec;
    }
  }
  return nullptr;
}

bool Profile::has_op(const std::string& op) const {
  return std::any_of(commands.begin(), commands.end(), [&](const auto& c) { return c.op == op; });
}

Profile parse_profile(const std::string& yaml_text, const std::string& source) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    throw ProfileError(source, {std::string{"YAML として読めない: "} + e.what()});
  }
  Checker c;
  if (!root.IsMap()) {
    throw ProfileError(source, {": object でない"});
  }
  c.only(root, {"device_id", "transport", "channels", "actuators", "modes", "states", "commands"},
         "");
  Profile profile;
  const auto device_id = string_of(root["device_id"]);
  c.need(device_id && std::regex_match(*device_id, kDeviceIdRe), "/device_id", "device_id でない");
  profile.device_id = device_id.value_or("");
  const auto transport = string_of(root["transport"]);
  c.need(transport == "uart" || transport == "mqtt", "/transport", "uart か mqtt でない");
  profile.transport = transport.value_or("");

  for (const auto* key : {"channels", "actuators"}) {
    c.need(root[key] && root[key].IsSequence(), std::string{"/"} + key, "必須の配列がない");
  }
  if (root["channels"].IsSequence()) {
    for (std::size_t i = 0; i < root["channels"].size(); ++i) {
      profile.channels.push_back(
          parse_channel(root["channels"][i], "/channels/" + std::to_string(i), c));
    }
  }
  if (root["actuators"].IsSequence()) {
    for (std::size_t i = 0; i < root["actuators"].size(); ++i) {
      profile.actuators.push_back(
          parse_actuator(root["actuators"][i], "/actuators/" + std::to_string(i), c));
    }
  }
  if (const auto modes = root["modes"]) {
    c.need(modes.IsSequence() && modes.size() >= 1, "/modes", "1件以上の配列でない");
    if (modes.IsSequence()) {
      std::set<std::string> seen;
      for (const auto& mode : modes) {
        const auto text = string_of(mode);
        c.need(text && std::regex_match(*text, kModeRe), "/modes", "mode の形でない");
        c.need(!text || seen.insert(*text).second, "/modes", "重複している");
        profile.modes.push_back(text.value_or(""));
      }
    }
  }
  if (const auto states = root["states"]) {
    c.need(states.IsMap() && states.size() >= 1, "/states", "1件以上の object でない");
    if (states.IsMap()) {
      for (const auto& item : states) {
        int number = -1;
        const bool key_ok = YAML::convert<int>::decode(item.first, number) && number >= 0 &&
                            number <= kMaxStateNumber;
        const auto name = string_of(item.second);
        c.need(key_ok, "/states", "状態番号が 0〜255 でない");
        c.need(name && std::regex_match(*name, kStateNameRe), "/states", "状態名の形でない");
        if (key_ok && name) {
          profile.states[number] = *name;
        }
      }
    }
  }
  if (const auto commands = root["commands"]) {
    c.need(commands.IsSequence(), "/commands", "配列でない");
    if (commands.IsSequence()) {
      for (std::size_t i = 0; i < commands.size(); ++i) {
        profile.commands.push_back(parse_command(commands[i], "/commands/" + std::to_string(i), c));
      }
    }
  }
  if (profile.transport == "uart") {
    c.need(static_cast<bool>(root["states"]), "", "uart のプロファイルに states がない");
    c.need(static_cast<bool>(root["commands"]), "", "uart のプロファイルに commands がない");
  }
  if (!c.errors.empty()) {
    throw ProfileError(source, c.errors);
  }

  // Schema で書けない整合（profile.parse_profile）
  std::set<std::string> actuator_ids;
  for (const auto& a : profile.actuators) {
    actuator_ids.insert(a.id);
  }
  std::set<std::pair<std::string, std::string>> keys;
  for (std::size_t i = 0; i < profile.commands.size(); ++i) {
    const auto& cmd = profile.commands[i];
    const auto ptr = "/commands/" + std::to_string(i);
    c.need(!cmd.target || actuator_ids.contains(*cmd.target), ptr + "/target",
           "actuators にない '" + cmd.target.value_or("") + "'");
    std::set<std::string> names;
    for (const auto& arg : cmd.args) {
      names.insert(arg.first);
    }
    std::set<std::string> want;
    if (const auto* spec = cmd_args_spec(cmd.uart_op)) {
      for (const auto name : spec->names) {
        want.emplace(name);
      }
    }
    c.need(names == want, ptr + "/args", "UART の op の引数と合わない");
    c.need(keys.emplace(cmd.op, cmd.target.value_or("\x01")).second, "/commands",
           "同じ op と target の組が重複している");
  }
  if (!c.errors.empty()) {
    throw ProfileError(source, c.errors);
  }
  return profile;
}

Profile load_profile_file(const std::filesystem::path& path) {
  const std::ifstream in(path);
  if (!in) {
    throw ProfileError(path.string(), {"ファイルを開けない"});
  }
  std::stringstream text;
  text << in.rdbuf();
  return parse_profile(text.str(), path.string());
}

Profile load_profile(const std::filesystem::path& profiles_dir, const std::string& device_id) {
  const auto path = profiles_dir / (device_id + ".yaml");
  auto profile = load_profile_file(path);
  if (profile.device_id != device_id) {
    throw ProfileError(path.string(), {"/device_id: ファイル名と違う '" + profile.device_id + "'"});
  }
  return profile;
}

}  // namespace gw::core::uart
