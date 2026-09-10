#ifndef DOS_COMMON_HASH_H_
#define DOS_COMMON_HASH_H_

#include <cstdint>
#include <string_view>

namespace dos {

// Stable, portable 64-bit hash derived from SHA-256 (the top 8 bytes, big-endian).
//
// Unlike std::hash, this is deterministic across platforms, runs, and standard
// library implementations, which is a hard requirement for the consistent-hash
// ring: object placement must not change just because the binary was rebuilt on
// a different machine.
uint64_t Hash64(std::string_view data);

} // namespace dos

#endif // DOS_COMMON_HASH_H_
