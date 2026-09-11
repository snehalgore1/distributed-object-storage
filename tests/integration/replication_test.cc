// End-to-end replication tests over real in-process gRPC storage nodes.
// Spins up three StorageNode servers on loopback, wires a Coordinator to them,
// and exercises the quorum write / fallback read guarantees (spec Milestone 4).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cluster/cluster_map.h"
#include "common/digest.h"
#include "network/coordinator.h"
#include "network/repairer.h"
#include "network/storage_node_client.h"
#include "network/storage_node_server.h"
#include "storage/local_object_store.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

struct Node {
  std::string id;
  fs::path data_dir;
  std::unique_ptr<LocalObjectStore> store;
  std::unique_ptr<StorageNodeServer> server;
  std::string address;
};

class ReplicationTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_repl_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(root_);

    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    for (const auto& id : ids) {
      auto node = std::make_unique<Node>();
      node->id = id;
      node->data_dir = root_ / id;
      auto s = LocalObjectStore::Open(node->data_dir);
      ASSERT_TRUE(s.ok()) << s.status().ToString();
      node->store = std::move(s).value();
      node->server = std::make_unique<StorageNodeServer>(*node->store);
      ASSERT_TRUE(node->server->Start("127.0.0.1:0"));
      node->address = "127.0.0.1:" + std::to_string(node->server->bound_port());

      NodeInfo info;
      info.id = id;
      info.address = node->address;
      cluster_.AddOrUpdateNode(info);
      clients_[id] =
          std::make_shared<StorageNodeClient>(node->address, std::chrono::milliseconds(300));
      nodes_.push_back(std::move(node));
    }

    Coordinator::Options opts; // RF=3, W=2
    coordinator_ = std::make_unique<Coordinator>(cluster_, clients_, opts);
  }

  void TearDown() override {
    for (auto& n : nodes_) {
      if (n->server)
        n->server->Shutdown();
    }
    coordinator_.reset();
    nodes_.clear();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  Node* NodeById(const std::string& id) {
    for (auto& n : nodes_) {
      if (n->id == id)
        return n.get();
    }
    return nullptr;
  }

  // Overwrites the on-disk object file for `key` on a specific node, simulating
  // silent corruption. The node's stored checksum still reflects the good data.
  void CorruptObjectOnNode(const std::string& node_id, const std::string& key) {
    Node* n = NodeById(node_id);
    ASSERT_NE(n, nullptr);
    const fs::path path = PhysicalPath(n->data_dir / "data", KeyDigest(key));
    ASSERT_TRUE(fs::exists(path)) << path;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "corrupted-bytes";
  }

  // Stops a node's server (keeping its on-disk store) to simulate a crash.
  void StopNode(const std::string& node_id) {
    Node* n = NodeById(node_id);
    ASSERT_NE(n, nullptr);
    n->server->Shutdown();
    n->server.reset();
  }

  // Restarts a previously stopped node on its original address so existing
  // clients reconnect transparently.
  void RestartNode(const std::string& node_id) {
    Node* n = NodeById(node_id);
    ASSERT_NE(n, nullptr);
    n->server = std::make_unique<StorageNodeServer>(*n->store);
    ASSERT_TRUE(n->server->Start(n->address)) << "rebind " << n->address;
  }

  // Polls a node's Health RPC until it responds or the timeout elapses. Needed
  // after a restart because the client channel reconnects with backoff.
  bool WaitForNodeReady(const std::string& node_id, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (clients_[node_id]->Health().ok()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  fs::path root_;
  std::vector<std::unique_ptr<Node>> nodes_;
  ClusterMap cluster_{150};
  std::map<std::string, std::shared_ptr<StorageNodeClient>> clients_;
  std::unique_ptr<Coordinator> coordinator_;
};

TEST_F(ReplicationTest, QuorumWriteThenReadReplicatesToAllNodes) {
  auto put = coordinator_->Put("photos/cat.jpg", "meow-bytes");
  ASSERT_TRUE(put.ok()) << put.status().ToString();
  EXPECT_EQ(put.value().version, 1u);

  auto got = coordinator_->Get("photos/cat.jpg");
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ(got.value(), "meow-bytes");

  // With RF=3 across a 3-node cluster, every node holds the object.
  int holders = 0;
  for (auto& n : nodes_) {
    if (n->store->Head("photos/cat.jpg").ok())
      ++holders;
  }
  EXPECT_EQ(holders, 3);
}

TEST_F(ReplicationTest, WriteSucceedsWithOneReplicaDown) {
  // Take one node offline; W=2 is still reachable via the other two.
  nodes_.back()->server->Shutdown();

  auto put = coordinator_->Put("k1", "v1");
  ASSERT_TRUE(put.ok()) << put.status().ToString();

  auto got = coordinator_->Get("k1");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), "v1");

  // The downed replica is recorded as lagging (a repair candidate for M6).
  auto health = coordinator_->ReplicaHealthSnapshot();
  int lagging = 0;
  for (const auto& [id, h] : health) {
    if (h == ReplicaHealth::kLagging)
      ++lagging;
  }
  EXPECT_GE(lagging, 1);
}

