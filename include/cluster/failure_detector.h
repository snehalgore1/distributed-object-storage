#ifndef DOS_CLUSTER_FAILURE_DETECTOR_H_
#define DOS_CLUSTER_FAILURE_DETECTOR_H_

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "cluster/node_info.h"

namespace dos {

// Tracks per-node liveness via a heartbeat state machine (spec Milestone 6):
//
//   Healthy --miss--> Suspect --miss(threshold)--> Unavailable
//      ^                 |                              |
//      |               success                       success
//      |                 v                              v
//      +-------------- Healthy                     Recovering --RepairDone--> Healthy
//
// A miss threshold prevents a single transient timeout from declaring a node
// dead. A node that returns after being Unavailable enters Recovering (it must
// be repaired before it is trusted again) rather than jumping straight to
// Healthy. Liveness is decided by the caller feeding heartbeat outcomes via
// RecordSuccess/RecordFailure; this keeps the FSM deterministic and testable,
// independent of timers and sockets.
class FailureDetector {
public:
  explicit FailureDetector(std::size_t miss_threshold = 3);

  // Register a node as known and Healthy (idempotent).
  void AddNode(std::string_view node_id);

  void RecordSuccess(std::string_view node_id);
  void RecordFailure(std::string_view node_id);

  // Called by the repair path when a Recovering node has been reconciled.
  void RecordRepairComplete(std::string_view node_id);

  NodeState GetState(std::string_view node_id) const;
  std::size_t consecutive_misses(std::string_view node_id) const;

  std::map<std::string, NodeState> Snapshot() const;

private:
  struct Entry {
    NodeState state = NodeState::kHealthy;
    std::size_t misses = 0;
  };

  const std::size_t miss_threshold_;
  mutable std::mutex mu_;
  std::map<std::string, Entry> nodes_;
};

} // namespace dos

#endif // DOS_CLUSTER_FAILURE_DETECTOR_H_
