#ifndef DOS_STORAGE_METADATA_STORE_H_
#define DOS_STORAGE_METADATA_STORE_H_

#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/object_metadata.h"

namespace dos {

// Persistence boundary for object metadata. Kept behind an interface so the
// control/data-plane split (spec Milestone 8) can swap the backend
// (SQLite -> PostgreSQL / metadata service) without touching the object store.
class MetadataStore {
public:
  virtual ~MetadataStore() = default;

  // Inserts or replaces the metadata row for a key.
  virtual Status Put(const ObjectMetadata& meta) = 0;

  // Returns the current (non-tombstoned) metadata, or kNotFound. A tombstoned
  // key is reported as kNotFound.
  virtual StatusOr<ObjectMetadata> Get(std::string_view key) = 0;

  // Returns the current version for `key` including tombstones, or 0 if the key
  // was never written. Used to assign the next monotonic version.
  virtual StatusOr<uint64_t> CurrentVersion(std::string_view key) = 0;

  // Marks a key deleted (tombstone). Returns kNotFound if the key is absent or
  // already tombstoned.
  virtual Status Delete(std::string_view key) = 0;

  // Lists current (non-tombstoned) objects whose key starts with `prefix`.
  virtual StatusOr<std::vector<ObjectMetadata>> List(std::string_view prefix) = 0;
};

} // namespace dos

#endif // DOS_STORAGE_METADATA_STORE_H_
