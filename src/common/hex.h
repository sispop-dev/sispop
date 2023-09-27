#pragma once
#include <lokimq/hex.h>
#include <type_traits>
#include "span.h" // epee

namespace tools {
  // Reads a hex string directly into a trivially copyable type T without performing any temporary
  // allocation.  Returns false if the given string is not hex or does not match T in length,
  // otherwise copies directly into `x` and returns true.
  template <typename T, typename = std::enable_if_t<
    !std::is_const_v<T> && (std::is_trivially_copyable_v<T> || epee::is_byte_spannable<T>)
  >>
  bool hex_to_type(std::string_view hex, T& x) {
    if (!lokimq::is_hex(hex) || hex.size() != 2*sizeof(T))
      return false;
    lokimq::from_hex(hex.begin(), hex.end(), reinterpret_cast<char*>(&x));
    return true;
  }
}
