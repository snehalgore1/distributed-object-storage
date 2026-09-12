// Raft acceptance tests (spec Milestone 15). A three-node metadata group runs
// over the in-memory transport with real background threads (elections,
// heartbeats, replication) so timing is exercised but deterministic — failures
// are injected by marking a node down, not by racing the network.
//
// Covers the spec's acceptance scenario: A=leader, kill A, B or C becomes
// leader, metadata writes continue after the election, a recovered node catches
// up, and no committed mutation is lost. Also verifies crash recovery from the
// on-disk log.

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "consensus/in_memory_transport.h"
#include "consensus/raft_node.h"
#include "consensus/raft_storage.h"

namespace dos::consensus {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Records the commands a node's state machine has applied, in order.
struct AppliedLog {
  mutable std::mutex mu;
  std::vector<std::string> commands;

  std::vector<std::string> snapshot() const {
    std::lock_guard<std::mutex> lock(mu);
    return commands;
  }
};

// A small managed Raft cluster over one InMemoryCluster.
class RaftCluster {
public:
  RaftCluster(std::vector<std::string> ids, fs::path root)
      : ids_(std::move(ids)), root_(std::move(root)) {
    fs::remove_all(root_);
    for (const std::string& id : ids_) {
      applied_[id] = std::make_shared<AppliedLog>();
      transports_[id] = wire_.EndpointFor(id);
      Build(id);
    }
  }

  ~RaftCluster() {
    for (auto& [id, node] : nodes_) {
      if (node) {
        node->Stop();
      }
    }
  }

  void StartAll() {
    for (auto& [id, node] : nodes_) {
      node->Start();
    }
  }

  RaftConfig Config(const std::string& id) const {
    RaftConfig cfg;
    cfg.id = id;
    for (const std::string& other : ids_) {
      if (other != id) {
        cfg.peers.push_back(other);
      }
    }
    // Heartbeat well under the election window (~6-12x) so a leader's beats beat
    // followers' timeouts even under the CPU/scheduling jitter of a parallel
    // `ctest` run. Tighter margins here caused spurious elections under load.
    cfg.election_min = 300ms;
    cfg.election_max = 600ms;
    cfg.heartbeat = 50ms;
    return cfg;
  }

  // Builds (or rebuilds) a node from its on-disk state.
  void Build(const std::string& id) {
    auto storage = RaftStorage::Open(root_ / id);
    storages_[id] = std::move(storage).value();
    auto log = applied_[id];
    ApplyFn apply = [log](const LogEntry& e) {
      std::lock_guard<std::mutex> lock(log->mu);
      log->commands.push_back(e.command);
    };
    auto node = std::make_unique<RaftNode>(Config(id), transports_[id].get(),
                                           storages_[id].get(), apply);
    wire_.Register(id, node.get());
    nodes_[id] = std::move(node);
  }

  // Simulates a process crash: stops the node's threads and drops in-memory
  // state, leaving only the persisted log/hard-state on disk. A crashed node
  // (unlike a partitioned one) does not keep incrementing its term, so on
  // Restart it rejoins behind the current leader and simply catches up.
  void CrashStop(const std::string& id) {
    wire_.SetDown(id, true);
    nodes_[id]->Stop();
    nodes_[id].reset();
    storages_[id].reset();
    applied_[id] = std::make_shared<AppliedLog>(); // state machine rebuilds from log
  }

  // Restarts a previously CrashStop'd node purely from its on-disk state.
  void Restart(const std::string& id) {
    Build(id);
    wire_.SetDown(id, false);
    nodes_[id]->Start();
  }

  void CrashRestart(const std::string& id) {
    CrashStop(id);
    Restart(id);
  }

  void SetDown(const std::string& id, bool down) { wire_.SetDown(id, down); }

  RaftNode* node(const std::string& id) { return nodes_[id].get(); }
  std::shared_ptr<AppliedLog> applied(const std::string& id) { return applied_[id]; }

  // Returns the id of a current leader among reachable nodes, or "" if none is
  // found within the timeout.
  std::string WaitForLeader(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      for (const std::string& id : ids_) {
        if (!wire_.IsDown(id) && nodes_[id] && nodes_[id]->is_leader()) {
          return id;
        }
      }
      std::this_thread::sleep_for(10ms);
    }
    return "";
  }

  // Proposes on the given leader and waits for the entry to be applied there.
  Status ProposeAndWait(const std::string& leader, const std::string& command,
                        std::chrono::milliseconds timeout) {
    RaftNode* n = nodes_[leader].get();
    const uint64_t term = n->current_term();
    auto idx = n->Propose(command);
    if (!idx.ok()) {
      return idx.status();
    }
    return n->WaitApplied(idx.value(), term, timeout);
  }

