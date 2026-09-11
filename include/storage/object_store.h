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

  // Writes `data` at an externally-assigned `version` (used by the replication
  // coordinator so all replicas store the same version). `version` must be > 0.
  virtual StatusOr<ObjectMetadata> PutWithVersion(std::string_view key, std::string_view data,
                                                  uint64_t version) = 0;

  // Conditional, idempotent write (spec Milestone 5). Writes only if the current
  // committed version equals `expected_version` (0 = "must not already exist"),
  // producing version expected_version + 1; otherwise returns kConflict. If
  // `request_id` is non-empty and matches the request that produced the current
  // version, the write is a recognized retry: the existing metadata is returned
  // unchanged (no new version), making client retries safe.
  virtual StatusOr<ObjectMetadata> PutConditional(std::string_view key, std::string_view data,
                                                  uint64_t expected_version,
                                                  std::string_view request_id) = 0;

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
