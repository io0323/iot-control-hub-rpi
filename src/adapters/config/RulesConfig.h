// rules.yaml（基本設計 第6章「ルール定義」、FR-07。形は core/rules/Rule.h のコメント）の読み込みと検証。
//
// 検証すること（誤りはすべて理由のリストにして ConfigError。起動時は終了コード 2、再読込では旧いまま）：
// - id は英小文字・数字・ハイフンで重複なし。priority は safety・manual・auto・schedule
// - when は all・any・not と葉（チャネル {device, channel, op, value}、生存状態 {device, status}、
//   システムモード {system_mode}、時刻帯 {time: {from, to}}）のどれか1つ
// - devices.yaml にあるデバイスなら、チャネル・アクチュエーターがプロファイルにあるか
// - アクション：command（op は共通仕様の op。set_actuator は args.id・level 0〜1000。lease は秒の整数
//   1〜600 で省略は60秒。lease・hold は set_actuator だけ）、set_system_mode（ALERT・EMERGENCY。
//   EMERGENCY は safety のルールだけ。open-questions #35）、notify（level は info・warning・critical、
//   message は文字列。1ルールに1つまで）、reconnect
// - 時間は "10s"・"5m"・"1h"・"500ms"（open-questions #40）。clear_when_below は使わない（hysteresis）
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "adapters/config/GatewayConfig.h"
#include "core/rules/Rule.h"
#include "core/uart/Profile.h"

namespace gw::adapters {

// "10s"・"5m"・"1h"・"500ms"。形が違えば nullopt
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_duration(const std::string& text);

[[nodiscard]] core::rules::RuleSet parse_rules_config(
    const std::string& yaml_text, const std::string& source,
    const std::vector<core::uart::Profile>& devices);
[[nodiscard]] core::rules::RuleSet load_rules_config(
    const std::filesystem::path& path, const std::vector<core::uart::Profile>& devices);

}  // namespace gw::adapters
