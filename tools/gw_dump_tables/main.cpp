// gw-dump-tables：C++ 側の対応表と定数を JSON で出す（詳細設計書 第8章の2段目）。
// tools/check_tables.py が iotspec の constants・mapping と突き合わせる。
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/common/Constants.h"
#include "core/common/Topics.h"
#include "core/uart/FrameCodec.h"
#include "core/uart/Mapping.h"

namespace {

using Json = nlohmann::ordered_json;
namespace c = gw::core::constants;
namespace u = gw::core::uart;

Json table(const u::ValueTable& t) {
  Json out = Json::object();
  for (const auto& pair : t.pairs()) {
    const auto& common = pair.second;
    if (common.has_value()) {
      out[std::to_string(pair.first)] = std::string{*common};
    } else {
      out[std::to_string(pair.first)] = nullptr;
    }
  }
  return out;
}

}  // namespace

namespace {

int dump() {
  Json out = Json::object();
  out["mapping"] = {
      {"QUALITY", table(u::quality_table())},   {"STATUS", table(u::status_table())},
      {"REASON", table(u::reason_table())},     {"EVENT_CODE", table(u::event_code_table())},
      {"SEVERITY", table(u::severity_table())}, {"RESET_REASON", table(u::reset_reason_table())},
  };
  Json kinds = Json::object();
  for (const auto& k : gw::core::kKinds) {
    kinds[std::string{k.kind}] = {
        {"schema", std::string{k.schema}}, {"qos", k.qos}, {"retain", k.retain}};
  }
  out["kinds"] = kinds;
  out["topic_format"] = std::string{gw::core::kTopicFormat};
  Json ops = Json::object();
  for (const auto& [code, name] : c::kUartOpNames) {
    ops[std::to_string(code)] = std::string{name};
  }
  out["uart_op_names"] = ops;
  Json args = Json::object();
  for (const auto& spec : u::cmd_args_specs()) {
    Json names = Json::array();
    for (const auto n : spec.names) {
      names.push_back(std::string{n});
    }
    args[std::to_string(spec.op)] = names;
  }
  out["cmd_arg_names"] = args;
  // iotspec.constants の名前と同じキーで出す
  out["constants"] = {
      {"DEVICE_GW_01", std::string{c::kDeviceGw01}},
      {"DEVICE_MCU_C_01", std::string{c::kDeviceMcuC01}},
      {"DEVICE_MCU_CPP_01", std::string{c::kDeviceMcuCpp01}},
      {"DEVICE_APP_01", std::string{c::kDeviceApp01}},
      {"DEFAULT_SITE", std::string{c::kDefaultSite}},
      {"UINT32_MAX", c::kUint32Max},
      {"BOOT_ID_BITS", c::kBootIdBits},
      {"UART_ACK_TIMEOUT_S", c::kUartAckTimeoutS},
      {"UART_CMD_MAX_RETRIES", c::kUartCmdMaxRetries},
      {"UART_CMD_REF_DEDUP_CAPACITY", c::kUartCmdRefDedupCapacity},
      {"UART_CMD_RATE_PER_S", c::kUartCmdRatePerS},
      {"UART_CMD_MAX_IN_FLIGHT", c::kUartCmdMaxInFlight},
      {"CMD_REF_MODULO", c::kCmdRefModulo},
      {"SEQ_DEDUP_CAPACITY", c::kSeqDedupCapacity},
      {"PUBLISH_RATE_AVG_PER_S", c::kPublishRateAvgPerS},
      {"PUBLISH_RATE_BURST_PER_S", c::kPublishRateBurstPerS},
      {"PUBLISH_RATE_TOTAL_PER_S", c::kPublishRateTotalPerS},
      {"MQTT_MAX_IN_FLIGHT", c::kMqttMaxInFlight},
      {"LEASE_DEFAULT_S", c::kLeaseDefaultS},
      {"LEASE_MIN_S", c::kLeaseMinS},
      {"LEASE_MAX_S", c::kLeaseMaxS},
      {"LEASE_RENEW_DIVISOR", c::kLeaseRenewDivisor},
      {"UART_BAUD_RATE", c::kUartBaudRate},
      {"UART_FRAME_DELIMITER", c::kUartFrameDelimiter},
      {"UART_HEADER_SIZE", c::kUartHeaderSize},
      {"UART_CRC_SIZE", c::kUartCrcSize},
      {"UART_PAYLOAD_MAX", c::kUartPayloadMax},
      {"UART_SEQ_MODULO", c::kUartSeqModulo},
      {"CRC16_POLY", c::kCrc16Poly},
      {"CRC16_INIT", c::kCrc16Init},
      {"CRC16_XOROUT", c::kCrc16XorOut},
      {"UART_TYPE_TELEMETRY", c::kUartTypeTelemetry},
      {"UART_TYPE_EVENT", c::kUartTypeEvent},
      {"UART_TYPE_HEARTBEAT", c::kUartTypeHeartbeat},
      {"UART_TYPE_STATE", c::kUartTypeState},
      {"UART_TYPE_DIAG", c::kUartTypeDiag},
      {"UART_TYPE_CMD", c::kUartTypeCmd},
      {"UART_TYPE_ACK", c::kUartTypeAck},
      {"UART_DIAG_PAYLOAD_SIZE", c::kUartDiagPayloadSize},
      {"UART_DIAG_U16_SATURATED", c::kUartDiagU16Saturated},
      {"UART_BOOT_EVENT_TO", c::kUartBootEventTo},
      {"UART_TTL_MIN_S", c::kUartTtlMinS},
      {"UART_TTL_MAX_S", c::kUartTtlMaxS},
      {"UART_OP_SET_FAN_OVERRIDE", c::kUartOpSetFanOverride},
      {"UART_OP_CLEAR_OVERRIDE", c::kUartOpClearOverride},
      {"UART_OP_SET_THRESHOLD", c::kUartOpSetThreshold},
      {"UART_OP_GET_STATUS", c::kUartOpGetStatus},
      {"UART_OP_GET_DIAG", c::kUartOpGetDiag},
      {"UART_HEARTBEAT_PERIOD_S", c::kUartHeartbeatPeriodS},
      {"UART_HEARTBEAT_LOSS_S", c::kUartHeartbeatLossS},
      {"UART_UPSTREAM_LOST_S", c::kUartUpstreamLostS},
      {"UART_STATE_PERIOD_S", c::kUartStatePeriodS},
      {"UART_INITIAL_DEVICE_STATE", std::string{c::kUartInitialDeviceState}},
  };
  std::cout << out.dump(2) << "\n";
  return 0;
}

}  // namespace

int main() {
  try {
    return dump();
  } catch (const std::exception& e) {
    std::cerr << "gw-dump-tables: " << e.what() << "\n";
    return 1;
  }
}
