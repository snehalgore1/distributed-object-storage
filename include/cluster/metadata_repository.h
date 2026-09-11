#ifndef DOS_CLUSTER_METADATA_REPOSITORY_H_
#define DOS_CLUSTER_METADATA_REPOSITORY_H_

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/cluster_map.h"
#include "cluster/metadata_view.h"
#include "cluster/node_info.h"

namespace dos {

// Authoritative control-plane store (spec Milestone 8). Owns cluster membership
// and consistent-hash placement plus the object -> replica-set map. In-memory
// and thread-safe; the interface is the point — a persistent (SQLite/Postgres)
// or Raft-replicated backend can be swapped in without changing callers.
//
// This is the in-process implementation of MetadataView; the Metadata gRPC
// service wraps one of these, and RemoteMetadataView talks to that service.
class MetadataRepository : public MetadataView {
public:
  explicit MetadataRepository(std::size_t virtual_nodes_per_node = 150)
      : cluster_(virtual_nodes_per_node) {}

  // Membership admin (control-plane configuration).
  void AddNode(const NodeInfo& node);
  std::vector<NodeInfo> Nodes() const;

  std::vector<std::string> PlacementFor(std::string_view key, std::size_t rf) override;
  Status RegisterObject(const ObjectLocation& loc) override;
  StatusOr<ObjectLocation> LookupObject(std::string_view key) override;
  Status RemoveObject(std::string_view key) override;
  StatusOr<std::vector<ObjectLocation>> ListObjects(std::string_view prefix) override;

private:
  mutable std::mutex mu_;
  ClusterMap cluster_;
  std::map<std::string, ObjectLocation> objects_; // key -> location
};

} // namespace dos

#endif // DOS_CLUSTER_METADATA_REPOSITORY_H_
