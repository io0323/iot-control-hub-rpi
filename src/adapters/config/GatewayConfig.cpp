#include "adapters/config/GatewayConfig.h"

#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace gw::adapters {

namespace {

const std::regex kTimezoneRe("^[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+)*$");

// IANA のタイムゾーン名が tzdata（TZDIR、既定 /usr/share/zoneinfo）にあるか
bool timezone_exists(const std::string& name) {
  if (!std::regex_match(name, kTimezoneRe)) {
    return false;
  }
  const char* dir = std::getenv("TZDIR");  // NOLINT(concurrency-mt-unsafe)：起動時に1回だけ
  const std::filesystem::path base = dir != nullptr ? dir : "/usr/share/zoneinfo";
  std::error_code ec;
  return std::filesystem::is_regular_file(base / name, ec);
}

const std::regex kSiteRe("^[a-z0-9]+(-[a-z0-9]+)*$");
const std::regex kDeviceIdRe("^[a-z][a-z0-9]*(-[a-z][a-z0-9]*)?-[0-9]{2,}$");
const std::set<std::string, std::less<>> kLogLevels{"trace", "debug", "info", "warn", "error"};
const std::set<int> kBauds{9600, 19200, 38400, 57600, 115200, 230400};
constexpr int kPortMax = 65535;
constexpr int kKeepaliveMax = 65535;

std::string join(const std::vector<std::string>& errors) {
  std::string out;
  for (const auto& e : errors) {
    out += out.empty() ? "" : "; ";
    out += e;
  }
  return out;
}

class Reader {
 public:
  void only(const YAML::Node& map, const std::set<std::string, std::less<>>& allowed,
            const std::string& ptr) {
    for (const auto& item : map) {
      const auto key = item.first.as<std::string>();
      if (!allowed.contains(key)) {
        std::string message = ptr;
        message.append("/").append(key).append(": 知らない項目");
        errors.push_back(std::move(message));
      }
    }
  }

  void string(const YAML::Node& node, const std::string& ptr, std::string& out) {
    if (!node) {
      return;
    }
    if (!node.IsScalar()) {
      errors.push_back(ptr + ": 文字列でない");
      return;
    }
    out = node.as<std::string>();
  }

  void integer(const YAML::Node& node, const std::string& ptr, int min, int max, int& out) {
    if (!node) {
      return;
    }
    int value = 0;
    if (!node.IsScalar() || !YAML::convert<int>::decode(node, value)) {
      errors.push_back(ptr + ": 整数でない");
      return;
    }
    if (value < min || value > max) {
      errors.push_back(ptr + ": " + std::to_string(min) + "〜" + std::to_string(max) + " でない");
      return;
    }
    out = value;
  }

  void capacity(const YAML::Node& node, const std::string& ptr, std::size_t& out) {
    int value = static_cast<int>(out);
    integer(node, ptr, 1, 1'000'000, value);
    out = static_cast<std::size_t>(value);
  }

  void map(const YAML::Node& node, const std::string& ptr) {
    if (node && !node.IsMap()) {
      errors.push_back(ptr + ": object でない");
    }
  }

  std::vector<std::string> errors;
};

}  // namespace

ConfigError::ConfigError(const std::string& source, std::vector<std::string> errors)
    : std::runtime_error(source + ": " + join(errors)), errors_(std::move(errors)) {
}

