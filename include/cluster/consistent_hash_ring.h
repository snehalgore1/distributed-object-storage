#ifndef DOS_CLUSTER_CONSISTENT_HASH_RING_H_
#define DOS_CLUSTER_CONSISTENT_HASH_RING_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dos {

// A 64-bit consistent-hash ring with virtual nodes (spec Milestone 3).
//
// Each physical node is placed at `vnodes` positions on the ring (hash of
// "<node_id>#<i>"). A key maps to the first node clockwise from hash(key).
// Virtual nodes smooth out distribution and bound the fraction of keys that
// move when a node is added or removed to roughly 1/N of the keyspace, versus
// ~all keys under modulo hashing.
//
// This type is not thread-safe; guard it externally if shared across threads.
class ConsistentHashRing {
public:
  explicit ConsistentHashRing(std::size_t virtual_nodes_per_node = 150);

  // Adds a physical node with the configured number of virtual nodes. Adding a
  // node that already exists is a no-op.
  void AddNode(std::string_view node_id);

  // Removes a physical node and all of its virtual nodes. Removing an absent
  // node is a no-op.
  void RemoveNode(std::string_view node_id);

  bool Contains(std::string_view node_id) const;
  std::size_t NumPhysicalNodes() const { return physical_nodes_.size(); }
  std::size_t NumVirtualNodes() const { return ring_.size(); }
  bool empty() const { return ring_.empty(); }

  // Returns the primary node for `key`, or "" if the ring is empty.
  std::string LookupPrimary(std::string_view key) const;

  // Returns up to `count` distinct physical nodes for `key`, walking clockwise:
  // the primary followed by successive distinct nodes. Fewer than `count` are
  // returned only if the ring has fewer physical nodes.
  std::vector<std::string> LookupReplicas(std::string_view key, std::size_t count) const;

private:
  std::size_t vnodes_;
  std::map<uint64_t, std::string> ring_; // ring position -> physical node id
  std::vector<std::string> physical_nodes_;
};

} // namespace dos

#endif // DOS_CLUSTER_CONSISTENT_HASH_RING_H_
