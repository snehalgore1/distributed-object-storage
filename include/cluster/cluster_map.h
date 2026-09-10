#ifndef DOS_CLUSTER_CLUSTER_MAP_H_
#define DOS_CLUSTER_CLUSTER_MAP_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/consistent_hash_ring.h"
#include "cluster/node_info.h"

namespace dos {

// The cluster's view of membership plus object placement. Wraps a node table
// (id -> NodeInfo) and a consistent-hash ring so callers get a single source of
// truth for "which nodes are in the cluster" and "which nodes should hold this
// key" (spec Milestone 3).
//
// Not thread-safe; the owning service serializes access.
class ClusterMap {
public:
  explicit ClusterMap(std::size_t virtual_nodes_per_node = 150);

  // Adds or updates a node. New ids are placed on the ring; updating an existing
  // id refreshes its NodeInfo without disturbing placement.
  void AddOrUpdateNode(const NodeInfo& node);

  // Removes a node from the table and the ring. Returns false if absent.
  bool RemoveNode(std::string_view node_id);

  std::optional<NodeInfo> GetNode(std::string_view node_id) const;
  std::vector<NodeInfo> Nodes() const;
  std::size_t Size() const { return nodes_.size(); }

  // Placement: the replica set of node ids for `key` (primary first), of size
  // min(replication_factor, cluster size).
  std::vector<std::string> PlacementFor(std::string_view key, std::size_t replication_factor) const;

  // Convenience: the primary node id for `key`, or "" if the cluster is empty.
  std::string PrimaryFor(std::string_view key) const { return ring_.LookupPrimary(key); }

  const ConsistentHashRing& ring() const { return ring_; }

private:
  std::vector<NodeInfo> nodes_; // small membership table; linear scan is fine
  ConsistentHashRing ring_;
};

} // namespace dos

#endif // DOS_CLUSTER_CLUSTER_MAP_H_
