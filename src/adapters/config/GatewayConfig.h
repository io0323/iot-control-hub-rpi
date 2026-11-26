// gateway.yaml の読み込みと検証（基本設計 第7章「設定ファイル」、詳細設計書 第4章「起動の手順」）。
// 誤りは理由のリストを持つ ConfigError。app は理由をログに出して終了コード 2 で終わる。
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace gw::adapters {

class ConfigError : public std::runtime_error {
 public:
  ConfigError(const std::string& source, std::vector<std::string> errors);
  [[nodiscard]] const std::vector<std::string>& errors() const { return errors_; }

 private:
  std::vector<std::string> errors_;
};

struct GatewayConfig {
  std::string site = "home";
  struct Mqtt {
    std::string host = "localhost";
    int port = 1883;
    int keepalive_s = 60;  // open-questions #10（io の決定 2026-11-26）
  } mqtt;
  struct Uart {
    std::string path;  // --uart で上書きできる
    int baud = 115200;
    std::string device_id = "mcu-c-01";
  } uart;
  // profiles/{device_id}.yaml を置いたディレクトリ。省略時は実行ファイルの ../share/iot-gw/profiles
  std::optional<std::filesystem::path> profiles_dir;
  // schemas/*.schema.json を置いたディレクトリ。省略時は実行ファイルの ../share/iot-gw/schemas
  std::optional<std::filesystem::path> schemas_dir;
  // devices.yaml（静的に登録するデバイス）。省略時は gateway.yaml と同じディレクトリの devices.yaml
  std::filesystem::path devices_file = "devices.yaml";
  struct Queues {
    std::size_t inbound = 4096;
    std::size_t uart_out = 64;
    std::size_t mqtt_out = 4096;
    std::size_t mqtt_hold = 2000;  // 切断中に mqtt-tx が保持する event・cmd・ack
  } queues;
  std::string log_level = "info";
};

[[nodiscard]] GatewayConfig parse_gateway_config(const std::string& yaml_text,
                                                 const std::string& source,
                                                 const std::filesystem::path& base_dir);
[[nodiscard]] GatewayConfig load_gateway_config(const std::filesystem::path& path);

}  // namespace gw::adapters
