// 電文の JSON Schema 検証（D6：draft 2020-12、jsoncons）。対応する Python：iotspec.schema.validate
// 電文の schema フィールド（例："iot.v1.cmd_ack"）から kind ごとの Schema を選んで検証する。
// Schema は iot-common-spec の schemas/ をそのまま読む（$ref は同じディレクトリのファイルに解決する）。
// mqtt-loop のスレッドだけが使う（スレッドをまたいで共有しない）。
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace gw::adapters {

struct ValidationResult {
  bool ok = false;
  // 拒否の理由：json_invalid（JSON として読めない・オブジェクトでない）、unknown_schema、schema_invalid
  std::string reason;
  std::string detail;
  std::string schema;                             // 選んだ schema（例：iot.v1.telemetry）
  std::string kind;                               // トピックの kind（例：telemetry）
  std::optional<nlohmann::ordered_json> message;  // ok のときだけ
};

class SchemaValidator {
 public:
  // schemas_dir の kind ごとの Schema を読み込んでコンパイルする。読めなければ std::runtime_error
  explicit SchemaValidator(const std::filesystem::path& schemas_dir);
  ~SchemaValidator();
  SchemaValidator(const SchemaValidator&) = delete;
  SchemaValidator& operator=(const SchemaValidator&) = delete;
  SchemaValidator(SchemaValidator&& other) noexcept;
  SchemaValidator& operator=(SchemaValidator&& other) noexcept;

  [[nodiscard]] ValidationResult validate(std::string_view text) const;
  // すでにパース済みの電文（UART 由来の電文の検証：open-questions #9 の一致テスト）
  [[nodiscard]] bool is_valid(const nlohmann::ordered_json& message) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gw::adapters
