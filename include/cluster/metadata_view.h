#ifndef DOS_CLUSTER_METADATA_VIEW_H_
#define DOS_CLUSTER_METADATA_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace dos {

// Cluster-level (control-plane) record: an object's authoritative version and
// checksum, and which nodes hold it. Never carries the object payload.
struct ObjectLocation {
  std::string key;
  uint64_t version = 0;
  std::string checksum;
  uint64_t size = 0;
  bool deleted = false;
  std::vector<std::string> replicas; // node ids currently holding this version
};

// The control plane as seen by the coordinator and repairer. Backed either by
// an in-process repository (LocalMetadataView / MetadataRepository) or by the
// remote Metadata gRPC service (RemoteMetadataView). Keeping this an interface
// is what lets placement + object metadata live "behind the metadata service"
// (spec Milestone 8) without the coordinator caring where it runs.
class MetadataView {
public:
  virtual ~MetadataView() = default;

  // Replica set for a key (primary first), size min(rf, cluster size).
  virtual std::vector<std::string> PlacementFor(std::string_view key, std::size_t rf) = 0;

  // Record where an object version lives after a successful quorum write.
  virtual Status RegisterObject(const ObjectLocation& loc) = 0;

  // Authoritative metadata for a key; kNotFound if absent or tombstoned.
  virtual StatusOr<ObjectLocation> LookupObject(std::string_view key) = 0;

  // Tombstone the object in the control plane.
  virtual Status RemoveObject(std::string_view key) = 0;

  // All live object locations under a prefix (for repair).
  virtual StatusOr<std::vector<ObjectLocation>> ListObjects(std::string_view prefix) = 0;
};

} // namespace dos

#endif // DOS_CLUSTER_METADATA_VIEW_H_
