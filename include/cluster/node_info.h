#ifndef DOS_CLUSTER_NODE_INFO_H_
#define DOS_CLUSTER_NODE_INFO_H_

#include <cstdint>
#include <string>

namespace dos {

// Liveness state of a node. Milestone 3 only distinguishes healthy vs not; the
// full Healthy -> Suspect -> Unavailable -> Recovering state machine and the
// heartbeat logic that drives it arrive in Milestone 6.
enum class NodeState {
  kJoining,
  kHealthy,
  kSuspect,
  kUnavailable,
  kRecovering, // rejoined after failure; being repaired before it is trusted
};

const char* NodeStateName(NodeState state);

// Membership record for one physical storage node.
struct NodeInfo {
  std::string id;      // stable logical id, e.g. "node-a"
  std::string address; // host:port for RPC (used from M3+ transport)
  NodeState state = NodeState::kHealthy;
  uint64_t capacity_bytes = 0;   // advertised capacity (0 = unspecified)
  int64_t last_heartbeat_ms = 0; // unix epoch millis of last heartbeat
};

} // namespace dos

#endif // DOS_CLUSTER_NODE_INFO_H_
