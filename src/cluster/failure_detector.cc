#include "cluster/failure_detector.h"

namespace dos {

FailureDetector::FailureDetector(std::size_t miss_threshold)
    : miss_threshold_(miss_threshold == 0 ? 1 : miss_threshold) {}

void FailureDetector::AddNode(std::string_view node_id) {
  std::lock_guard<std::mutex> lock(mu_);
  nodes_.try_emplace(std::string(node_id));
}

void FailureDetector::RecordSuccess(std::string_view node_id) {
  std::lock_guard<std::mutex> lock(mu_);
  Entry& e = nodes_[std::string(node_id)];
  e.misses = 0;
  switch (e.state) {
  case NodeState::kUnavailable:
    // Came back after being declared dead: must be repaired before trusted.
    e.state = NodeState::kRecovering;
    break;
  case NodeState::kRecovering:
    // Stays Recovering until repair completes; a healthy ping alone is not
    // enough to re-trust it.
    break;
  case NodeState::kHealthy:
  case NodeState::kSuspect:
  case NodeState::kJoining:
    e.state = NodeState::kHealthy;
    break;
  }
}

void FailureDetector::RecordFailure(std::string_view node_id) {
  std::lock_guard<std::mutex> lock(mu_);
  Entry& e = nodes_[std::string(node_id)];
  ++e.misses;
  if (e.misses >= miss_threshold_) {
    e.state = NodeState::kUnavailable;
  } else if (e.state == NodeState::kHealthy || e.state == NodeState::kJoining) {
    e.state = NodeState::kSuspect;
  }
  // A failure while Recovering that reaches the threshold flips to Unavailable
  // (handled above); otherwise it remains Recovering.
}

void FailureDetector::RecordRepairComplete(std::string_view node_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = nodes_.find(std::string(node_id));
  if (it != nodes_.end() && it->second.state == NodeState::kRecovering) {
    it->second.state = NodeState::kHealthy;
    it->second.misses = 0;
  }
}

NodeState FailureDetector::GetState(std::string_view node_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = nodes_.find(std::string(node_id));
  return it == nodes_.end() ? NodeState::kJoining : it->second.state;
}

std::size_t FailureDetector::consecutive_misses(std::string_view node_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = nodes_.find(std::string(node_id));
  return it == nodes_.end() ? 0 : it->second.misses;
}

std::map<std::string, NodeState> FailureDetector::Snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::map<std::string, NodeState> out;
  for (const auto& [id, e] : nodes_) {
    out[id] = e.state;
  }
  return out;
}

} // namespace dos
