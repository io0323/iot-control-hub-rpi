// requests.yaml（基本設計 第7章「設定ファイル」：作品04 に許可する操作の一覧。open-questions #20）。
//
//   allow:
//     - {target: mcu-c-01, op: set_actuator, actuators: [fan_main]}
//     - {target: mcu-c-01, op: release, actuators: [fan_main]}
//     - {target: mcu-cpp-01, op: set_mode}
//
// - 優先度は request の kind（manual・schedule）で決まるので書かない（共通仕様 第3章）
// - actuators は set_actuator・release だけに書ける。省略するとその宛先の全アクチュエーター
// - target が devices.yaml にあるときは、actuators がそのプロファイルにあるかも確かめる
//
// 誤りは ConfigError。app は終了コード 2 で終わる。SIGHUP での再読込は Phase 4。
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "adapters/config/GatewayConfig.h"
#include "core/command/RequestHandler.h"
#include "core/uart/Profile.h"

namespace gw::adapters {

[[nodiscard]] core::command::RequestRules parse_requests_config(
    const std::string& yaml_text, const std::string& source,
    const std::vector<core::uart::Profile>& devices);
[[nodiscard]] core::command::RequestRules load_requests_config(
    const std::filesystem::path& path, const std::vector<core::uart::Profile>& devices);

}  // namespace gw::adapters
