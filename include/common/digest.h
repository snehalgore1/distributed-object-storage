#ifndef DOS_COMMON_DIGEST_H_
#define DOS_COMMON_DIGEST_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace dos {

// Deterministic digest of a logical object key. This is what decides physical
// placement on disk; it is independent of the object's content checksum.
std::string KeyDigest(std::string_view key);

// Maps a key digest to a two-level fan-out path under `data_root`:
//   <data_root>/<aa>/<bb>/<full-hex-digest>
// The logical key is never used as a filename; it lives only in metadata.
std::filesystem::path PhysicalPath(const std::filesystem::path& data_root,
                                   std::string_view key_digest);

} // namespace dos

#endif // DOS_COMMON_DIGEST_H_
