#include "core/uart/Mapping.h"

namespace gw::core::uart {

const ValueTable& quality_table() {
  static const ValueTable table("quality", {{0, "good"}, {1, "stale"}, {2, "fault"}});
  return table;
}

const ValueTable& status_table() {
  static const ValueTable table("status",
                                {{0, "accepted"}, {1, "done"}, {2, "rejected"}, {3, "failed"}});
  return table;
}

const ValueTable& reason_table() {
  static const ValueTable table("reason", {{0, std::nullopt},
                                           {1, "unsafe"},
                                           {2, "invalid_arg"},
                                           {3, "unknown_op"},
                                           {4, "expired"},
                                           {5, "internal"}});
  return table;
}

const ValueTable& event_code_table() {
  static const ValueTable table("event code", {{1, "BOOT"},
                                               {2, "STATE_CHANGED"},
                                               {3, "THRESHOLD_EXCEEDED"},
                                               {4, "THRESHOLD_CLEARED"},
                                               {5, "SENSOR_FAULT"},
                                               {6, "SENSOR_RECOVERED"},
                                               {7, "UPSTREAM_LOST"},
                                               {8, "UPSTREAM_RESTORED"},      // v0.2.1 #34
                                               {9, "INTERLOCK_ACTIVE"},       // v0.2.2 #39
                                               {10, "INTERLOCK_RELEASED"}});  // v0.2.2 #39
  return table;
}

const ValueTable& severity_table() {
  static const ValueTable table("severity", {{0, "info"}, {1, "warning"}, {2, "critical"}});
  return table;
}

const ValueTable& reset_reason_table() {
  // BOOT の EVENT の from と DIAG の reset_reason（v0.2.2 #38）
  static const ValueTable table("reset_reason", {{0, "unknown"},
                                                 {1, "power_on"},
                                                 {2, "pin"},
                                                 {3, "watchdog"},
                                                 {4, "software"},
                                                 {5, "brownout"},
                                                 {6, "low_power"}});
  return table;
}

std::string to_common_string(const ValueTable& table, int value) {
  const auto common = table.to_common(value);
  if (!common) {
    throw UnknownValueError(std::string{table.name()} + ": null になる値 " + std::to_string(value));
  }
  return std::string{*common};
}

}  // namespace gw::core::uart
