#ifndef DOS_COMMON_SHA256_H_
#define DOS_COMMON_SHA256_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dos {

// Streaming SHA-256 (FIPS 180-4). Self-contained; no external crypto library.
// Use this for content integrity checksums, not for password hashing.
class Sha256 {
public:
  Sha256();

  void Update(const void* data, std::size_t len);
  void Update(std::string_view data) { Update(data.data(), data.size()); }

  // Finalizes and returns the 64-char lowercase hex digest. The object must not
  // be reused after Finalize without calling Reset.
  std::string HexDigest();

  void Reset();

private:
  void ProcessBlock(const uint8_t block[64]);

  uint32_t state_[8];
  uint64_t bit_count_;
  uint8_t buffer_[64];
  std::size_t buffer_len_;
};

// Convenience one-shot: hex SHA-256 of the whole buffer.
std::string Sha256Hex(std::string_view data);

} // namespace dos

#endif // DOS_COMMON_SHA256_H_