GatewayConfig parse_gateway_config(const std::string& yaml_text, const std::string& source,
                                   const std::filesystem::path& base_dir) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    throw ConfigError(source, {std::string{"YAML として読めない: "} + e.what()});
  }
  GatewayConfig config;
  Reader r;
  if (!root.IsMap()) {
    throw ConfigError(source, {": object でない"});
  }
  r.only(root,
         {"site", "mqtt", "uart", "profiles_dir", "schemas_dir", "devices_file", "requests_file",
          "rules_file", "timezone", "queues", "history", "admin", "log_level"},
         "");

  r.string(root["site"], "/site", config.site);
  if (!std::regex_match(config.site, kSiteRe)) {
    r.errors.emplace_back("/site: site 名の形でない（英小文字・数字・ハイフン）");
  }

  const auto mqtt = root["mqtt"];
  r.map(mqtt, "/mqtt");
  if (mqtt && mqtt.IsMap()) {
    r.only(mqtt, {"host", "port", "keepalive_s"}, "/mqtt");
    r.string(mqtt["host"], "/mqtt/host", config.mqtt.host);
    r.integer(mqtt["port"], "/mqtt/port", 1, kPortMax, config.mqtt.port);
    r.integer(mqtt["keepalive_s"], "/mqtt/keepalive_s", 5, kKeepaliveMax, config.mqtt.keepalive_s);
  }
  if (config.mqtt.host.empty()) {
    r.errors.emplace_back("/mqtt/host: 空");
  }

  const auto uart = root["uart"];
  r.map(uart, "/uart");
  if (uart && uart.IsMap()) {
    r.only(uart, {"path", "baud", "device_id"}, "/uart");
    r.string(uart["path"], "/uart/path", config.uart.path);
    r.integer(uart["baud"], "/uart/baud", 1, 4'000'000, config.uart.baud);
    if (!kBauds.contains(config.uart.baud)) {
      r.errors.emplace_back("/uart/baud: 扱えない速度 " + std::to_string(config.uart.baud));
    }
    r.string(uart["device_id"], "/uart/device_id", config.uart.device_id);
    if (!std::regex_match(config.uart.device_id, kDeviceIdRe)) {
      r.errors.emplace_back("/uart/device_id: device_id の形でない");
    }
  }

  const auto read_path = [&](const char* key) -> std::optional<std::filesystem::path> {
    std::string text;
    r.string(root[key], std::string{"/"} + key, text);
    if (text.empty()) {
      return std::nullopt;
    }
    const std::filesystem::path path{text};
    return path.is_absolute() ? path : base_dir / path;
  };
  config.profiles_dir = read_path("profiles_dir");
  config.schemas_dir = read_path("schemas_dir");
  config.devices_file = read_path("devices_file").value_or(base_dir / "devices.yaml");
  config.requests_file = read_path("requests_file").value_or(base_dir / "requests.yaml");
  config.rules_file = read_path("rules_file").value_or(base_dir / "rules.yaml");
  r.string(root["timezone"], "/timezone", config.timezone);
  if (!timezone_exists(config.timezone)) {
    r.errors.emplace_back("/timezone: タイムゾーンが見つからない '" + config.timezone + "'");
  }

  const auto queues = root["queues"];
  r.map(queues, "/queues");
  if (queues && queues.IsMap()) {
    r.only(queues, {"inbound", "uart_out", "mqtt_out", "mqtt_hold", "history"}, "/queues");
    r.capacity(queues["history"], "/queues/history", config.queues.history);
    r.capacity(queues["inbound"], "/queues/inbound", config.queues.inbound);
    r.capacity(queues["uart_out"], "/queues/uart_out", config.queues.uart_out);
    r.capacity(queues["mqtt_out"], "/queues/mqtt_out", config.queues.mqtt_out);
    r.capacity(queues["mqtt_hold"], "/queues/mqtt_hold", config.queues.mqtt_hold);
  }

  const auto under = [&](const char* section,
                         const char* key) -> std::optional<std::filesystem::path> {
    const auto node = root[section];
    r.map(node, std::string{"/"} + section);
    if (!node || !node.IsMap()) {
      return std::nullopt;
    }
    r.only(node, {key}, std::string{"/"} + section);
    std::string text;
    r.string(node[key], std::string{"/"} + section + "/" + key, text);
    if (text.empty()) {
      return std::nullopt;
    }
    const std::filesystem::path path{text};
    return path.is_absolute() ? path : base_dir / path;
  };
  if (const auto db = under("history", "db_path")) {
    config.history.db_path = *db;
  }
  if (const auto socket = under("admin", "socket")) {
    config.admin.socket = *socket;
  }

  r.string(root["log_level"], "/log_level", config.log_level);
  if (!kLogLevels.contains(config.log_level)) {
    r.errors.emplace_back("/log_level: trace・debug・info・warn・error のどれでもない");
  }

  if (!r.errors.empty()) {
    throw ConfigError(source, r.errors);
  }
  return config;
}

GatewayConfig load_gateway_config(const std::filesystem::path& path) {
  const std::ifstream in(path);
  if (!in) {
    throw ConfigError(path.string(), {"ファイルを開けない"});
  }
  std::stringstream text;
  text << in.rdbuf();
  return parse_gateway_config(text.str(), path.string(), path.parent_path());
}

}  // namespace gw::adapters
