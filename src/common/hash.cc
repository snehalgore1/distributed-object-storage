#include "common/hash.h"

#include <string>

#include "common/sha256.h"

namespace dos {

uint64_t Hash64(std::string_view data) {
  Sha256 h;
  h.Update(data);
  const std::string hex = h.HexDigest(); // 64 lowercase hex chars

  // Parse the first 16 hex chars (8 bytes) as a big-endian uint64.
  auto nibble = [](char c) -> uint64_t {
    if (c >= '0' && c <= '9') {
      return static_cast<uint64_t>(c - '0');
    }
    return static_cast<uint64_t>(c - 'a' + 10); // hex digest is lowercase
  };
  uint64_t value = 0;
  for (int i = 0; i < 16; ++i) {
    value = (value << 4) | nibble(hex[static_cast<std::size_t>(i)]);
  }
  return value;
}

} // namespace dos
