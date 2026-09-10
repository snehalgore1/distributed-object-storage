#ifndef DOS_STORAGE_OBJECT_STORE_H_
#define DOS_STORAGE_OBJECT_STORE_H_

#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/object_metadata.h"

namespace dos {

// The object storage contract: PUT / GET / HEAD / DELETE / LIST. Implementations
// must never expose a partially written object as committed, and must verify
// integrity on read (spec Milestone 1 invariants).
class ObjectStore {
public:
  virtual ~ObjectStore() = default;

  // Writes `data` under `key`, returning the committed metadata (with its
  // assigned version). Overwrites bump the version monotonically.
  virtual StatusOr<ObjectMetadata> Put(std::string_view key, std::string_view data) = 0;

  // Returns the object bytes, after verifying the stored checksum. A checksum
  // mismatch yields kChecksumMismatch; a missing key yields kNotFound.
  virtual StatusOr<std::string> Get(std::string_view key) = 0;

  // Returns metadata only; never reads or returns the payload.
  virtual StatusOr<ObjectMetadata> Head(std::string_view key) = 0;

  // Logically deletes the object (tombstone). kNotFound if absent.
  virtual Status Delete(std::string_view key) = 0;

  // Lists committed objects whose key starts with `prefix` ("" = all).
  virtual StatusOr<std::vector<ObjectMetadata>> List(std::string_view prefix) = 0;
};

} // namespace dos

#endif // DOS_STORAGE_OBJECT_STORE_H_
