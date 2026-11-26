// フレームの往復と壊れた入力（iotspec の
// test_frame_round_trip・test_corrupt_frames_are_discarded_with_reason）
#include <gtest/gtest.h>

#include <random>

#include "core/common/Constants.h"
#include "core/uart/FrameCodec.h"

namespace u = gw::core::uart;
namespace c = gw::core::constants;

namespace {

u::Bytes raw_frame(std::uint8_t type, std::uint16_t seq, const u::Bytes& payload,
                   std::optional<std::uint8_t> length = std::nullopt) {
  u::Bytes body{type, static_cast<std::uint8_t>(seq & 0xFFU), static_cast<std::uint8_t>(seq >> 8U),
                length.value_or(static_cast<std::uint8_t>(payload.size()))};
  body.insert(body.end(), payload.begin(), payload.end());
  const auto crc = u::crc16_ccitt_false(body);
  body.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
  body.push_back(static_cast<std::uint8_t>(crc >> 8U));
  auto out = u::cobs_encode(body);
  out.push_back(0);
  return out;
}

u::Bytes corrupt_crc(const u::Frame& frame) {
  auto encoded = u::encode_frame(frame);
  auto raw = *u::cobs_decode(std::span(encoded).first(encoded.size() - 1)).data;
  raw.back() ^= 0x01;
  auto out = u::cobs_encode(raw);
  out.push_back(0);
  return out;
}

std::string reason_of(const u::Decoded& d) {
  const auto* bad = std::get_if<u::Discarded>(&d);
  return bad == nullptr ? "(復号できた)" : bad->reason;
}

}  // namespace

TEST(FrameCodec, RoundTripEveryType) {
  std::mt19937 rng(7);
  const auto u8 = [&] { return static_cast<std::uint8_t>(rng()); };
  const auto u16 = [&] { return static_cast<std::uint16_t>(rng()); };
  const auto i16 = [&] { return static_cast<std::int16_t>(rng()); };
  const auto u32 = [&] { return static_cast<std::uint32_t>(rng()); };
  for (int n = 0; n < 300; ++n) {
    const std::vector<u::Frame> frames{
        u::Telemetry{u16(), u32(), i16(), u8(), u8(), u16()},
        u::Event{u16(), u32(), u8(), u8(), u8(), u8(), i16()},
        u::Heartbeat{u16(), u32()},
        u::State{u16(), u16(), u32(), u16()},
        u::Diag{u16(), u32(), u8(), u16(), u16(), u16(), u16(), u16()},
        u::Ack{u16(), u16(), u8(), u8(), u8(), u16()},
        u::Cmd{u16(), u16(), c::kUartOpSetFanOverride, u16(), {u16()}, {}},
        u::Cmd{u16(), u16(), c::kUartOpSetThreshold, u16(), {i16(), i16(), i16()}, {}},
        u::Cmd{u16(), u16(), c::kUartOpGetDiag, u16(), {}, {}},
        u::Cmd{u16(), u16(), 0x7F, u16(), {}, u::Bytes(rng() % 195, 0x01)},
    };
    for (const auto& frame : frames) {
      const auto data = u::encode_frame(frame);
      EXPECT_EQ(data.back(), 0);
      EXPECT_EQ(std::count(data.begin(), data.end() - 1, 0), 0);
      const auto decoded = u::decode_frame(data);
      ASSERT_TRUE(std::holds_alternative<u::Frame>(decoded)) << reason_of(decoded);
      EXPECT_EQ(std::get<u::Frame>(decoded), frame);
    }
  }
}

TEST(FrameCodec, DiagPayloadIsFifteenBytes) {
  const auto data = u::encode_frame(u::Diag{1, 2, 3, 4, 5, 6, 7, 8});
  const auto raw = *u::cobs_decode(std::span(data).first(data.size() - 1)).data;
  EXPECT_EQ(raw[0], 0x05);
  EXPECT_EQ(raw[3], 15);
  EXPECT_EQ(raw.size(), 4U + 15U + 2U);
}

TEST(FrameCodec, CorruptFramesAreDiscardedWithReason) {
  const std::vector<std::pair<u::Bytes, std::string>> cases{
      {corrupt_crc(u::Heartbeat{1, 2}), "crc_mismatch"},
      {{0x05, 0x11, 0x22, 0x00}, "cobs_invalid"},
      {{0x00}, "cobs_invalid"},
      {raw_frame(c::kUartTypeHeartbeat, 1, {1, 2, 3, 4}, 5), "length_mismatch"},
      {raw_frame(0x7E, 1, {}), "unknown_type"},
      {raw_frame(c::kUartTypeHeartbeat, 1, {1, 2}), "payload_size"},
      {raw_frame(c::kUartTypeDiag, 1, u::Bytes(14, 0)), "payload_size"},
      {raw_frame(c::kUartTypeDiag, 1, u::Bytes(16, 0)), "payload_size"},
      {raw_frame(c::kUartTypeDiag, 1, u::Bytes(15, 0), 16), "length_mismatch"},
      {raw_frame(c::kUartTypeCmd, 1, {0x01, 0x00, 0x01, 0x3c, 0x00}), "args_size"},
      {raw_frame(c::kUartTypeCmd, 1, {0x01, 0x00}), "payload_size"},
      {[] {
         auto out = u::cobs_encode(u::Bytes{0x03, 0x01});
         out.push_back(0);
         return out;
       }(),
       "too_short"},
  };
  for (const auto& [data, reason] : cases) {
    EXPECT_EQ(reason_of(u::decode_frame(data)), reason);
  }
}

TEST(FrameCodec, EncodeRejectsOutOfRange) {
  EXPECT_THROW((void)u::encode_frame(u::Cmd{0, 0, c::kUartOpSetFanOverride, 0, {1, 2}, {}}),
               std::invalid_argument);
  EXPECT_THROW((void)u::encode_frame(u::Cmd{0, 0, c::kUartOpSetFanOverride, 0, {70000}, {}}),
               std::invalid_argument);
  EXPECT_THROW((void)u::encode_frame(u::Cmd{0, 0, 0x7F, 0, {}, u::Bytes(196, 1)}),
               std::invalid_argument);
  EXPECT_NO_THROW((void)u::encode_frame(u::Cmd{0, 0, 0x7F, 0, {}, u::Bytes(195, 1)}));
}

TEST(FrameReader, SplitsStreamAndSkipsEmpty) {
  u::FrameReader reader;
  auto a = u::encode_frame(u::Heartbeat{0, 1});
  auto b = u::encode_frame(u::Heartbeat{1, 2});
  u::Bytes stream{0, 0};
  stream.insert(stream.end(), a.begin(), a.end());
  stream.insert(stream.end(), b.begin(), b.begin() + 3);
  auto out = reader.feed(stream);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(std::get<u::Frame>(out[0]), (u::Frame{u::Heartbeat{0, 1}}));
  out = reader.feed(std::span(b).subspan(3));
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(std::get<u::Frame>(out[0]), (u::Frame{u::Heartbeat{1, 2}}));
  EXPECT_EQ(reader.buffered(), 0U);
}
