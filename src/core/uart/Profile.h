// デバイスプロファイル（profiles/{device_id}.yaml、v0.2.1 #16）。対応する Python：iotspec.profile
// schemas/profile.schema.json と同じ形を確かめ、加えて commands の target が actuators にあるかなど
// Schema で書けない整合を確かめる。不正なら ProfileError（理由のリスト）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace gw::core::uart {

class ProfileError : public std::runtime_error {
 public:
  ProfileError(const std::string& source, std::vector<std::string> errors);
  [[nodiscard]] const std::vector<std::string>& errors() const { return errors_; }

 private:
  std::vector<std::string> errors_;
};

struct ChannelSpec {
  std::string channel;
  std::string unit;
  std::int64_t period_ms = 0;
};

struct ActuatorSpec {  // NOLINT(bugprone-exception-escape)：JSON の既定値の構築は投げない
  std::string id;
  nlohmann::ordered_json min;  // YAML の数値のまま（整数は整数で出す）
  nlohmann::ordered_json max;
  nlohmann::ordered_json max_on_s;  // 省略時は null
};

// 汎用指令（op と対象 id）→ UART の op と引数の作り方
struct CommandSpec {
  std::string op;
  std::uint8_t uart_op = 0;
  std::optional<std::string> target;
  std::vector<std::pair<std::string, std::string>> args;  // (UART の引数名, cmd の args の項目名)
  bool ttl_from_lease = false;
};

struct Profile {
  std::string device_id;
  std::string transport;
  std::vector<ChannelSpec> channels;
  std::vector<ActuatorSpec> actuators;
  std::map<int, std::string> states;
  std::vector<CommandSpec> commands;
  std::vector<std::string> modes;

  // 状態番号 → 状態名。表にない番号は UnknownValueError（Python の KeyError）
  [[nodiscard]] const std::string& state_name(int number) const;
  // 汎用指令に対応する UART の指令。target を持たない定義は op だけで一致させる
  [[nodiscard]] const CommandSpec* find_command(const std::string& op,
                                                const std::optional<std::string>& target) const;
  [[nodiscard]] bool has_op(const std::string& op) const;
};

[[nodiscard]] Profile parse_profile(const std::string& yaml_text, const std::string& source);
[[nodiscard]] Profile load_profile_file(const std::filesystem::path& path);
// profiles_dir/{device_id}.yaml を読み、device_id がファイル名と同じかも確かめる
[[nodiscard]] Profile load_profile(const std::filesystem::path& profiles_dir,
                                   const std::string& device_id);

}  // namespace gw::core::uart
