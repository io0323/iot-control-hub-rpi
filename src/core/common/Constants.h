// 共通仕様 v0.2.2 の定数（iotspec.constants と同じ値）。
// 値はここに集め、ほかの場所に直接書かない。ずれは gw-dump-tables と tools/check_tables.py
// で検知する。
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

namespace gw::core::constants {

using std::chrono::milliseconds;
using std::chrono::seconds;

// --- 予約済み device_id（第2章）
inline constexpr std::string_view kDeviceGw01 = "gw-01";
inline constexpr std::string_view kDeviceMcuC01 = "mcu-c-01";
inline constexpr std::string_view kDeviceMcuCpp01 = "mcu-cpp-01";
inline constexpr std::string_view kDeviceApp01 = "app-01";
inline constexpr std::string_view kDefaultSite = "home";

inline constexpr std::uint64_t kUint32Max = 0xFFFF'FFFFULL;
inline constexpr int kBootIdBits = 32;

// --- 区間ごとの約束（第4章）
inline constexpr int kUartAckTimeoutS = 2;
inline constexpr int kUartCmdMaxRetries = 3;
inline constexpr int kUartCmdRefDedupCapacity = 16;
inline constexpr int kUartCmdRatePerS = 1;
inline constexpr int kUartCmdMaxInFlight = 4;
inline constexpr std::uint32_t kCmdRefModulo = 65'536;

// --- 送信量の上限（第4章）
inline constexpr int kSeqDedupCapacity = 1024;
inline constexpr int kPublishRateAvgPerS = 10;
inline constexpr int kPublishRateBurstPerS = 50;
inline constexpr int kPublishRateTotalPerS = 200;
inline constexpr int kMqttMaxInFlight = 32;

// --- データ品質（第5章）
inline constexpr int kStalePeriodMultiplier = 3;
inline constexpr int kClockSkewThresholdS = 5 * 60;
inline constexpr int kClockSkewEventIntervalS = 60 * 60;
inline constexpr int kLatePeriodMultiplier = 3;
inline constexpr int kLateMaxAgeS = 5 * 60;

// --- リース（第4章）
inline constexpr int kLeaseDefaultS = 60;
inline constexpr int kLeaseMinS = 1;
inline constexpr int kLeaseMaxS = 600;
inline constexpr int kLeaseRenewDivisor = 3;

// --- UART フレーム（第7章）
inline constexpr int kUartBaudRate = 115'200;
inline constexpr std::uint8_t kUartFrameDelimiter = 0x00;
inline constexpr std::size_t kUartHeaderSize = 4;
inline constexpr std::size_t kUartCrcSize = 2;
inline constexpr std::size_t kUartPayloadMax = 200;
inline constexpr std::uint32_t kUartSeqModulo = 65'536;
inline constexpr std::uint16_t kCrc16Poly = 0x1021;
inline constexpr std::uint16_t kCrc16Init = 0xFFFF;
inline constexpr std::uint16_t kCrc16XorOut = 0x0000;

inline constexpr std::uint8_t kUartTypeTelemetry = 0x01;
inline constexpr std::uint8_t kUartTypeEvent = 0x02;
inline constexpr std::uint8_t kUartTypeHeartbeat = 0x03;
inline constexpr std::uint8_t kUartTypeState = 0x04;
inline constexpr std::uint8_t kUartTypeDiag = 0x05;
inline constexpr std::uint8_t kUartTypeCmd = 0x10;
inline constexpr std::uint8_t kUartTypeAck = 0x11;

inline constexpr std::size_t kUartDiagPayloadSize = 15;
inline constexpr std::uint16_t kUartDiagU16Saturated = 0xFFFF;
inline constexpr std::uint8_t kUartBootEventTo = 0;
inline constexpr int kUartTtlMinS = 1;
inline constexpr int kUartTtlMaxS = 600;

inline constexpr std::uint8_t kUartOpSetFanOverride = 0x01;
inline constexpr std::uint8_t kUartOpClearOverride = 0x02;
inline constexpr std::uint8_t kUartOpSetThreshold = 0x03;
inline constexpr std::uint8_t kUartOpGetStatus = 0x10;
inline constexpr std::uint8_t kUartOpGetDiag = 0x11;
inline constexpr std::array<std::pair<std::uint8_t, std::string_view>, 5> kUartOpNames{{
    {kUartOpSetFanOverride, "SET_FAN_OVERRIDE"},
    {kUartOpClearOverride, "CLEAR_OVERRIDE"},
    {kUartOpSetThreshold, "SET_THRESHOLD"},
    {kUartOpGetStatus, "GET_STATUS"},
    {kUartOpGetDiag, "GET_DIAG"},
}};

// --- 生存監視（第7章）
inline constexpr int kUartHeartbeatPeriodS = 1;
inline constexpr int kUartHeartbeatLossS = 3;
inline constexpr int kUartUpstreamLostS = 10;
inline constexpr int kUartStatePeriodS = 60;
inline constexpr std::string_view kUartInitialDeviceState = "INIT";

// --- 作品03 が使う時間（上の値から作る）
inline constexpr milliseconds kHeartbeatPeriod{seconds{kUartHeartbeatPeriodS}};
inline constexpr milliseconds kUartLiveness{seconds{kUartHeartbeatLossS}};
inline constexpr milliseconds kUartCmdInterval{seconds{1} / kUartCmdRatePerS};

}  // namespace gw::core::constants
