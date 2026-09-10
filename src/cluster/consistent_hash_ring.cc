#include "cluster/consistent_hash_ring.h"

#include <algorithm>

#include "common/hash.h"

namespace dos {

ConsistentHashRing::ConsistentHashRing(std::size_t virtual_nodes_per_node)
    : vnodes_(virtual_nodes_per_node == 0 ? 1 : virtual_nodes_per_node) {}

bool ConsistentHashRing::Contains(std::string_view node_id) const {
  return std::find(physical_nodes_.begin(), physical_nodes_.end(), node_id) !=
         physical_nodes_.end();
}

void ConsistentHashRing::AddNode(std::string_view node_id) {
  if (Contains(node_id)) {
    return;
  }
  const std::string id(node_id);
  physical_nodes_.push_back(id);
  for (std::size_t i = 0; i < vnodes_; ++i) {
    const uint64_t pos = Hash64(id + "#" + std::to_string(i));
    // On the rare hash collision, keep the existing owner; the lost vnode only
    // marginally affects balance and never breaks correctness.
    ring_.emplace(pos, id);
  }
}

void ConsistentHashRing::RemoveNode(std::string_view node_id) {
  auto it = std::find(physical_nodes_.begin(), physical_nodes_.end(), node_id);
  if (it == physical_nodes_.end()) {
    return;
  }
  physical_nodes_.erase(it);
  for (auto rit = ring_.begin(); rit != ring_.end();) {
    if (rit->second == node_id) {
      rit = ring_.erase(rit);
    } else {
      ++rit;
    }
  }
}

std::string ConsistentHashRing::LookupPrimary(std::string_view key) const {
  if (ring_.empty()) {
    return "";
  }
  const uint64_t pos = Hash64(key);
  auto it = ring_.lower_bound(pos);
  if (it == ring_.end()) {
    it = ring_.begin(); // wrap around the ring
  }
  return it->second;
}

std::vector<std::string> ConsistentHashRing::LookupReplicas(std::string_view key,
                                                            std::size_t count) const {
  std::vector<std::string> result;
  if (ring_.empty() || count == 0) {
    return result;
  }
  const std::size_t max_distinct = std::min(count, physical_nodes_.size());

  const uint64_t pos = Hash64(key);
  auto it = ring_.lower_bound(pos);
  if (it == ring_.end()) {
    it = ring_.begin();
  }

  // Walk clockwise, collecting distinct physical nodes until we have enough or
  // have visited every vnode once.
  const std::size_t steps = ring_.size();
  for (std::size_t visited = 0; visited < steps && result.size() < max_distinct; ++visited) {
    const std::string& node = it->second;
    if (std::find(result.begin(), result.end(), node) == result.end()) {
      result.push_back(node);
    }
    if (++it == ring_.end()) {
      it = ring_.begin();
    }
  }
  return result;
}

} // namespace dos
