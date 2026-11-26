// optional の中身へのポインタ（空なら nullptr）。clang-tidy 15 の bugprone-unchecked-optional-access は、
// map の要素や構造化束縛を通した optional の確認を追えないため、確認と取り出しをここに閉じる。
#pragma once

#include <optional>

namespace gw::core {

template <typename T>
[[nodiscard]] const T* opt_ptr(const std::optional<T>& o) {
  return o.has_value() ? &*o : nullptr;
}

template <typename T>
[[nodiscard]] T* opt_ptr(std::optional<T>& o) {
  return o.has_value() ? &*o : nullptr;
}

}  // namespace gw::core
