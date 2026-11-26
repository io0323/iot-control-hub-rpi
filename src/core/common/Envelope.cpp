#include "core/common/Envelope.h"

#include <limits>
#include <stdexcept>

#include "core/common/Topics.h"

namespace gw::core {

std::string Envelope::kind() const {
  const auto info = kind_of_schema(schema);
  if (!info) {
    throw std::invalid_argument("unknown schema: " + schema);
  }
  return std::string{info->kind};
}

Json Envelope::to_json() const {
  Json message = Json::object();
  message["schema"] = schema;
  message["msg_id"] = msg_id;
  message["device_id"] = device_id;
  message["seq"] = seq;
  message["boot_id"] = boot_id;
  message["ts"] = format_ts(ts);
  message["ts_source"] = ts_source;
  if (uptime_ms) {
    message["uptime_ms"] = *uptime_ms;
  }
  message["payload"] = payload;
  return message;
}

std::uint32_t SeqCounter::next(const std::string& kind) {
  auto& value = next_[kind];
  const auto current = value;
  value = current == std::numeric_limits<std::uint32_t>::max() ? 0 : current + 1;
  return current;
}

std::uint32_t SeqCounter::peek(const std::string& kind) const {
  const auto it = next_.find(kind);
  return it == next_.end() ? 0 : it->second;
}

}  // namespace gw::core
