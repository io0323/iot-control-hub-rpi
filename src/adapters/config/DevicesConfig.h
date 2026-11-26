// devices.yaml（基本設計 第7章「設定ファイル」、第0章「デバイス登録」）。
// 各デバイスは iotspec のデバイスプロファイルと同じ形で書く。
//
//   devices:
//     - profile: mcu-c-01          # profiles_dir/mcu-c-01.yaml（iot-common-spec 同梱）を使う
//     - device_id: mcu-cpp-02      # プロファイルと同じ形をその場に書く
//       transport: mqtt
//       channels: [{channel: temp_air, unit: degC, period_ms: 2000}]
//       actuators: []
//
// 誤り（読めない、プロファイルとして不正、device_id の重複、gateway.yaml の uart.device_id が
// transport: uart のデバイスとして無い）は ConfigError。app は終了コード 2 で終わる。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "adapters/config/GatewayConfig.h"
#include "core/uart/Profile.h"

namespace gw::adapters {

struct DevicesConfig {
  std::vector<core::uart::Profile> devices;
  // uart_device_id のプロファイル（transport: uart）
  [[nodiscard]] const core::uart::Profile& uart_device(const std::string& uart_device_id) const;
};

[[nodiscard]] DevicesConfig parse_devices_config(const std::string& yaml_text,
                                                 const std::string& source,
                                                 const std::filesystem::path& profiles_dir,
                                                 const std::string& uart_device_id);
[[nodiscard]] DevicesConfig load_devices_config(const std::filesystem::path& path,
                                                const std::filesystem::path& profiles_dir,
                                                const std::string& uart_device_id);

}  // namespace gw::adapters
