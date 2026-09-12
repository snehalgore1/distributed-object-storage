#ifndef DOS_CONSENSUS_RAFT_TYPES_H_
#define DOS_CONSENSUS_RAFT_TYPES_H_

#include <cstdint>
#include <string>

namespace dos {
namespace consensus {

// A Raft node is always exactly one of these (spec Milestone 15).
enum class Role : uint8_t { kFollower, kCandidate, kLeader };

const char* RoleName(Role role);

// One replicated log entry. `command` is opaque to consensus — a serialized
// MetadataCommand that only the state machine decodes and applies.
struct LogEntry {
  uint64_t term = 0;
  uint64_t index = 0; // 1-based position in the log
  std::string command;
};

// The persistent state Raft must survive a crash with (paper §5): the current
// term and the candidate this node voted for in that term. Losing these can
// violate election safety, so they are fsync'd before any RPC reply depends
// on them.
struct HardState {
  uint64_t current_term = 0;
  std::string voted_for; // node id, empty = none
};

} // namespace consensus
} // namespace dos

#endif // DOS_CONSENSUS_RAFT_TYPES_H_
