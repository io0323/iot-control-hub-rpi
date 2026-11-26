// COBS と CRC-16/CCITT-FALSE（iotspec の tests/unit/test_uart.py と同じ独立した値で裏取りする）
#include <gtest/gtest.h>

#include <random>
#include <string>

#include "core/uart/Cobs.h"

namespace u = gw::core::uart;

namespace {

u::Bytes bytes(std::initializer_list<int> values) {
  u::Bytes out;
  for (const int v : values) {
    out.push_back(static_cast<std::uint8_t>(v));
  }
  return out;
}

u::Bytes range(int first, int last) {  // [first, last]
  u::Bytes out;
  for (int v = first; v <= last; ++v) {
    out.push_back(static_cast<std::uint8_t>(v));
  }
  return out;
}

u::Bytes concat(std::initializer_list<u::Bytes> parts) {
  u::Bytes out;
  for (const auto& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

}  // namespace

TEST(Crc16, CheckValue) {
  const std::string text = "123456789";
  const u::Bytes data(text.begin(), text.end());
  EXPECT_EQ(u::crc16_ccitt_false(data), 0x29B1);
  EXPECT_EQ(u::crc16_ccitt_false({}), 0xFFFF);
}

// Wikipedia「Consistent Overhead Byte Stuffing」の Encoding examples（区切りの 0x00 は除く）
TEST(Cobs, KnownVectors) {
  struct Case {
    u::Bytes plain;
    u::Bytes encoded;
  };
  const std::vector<Case> cases{
      {bytes({0x00}), bytes({0x01, 0x01})},
      {bytes({0x00, 0x00}), bytes({0x01, 0x01, 0x01})},
      {bytes({0x00, 0x11, 0x00}), bytes({0x01, 0x02, 0x11, 0x01})},
      {bytes({0x11, 0x22, 0x00, 0x33}), bytes({0x03, 0x11, 0x22, 0x02, 0x33})},
      {bytes({0x11, 0x22, 0x33, 0x44}), bytes({0x05, 0x11, 0x22, 0x33, 0x44})},
      {bytes({0x11, 0x00, 0x00, 0x00}), bytes({0x02, 0x11, 0x01, 0x01, 0x01})},
      {range(1, 254), concat({bytes({0xFF}), range(1, 254)})},
      {range(0, 254), concat({bytes({0x01, 0xFF}), range(1, 254)})},
      {range(1, 255), concat({bytes({0xFF}), range(1, 254), bytes({0x02, 0xFF})})},
      {concat({range(2, 255), bytes({0x00})}),
       concat({bytes({0xFF}), range(2, 255), bytes({0x01, 0x01})})},
      {concat({range(3, 255), bytes({0x00, 0x01})}),
       concat({bytes({0xFE}), range(3, 255), bytes({0x02, 0x01})})},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(u::cobs_encode(c.plain), c.encoded);
    const auto decoded = u::cobs_decode(c.encoded);
    ASSERT_TRUE(decoded.data.has_value()) << decoded.error;
    EXPECT_EQ(*decoded.data, c.plain);
  }
}

TEST(Cobs, RoundTripRandom) {
  std::mt19937 rng(42);
  for (int n = 0; n < 500; ++n) {
    u::Bytes data(static_cast<std::size_t>(rng() % 600));
    for (auto& b : data) {
      b = static_cast<std::uint8_t>(rng() % 4 == 0 ? 0 : rng());
    }
    const auto encoded = u::cobs_encode(data);
    EXPECT_EQ(std::count(encoded.begin(), encoded.end(), 0), 0);
    const auto decoded = u::cobs_decode(encoded);
    ASSERT_TRUE(decoded.data.has_value());
    EXPECT_EQ(*decoded.data, data);
  }
}

TEST(Cobs, RejectsInvalid) {
  EXPECT_FALSE(u::cobs_decode({}).data.has_value());
  EXPECT_FALSE(u::cobs_decode(bytes({0x05, 0x11, 0x22})).data.has_value());  // 末尾を超える
  EXPECT_FALSE(u::cobs_decode(bytes({0x03, 0x00, 0x11})).data.has_value());  // ブロックに 0x00
  EXPECT_FALSE(u::cobs_decode(bytes({0x00})).data.has_value());
}