  // Commits a command against whichever node is currently leader, retrying
  // across leadership changes until `deadline`. This tolerates the elections a
  // live cluster undergoes (and that a killed leader forces) rather than
  // assuming a single stable leader for the whole test.
  bool CommitOnLeader(const std::string& command, const std::string& excluded,
                      std::chrono::milliseconds deadline_span) {
    const auto deadline = std::chrono::steady_clock::now() + deadline_span;
    while (std::chrono::steady_clock::now() < deadline) {
      for (const std::string& id : ids_) {
        if (id == excluded || nodes_[id] == nullptr || !nodes_[id]->is_leader()) {
          continue;
        }
        if (ProposeAndWait(id, command, 1000ms).ok()) {
          return true;
        }
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  // Waits until every reachable node's applied sequence equals `expected`.
  bool WaitConverged(const std::vector<std::string>& expected, const std::string& excluded,
                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bool all = true;
      for (const std::string& id : ids_) {
        if (id == excluded) {
          continue;
        }
        if (applied_[id]->snapshot() != expected) {
          all = false;
          break;
        }
      }
      if (all) {
        return true;
      }
      std::this_thread::sleep_for(20ms);
    }
    return false;
  }

  const std::vector<std::string>& ids() const { return ids_; }

private:
  std::vector<std::string> ids_;
  fs::path root_;
  InMemoryCluster wire_;
  std::map<std::string, std::unique_ptr<RaftTransport>> transports_;
  std::map<std::string, std::unique_ptr<RaftStorage>> storages_;
  std::map<std::string, std::unique_ptr<RaftNode>> nodes_;
  std::map<std::string, std::shared_ptr<AppliedLog>> applied_;
};

fs::path TempRoot() {
  static std::atomic<int> counter{0};
  return fs::temp_directory_path() /
         ("dos_raft_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)));
}

TEST(RaftClusterTest, ElectsExactlyOneLeader) {
  const fs::path root = TempRoot();
  RaftCluster cluster({"A", "B", "C"}, root);
  cluster.StartAll();

  const std::string leader = cluster.WaitForLeader(3000ms);
  ASSERT_FALSE(leader.empty()) << "no leader elected";

  // At most one node believes it is leader in the settled state.
  std::this_thread::sleep_for(200ms);
  int leaders = 0;
  for (const std::string& id : cluster.ids()) {
    if (cluster.node(id)->is_leader()) {
      ++leaders;
    }
  }
  EXPECT_EQ(leaders, 1);
  fs::remove_all(root);
}

TEST(RaftClusterTest, ReplicatesCommittedEntriesToAllNodes) {
  const fs::path root = TempRoot();
  RaftCluster cluster({"A", "B", "C"}, root);
  cluster.StartAll();
  ASSERT_FALSE(cluster.WaitForLeader(5000ms).empty());

  std::vector<std::string> expected;
  for (int i = 1; i <= 5; ++i) {
    const std::string cmd = "cmd-" + std::to_string(i);
    ASSERT_TRUE(cluster.CommitOnLeader(cmd, /*excluded=*/"", 5000ms)) << "could not commit " << cmd;
    expected.push_back(cmd);
  }

  // Every node converges on exactly the committed sequence.
  EXPECT_TRUE(cluster.WaitConverged(expected, /*excluded=*/"", 5000ms));
  fs::remove_all(root);
}

// The spec's Milestone 15 acceptance test.
TEST(RaftClusterTest, LeaderFailoverPreservesCommittedWritesAndCatchesUp) {
  const fs::path root = TempRoot();
  RaftCluster cluster({"A", "B", "C"}, root);
  cluster.StartAll();

  const std::string leader1 = cluster.WaitForLeader(5000ms);
  ASSERT_FALSE(leader1.empty());

  // Commit some metadata writes under the first leader.
  std::vector<std::string> committed;
  for (int i = 1; i <= 3; ++i) {
    const std::string cmd = "before-" + std::to_string(i);
    ASSERT_TRUE(cluster.CommitOnLeader(cmd, /*excluded=*/"", 5000ms)) << "could not commit " << cmd;
    committed.push_back(cmd);
  }

  // Kill the leader (process crash). A survivor must win the next election.
  cluster.CrashStop(leader1);
  std::string leader2;
  const auto deadline = std::chrono::steady_clock::now() + 5000ms;
  while (std::chrono::steady_clock::now() < deadline && leader2.empty()) {
    for (const std::string& id : cluster.ids()) {
      if (id != leader1 && cluster.node(id) != nullptr && cluster.node(id)->is_leader()) {
        leader2 = id;
      }
    }
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_FALSE(leader2.empty()) << "no new leader after killing " << leader1;
  EXPECT_NE(leader2, leader1);

  // Writes continue after the election (against whichever survivor leads).
  for (int i = 1; i <= 3; ++i) {
    const std::string cmd = "after-" + std::to_string(i);
    ASSERT_TRUE(cluster.CommitOnLeader(cmd, /*excluded=*/leader1, 5000ms))
        << "writes did not continue after failover for " << cmd;
    committed.push_back(cmd);
  }

  // The killed node restarts from disk and must catch up to the full committed
  // history. No committed mutation is lost: every node converges on exactly the
  // committed sequence, in order.
  cluster.Restart(leader1);
  EXPECT_TRUE(cluster.WaitConverged(committed, /*excluded=*/"", 5000ms))
      << "a node lost/reordered committed data or did not catch up";
  fs::remove_all(root);
}

TEST(RaftClusterTest, CrashedNodeRecoversFromDiskAndCatchesUp) {
  const fs::path root = TempRoot();
  RaftCluster cluster({"A", "B", "C"}, root);
  cluster.StartAll();
  const std::string leader = cluster.WaitForLeader(5000ms);
  ASSERT_FALSE(leader.empty());

  std::vector<std::string> committed;
  for (int i = 1; i <= 4; ++i) {
    const std::string cmd = "v" + std::to_string(i);
    ASSERT_TRUE(cluster.CommitOnLeader(cmd, /*excluded=*/"", 5000ms)) << "could not commit " << cmd;
    committed.push_back(cmd);
  }

  // Crash and restart a follower; it must rebuild state purely from disk + the
  // leader's catch-up, ending with the full committed sequence.
  std::string follower;
  for (const std::string& id : cluster.ids()) {
    if (id != leader) {
      follower = id;
      break;
    }
  }
  cluster.CrashRestart(follower);

  EXPECT_TRUE(cluster.WaitConverged(committed, /*excluded=*/"", 5000ms))
      << "recovered node did not rebuild the committed sequence";
  fs::remove_all(root);
}

} // namespace
} // namespace dos::consensus
