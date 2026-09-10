#ifndef DOS_STORAGE_OBJECT_METADATA_H_
#define DOS_STORAGE_OBJECT_METADATA_H_

#include <cstdint>
#include <string>

namespace dos {

// Durable record describing one logical object version. Mirrors the metadata
// schema (spec Section 15.1). The physical bytes live on the filesystem; this
// record is the source of truth for what is committed.
struct ObjectMetadata {
  std::string key;        // logical key (never used as a filename)
  uint64_t size = 0;      // payload size in bytes
  std::string checksum;   // sha256 hex of the payload
  uint64_t version = 0;   // monotonically increasing per key
  int64_t created_at = 0; // unix epoch seconds when this version was committed
  bool deleted = false;   // tombstone marker
};

} // namespace dos

#endif // DOS_STORAGE_OBJECT_METADATA_H_
