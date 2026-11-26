#include "core/uart/SeqExtender.h"

#include "core/common/Constants.h"

namespace gw::core::uart {

std::uint32_t SeqExtender::extend(std::uint16_t seq16) {
  if (last_ && seq16 < *last_) {
    base_ += constants::kUartSeqModulo;
  }
  last_ = seq16;
  return static_cast<std::uint32_t>((base_ + seq16) & constants::kUint32Max);
}

}  // namespace gw::core::uart
