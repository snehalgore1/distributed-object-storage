#include "cluster/cluster_map.h"

#include <algorithm>

namespace dos {

const char* NodeStateName(NodeState state) {
  switch (state) {
  case NodeState::kJoining:
    return "JOINING";
  case NodeState::kHealthy:
    return "HEALTHY";
  case NodeState::kSuspect:
    return "SUSPECT";
  case NodeState::kUnavailable:
    return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

ClusterMap::ClusterMap(std::size_t virtual_nodes_per_node) : ring_(virtual_nodes_per_node) {}

void ClusterMap::AddOrUpdateNode(const NodeInfo& node) {
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&](const NodeInfo& n) { return n.id == node.id; });
  if (it != nodes_.end()) {
    *it = node; // update metadata; ring placement is unchanged for a known id
    return;
  }
  nodes_.push_back(node);
  ring_.AddNode(node.id);
}

bool ClusterMap::RemoveNode(std::string_view node_id) {
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&](const NodeInfo& n) { return n.id == node_id; });
  if (it == nodes_.end()) {
    return false;
  }
  nodes_.erase(it);
  ring_.RemoveNode(node_id);
  return true;
}

std::optional<NodeInfo> ClusterMap::GetNode(std::string_view node_id) const {
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&](const NodeInfo& n) { return n.id == node_id; });
  if (it == nodes_.end()) {
    return std::nullopt;
  }
  return *it;
}

std::vector<NodeInfo> ClusterMap::Nodes() const { return nodes_; }

std::vector<std::string> ClusterMap::PlacementFor(std::string_view key,
                                                  std::size_t replication_factor) const {
  return ring_.LookupReplicas(key, replication_factor);
}

} // namespace dos