TEST_F(ReplicationTest, WriteFailsQuorumWithTwoReplicasDown) {
  nodes_[1]->server->Shutdown();
  nodes_[2]->server->Shutdown();

  auto put = coordinator_->Put("k2", "v2");
  ASSERT_FALSE(put.ok());
  EXPECT_EQ(put.status().code(), StatusCode::kUnavailable);
}

TEST_F(ReplicationTest, ReadFallsBackOnChecksumMismatch) {
  ASSERT_TRUE(coordinator_->Put("doc", "trusted-content").ok());

  // Corrupt the copy on whichever node is primary for this key.
  ClusterMap probe(150);
  // Rebuild placement identically to find the primary id.
  for (auto& n : nodes_) {
    NodeInfo info;
    info.id = n->id;
    info.address = n->address;
    probe.AddOrUpdateNode(info);
  }
  const std::string primary = probe.PrimaryFor("doc");
  ASSERT_FALSE(primary.empty());
  CorruptObjectOnNode(primary, "doc");

  // The primary now fails its integrity check; the coordinator falls back to a
  // healthy replica and still returns the correct bytes.
  auto got = coordinator_->Get("doc");
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ(got.value(), "trusted-content");
}

TEST_F(ReplicationTest, ConditionalWriteEnforcesVersionAcrossCluster) {
  // Create-only (expected=0) succeeds and replicates.
  auto v1 = coordinator_->PutConditional("cfg", "one", /*expected=*/0, "req-a");
  ASSERT_TRUE(v1.ok()) << v1.status().ToString();
  EXPECT_EQ(v1.value().version, 1u);

  // A stale writer expecting version 0 is rejected cluster-wide.
  auto stale = coordinator_->PutConditional("cfg", "two", /*expected=*/0, "req-b");
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.status().code(), StatusCode::kConflict);

  // Correct expectation advances the value.
  auto v2 = coordinator_->PutConditional("cfg", "two", /*expected=*/1, "req-c");
  ASSERT_TRUE(v2.ok());
  EXPECT_EQ(v2.value().version, 2u);
  EXPECT_EQ(coordinator_->Get("cfg").value(), "two");
}

TEST_F(ReplicationTest, IdempotentRetryThroughCoordinatorDoesNotBumpVersion) {
  auto first = coordinator_->PutConditional("k", "payload", /*expected=*/0, "idem-1");
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(first.value().version, 1u);

  // Same request replayed (e.g. client timed out and retried): still version 1.
  auto retry = coordinator_->PutConditional("k", "payload", /*expected=*/0, "idem-1");
  ASSERT_TRUE(retry.ok()) << retry.status().ToString();
  EXPECT_EQ(retry.value().version, 1u);

  // Every replica still holds exactly version 1.
  for (auto& n : nodes_) {
    auto head = n->store->Head("k");
    ASSERT_TRUE(head.ok());
    EXPECT_EQ(head.value().version, 1u);
  }
}

// A node misses writes while down, then rejoins and is repaired back to full
// redundancy from healthy peers.
TEST_F(ReplicationTest, RepairRestoresRedundancyAfterNodeRejoin) {
  // Objects written while all three nodes are up.
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(coordinator_->Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
  }

  // node-c goes down; more writes land on the surviving quorum only.
  StopNode("node-c");
  for (int i = 5; i < 10; ++i) {
    auto put = coordinator_->Put("k" + std::to_string(i), "v" + std::to_string(i));
    ASSERT_TRUE(put.ok()) << put.status().ToString();
  }

  // node-c rejoins: it still has k0..k4 but is missing k5..k9.
  RestartNode("node-c");
  ASSERT_TRUE(WaitForNodeReady("node-c", std::chrono::seconds(5)));
  Node* c = NodeById("node-c");
  ASSERT_EQ(c->store->Head("k7").status().code(), StatusCode::kNotFound);

  // Anti-entropy repair pulls the missing objects from healthy peers.
  Repairer repairer(cluster_, clients_, /*replication_factor=*/3);
  auto report = repairer.RepairNode("node-c");
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  EXPECT_EQ(report.value().copied, 5u);          // k5..k9 restored
  EXPECT_EQ(report.value().already_current, 5u); // k0..k4 already present
  EXPECT_EQ(report.value().failed, 0u);

  // node-c now holds every object, byte-identical and checksum-valid.
  for (int i = 0; i < 10; ++i) {
    const std::string key = "k" + std::to_string(i);
    auto got = c->store->Get(key);
    ASSERT_TRUE(got.ok()) << key << ": " << got.status().ToString();
    EXPECT_EQ(got.value(), "v" + std::to_string(i));
  }
}

TEST_F(ReplicationTest, DeleteRemovesAcrossReplicas) {
  ASSERT_TRUE(coordinator_->Put("temp", "bytes").ok());
  ASSERT_TRUE(coordinator_->Delete("temp").ok());

  auto got = coordinator_->Get("temp");
  EXPECT_EQ(got.status().code(), StatusCode::kNotFound);
}

} // namespace
} // namespace dos
