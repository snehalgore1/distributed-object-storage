#include "common/digest.h"

#include "common/sha256.h"

namespace dos {

std::string KeyDigest(std::string_view key) { return Sha256Hex(key); }

std::filesystem::path PhysicalPath(const std::filesystem::path& data_root,
                                   std::string_view key_digest) {
  // Expect a hex digest; fan out on the first two byte-pairs to keep any single
  // directory from accumulating an unbounded number of entries.
  std::string_view aa = key_digest.substr(0, 2);
  std::string_view bb = key_digest.substr(2, 2);
  return data_root / std::string(aa) / std::string(bb) / std::string(key_digest);
}

} // namespace dos
