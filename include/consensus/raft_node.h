#ifndef DOS_CONSENSUS_RAFT_NODE_H_
#define DOS_CONSENSUS_RAFT_NODE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "consensus/raft_storage.h"
#include "consensus/raft_transport.h"
#include "consensus/raft_types.h"
#include "raft.pb.h"

namespace dos {
namespace consensus {

// Timing knobs. Defaults are test-friendly; production overrides with larger
// values. The election timeout is drawn uniformly from [min, max) per attempt
// so split votes resolve (paper §5.2).
struct RaftConfig {
  std::string id;                 // this node's stable id
  std::vector<std::string> peers; // peer ids (excludes self)
  std::chrono::milliseconds election_min{150};
  std::chrono::milliseconds election_max{300};
  std::chrono::milliseconds heartbeat{50};
};

// Applies a committed log entry to the state machine. Called in log order,
// exactly once per entry, from a single thread — so the state machine sees a
// deterministic command sequence identical on every node.
using ApplyFn = std::function<void(const LogEntry&)>;

// A single Raft consensus node (spec Milestone 15): leader election with terms,
// heartbeats/election timeouts, replicated log with majority commit, follower
// catch-up, leader failover, and deterministic application of committed
// entries. Durable state lives in RaftStorage; peer RPCs go through a
// RaftTransport. Thread-safe.
class RaftNode {
public:
  RaftNode(RaftConfig config, RaftTransport* transport, RaftStorage* storage, ApplyFn apply);
  ~RaftNode();

  RaftNode(const RaftNode&) = delete;
  RaftNode& operator=(const RaftNode&) = delete;

  // Starts the background election/heartbeat loop.
  void Start();
  // Stops all background activity and joins threads.
  void Stop();

  // Inbound RPC handlers (invoked by the transport/server on the receiving
  // node). Thread-safe; may mutate durable state before replying.
  void HandleRequestVote(const rpc::RequestVoteRequest& req, rpc::RequestVoteResponse* resp);
  void HandleAppendEntries(const rpc::AppendEntriesRequest& req, rpc::AppendEntriesResponse* resp);

  // Leader-only: append `command` to the log and begin replication. Returns the
  // assigned log index, or kUnavailable with the current leader hint if this
  // node is not the leader.
  StatusOr<uint64_t> Propose(const std::string& command);

  // Blocks until the entry at `index` (proposed in `term`) has been applied to
  // the state machine, or the deadline passes, or this node learns a newer term
  // (meaning the entry may have been overwritten and the proposal lost).
  // Returns Ok on apply, Unavailable otherwise.
  Status WaitApplied(uint64_t index, uint64_t term, std::chrono::milliseconds timeout);

  // Observability.
  Role role() const;
  bool is_leader() const { return role() == Role::kLeader; }
  uint64_t current_term() const;
  std::string leader_id() const;
  uint64_t commit_index() const;
  uint64_t last_log_index() const;

private:
  // --- helpers, all assume mu_ is held unless noted ---
  uint64_t LastLogIndexLocked() const;
  uint64_t LastLogTermLocked() const;
  uint64_t TermAtLocked(uint64_t index) const; // 0 for index 0 / out of range
  void PersistHardStateLocked();
  void StepDownIfStaleLocked(uint64_t term); // if term > currentTerm, become follower
  void BecomeLeaderLocked();                 // won election: init leader state
  void AdvanceCommitLocked();                // leader: recompute commitIndex by majority
  void ApplyCommittedLocked();               // apply lastApplied+1..commitIndex
  int MajorityLocked() const;                // votes/replicas needed to commit
  std::chrono::milliseconds RandomElectionTimeout();

  // Background actions (acquire/release mu_ internally; send RPCs unlocked).
  void ElectionLoop();                           // drives follower/candidate timeouts
  void ReplicationLoop(const std::string& peer); // leader: keep one peer current
  void StartElection();                          // pre-vote round, then a real election
  // Fans a RequestVote (pre-vote or real) out to all peers and returns whether a
  // majority granted. Adopts a higher term seen in any reply (stepping down).
  bool WinVoteRound(const rpc::RequestVoteRequest& req, bool pre_vote, uint64_t term);
  void ReplicateTo(const std::string& peer, uint64_t term); // one AppendEntries round

  RaftConfig config_;
  RaftTransport* transport_;
  RaftStorage* storage_;
  ApplyFn apply_;

  mutable std::mutex mu_;
  std::condition_variable cv_; // wakes RunLoop and WaitApplied waiters

  // Persistent (mirrored to storage_).
  uint64_t current_term_ = 0;
  std::string voted_for_;
  std::vector<LogEntry> log_; // log_[i].index == i+1

  // Volatile.
  Role role_ = Role::kFollower;
  std::string leader_id_;
  uint64_t commit_index_ = 0;
  uint64_t last_applied_ = 0;
  std::chrono::steady_clock::time_point last_activity_;       // resets the election timer
  std::chrono::steady_clock::time_point last_leader_contact_; // last valid AppendEntries
  std::chrono::milliseconds election_timeout_{0};

  // Leader volatile, per peer.
  std::unordered_map<std::string, uint64_t> next_index_;
  std::unordered_map<std::string, uint64_t> match_index_;

  std::mt19937 rng_;
  std::atomic<bool> running_{false};
  std::thread election_thread_;
  std::vector<std::thread> peer_threads_; // one replication loop per peer
};

} // namespace consensus
} // namespace dos

#endif // DOS_CONSENSUS_RAFT_NODE_H_
