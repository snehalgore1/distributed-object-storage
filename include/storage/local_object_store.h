#ifndef DOS_STORAGE_LOCAL_OBJECT_STORE_H_
#define DOS_STORAGE_LOCAL_OBJECT_STORE_H_

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/metadata_store.h"
#include "storage/object_store.h"

namespace dos {

// Filesystem-backed single-node object store.
//
// Layout under `root`:
//   root/data/<aa>/<bb>/<key-digest>   object payloads
//   root/data/tmp/                     staging area for atomic writes
//   root/metadata/objects.sqlite       committed metadata (source of truth)
//
// Durability contract (see docs/storage-engine.md): payload bytes are staged in
// a temp file, fsync'd, atomically renamed into place, and the parent directory
// is fsync'd before the metadata row is committed. A crash before the metadata
// commit leaves an orphan file that is never observable as a committed object.
class LocalObjectStore : public ObjectStore {
public:
  static StatusOr<std::unique_ptr<LocalObjectStore>> Open(const std::filesystem::path& root);

  StatusOr<ObjectMetadata> Put(std::string_view key, std::string_view data) override;
  StatusOr<std::string> Get(std::string_view key) override;
  StatusOr<ObjectMetadata> Head(std::string_view key) override;
  Status Delete(std::string_view key) override;
  StatusOr<std::vector<ObjectMetadata>> List(std::string_view prefix) override;

private:
  LocalObjectStore(std::filesystem::path root, std::unique_ptr<MetadataStore> metadata)
      : root_(std::move(root)), metadata_(std::move(metadata)) {}

  std::filesystem::path DataRoot() const { return root_ / "data"; }
  std::filesystem::path TmpDir() const { return root_ / "data" / "tmp"; }

  // Striped locking (spec Milestone 2): rather than one global mutex around the
  // whole store, keys are hashed to a fixed set of shards. Operations on keys in
  // different shards proceed concurrently; Put/Delete take the shard exclusively
  // while Get/Head share it. This also serializes the read-version/write-version
  // sequence in Put so concurrent writers to the same key can't collide.
  static constexpr std::size_t kNumShards = 64;
  std::shared_mutex& ShardFor(std::string_view key) {
    return shards_[std::hash<std::string_view>{}(key) % kNumShards];
  }

  std::filesystem::path root_;
  std::unique_ptr<MetadataStore> metadata_;
  std::array<std::shared_mutex, kNumShards> shards_;
};

} // namespace dos

#endif // DOS_STORAGE_LOCAL_OBJECT_STORE_H_
