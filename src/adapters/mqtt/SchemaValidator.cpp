#include "adapters/mqtt/SchemaValidator.h"

#include <fstream>
#include <stdexcept>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

#include "core/common/Topics.h"

namespace gw::adapters {

namespace {

namespace fs = std::filesystem;
namespace jsonschema = jsoncons::jsonschema;
using jsoncons::json;

json load(const fs::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("スキーマを開けない: " + path.string());
  }
  return json::parse(in);
}

// 'iot.v1.cmd_ack' → 'cmd_ack.schema.json'（iotspec.schema._file_name）
std::string file_name(std::string_view schema) {
  constexpr std::string_view kPrefix = "iot.v1.";
  return std::string{schema.substr(kPrefix.size())} + ".schema.json";
}

}  // namespace

struct SchemaValidator::Impl {
  std::map<std::string, jsonschema::json_schema<json>, std::less<>> compiled;

  [[nodiscard]] const jsonschema::json_schema<json>* find(std::string_view schema) const {
    const auto it = compiled.find(schema);
    return it == compiled.end() ? nullptr : &it->second;
  }
};

SchemaValidator::SchemaValidator(const fs::path& schemas_dir) : impl_(std::make_unique<Impl>()) {
  // $ref（common.schema.json#/$defs/...）は $id の同じディレクトリのファイルに解決する
  const auto resolver = [schemas_dir](const jsoncons::uri& uri) -> json {
    return load(schemas_dir / fs::path(std::string{uri.path()}).filename());
  };
  for (const auto& info : core::kKinds) {
    const auto path = schemas_dir / file_name(info.schema);
    impl_->compiled.emplace(std::string{info.schema},
                            jsonschema::make_json_schema(load(path), resolver));
  }
}

SchemaValidator::~SchemaValidator() = default;
SchemaValidator::SchemaValidator(SchemaValidator&&) noexcept = default;
SchemaValidator& SchemaValidator::operator=(SchemaValidator&&) noexcept = default;

ValidationResult SchemaValidator::validate(std::string_view text) const {
  ValidationResult result;
  json instance;
  try {
    instance = json::parse(text);
  } catch (const std::exception& e) {
    result.reason = "json_invalid";
    result.detail = e.what();
    return result;
  }
  if (!instance.is_object()) {
    result.reason = "json_invalid";
    result.detail = "JSON オブジェクトではない";
    return result;
  }
  const auto name = instance.contains("schema") && instance["schema"].is_string()
                        ? instance["schema"].as<std::string>()
                        : std::string{};
  const auto* schema = impl_->find(name);
  const auto info = core::kind_of_schema(name);
  if (schema == nullptr || !info) {
    result.reason = "unknown_schema";
    result.detail = "未知または欠落した schema フィールド: " + name;
    return result;
  }
  result.schema = name;
  result.kind = std::string{info->kind};
  if (!schema->is_valid(instance)) {
    result.reason = "schema_invalid";
    return result;
  }
  try {
    result.message = nlohmann::ordered_json::parse(text);
  } catch (const std::exception& e) {  // jsoncons が読めて nlohmann が読めない入力（起きない想定）
    result.reason = "json_invalid";
    result.detail = e.what();
    return result;
  }
  result.ok = true;
  return result;
}

bool SchemaValidator::is_valid(const nlohmann::ordered_json& message) const {
  return validate(message.dump()).ok;
}

}  // namespace gw::adapters
