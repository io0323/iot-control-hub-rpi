// UART の16ビット連番を32ビットに広げる（第7章。bridge.SeqExtender）。
// UART は順序が入れ替わらないので、前の値より小さくなったら一周したとみなす。
// 同じ値が続いたときは一周とはみなさない。再起動（新しい STATE）では reset する。
#pragma once

#include <cstdint>
#include <optional>

namespace gw::core::uart {

class SeqExtender {
 public:
  [[nodiscard]] std::uint32_t extend(std::uint16_t seq16);
  void reset() {
    last_.reset();
    base_ = 0;
  }

 private:
  std::optional<std::uint16_t> last_;
  std::uint64_t base_ = 0;
};

}  // namespace gw::core::uart
