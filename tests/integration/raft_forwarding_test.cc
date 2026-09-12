// End-to-end test of the Raft-replicated control plane over real gRPC (spec
// Milestone 15 deployment path). Three full metadata nodes — each running a
// RaftMetadataRepository, a RaftNode, a Raft gRPC server, and a Metadata gRPC
// server — form a cluster. A client points at a *follower* and issues a write;
// the follower forwards it to the leader (etcd-style) and it succeeds, then is
// visible cluster-wide. This is what lets a gateway target any replica.

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cluster/raft_metadata_repository.h"
#include "consensus/raft_node.h"
#include "consensus/raft_storage.h"
#include "network/metadata_client.h"
#include "network/metadata_server.h"
#include "network/raft_client.h"
#include "network/raft_server.h"

namespace dos {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct MetaNode {
  std::string id;
  std::unique_ptr<consensus::RaftStorage> storage;
  std::unique_ptr<RaftMetadataRepository> repo;
  std::unique_ptr<GrpcRaftTransport> transport;
  std::unique_ptr<consensus::RaftNode> raft;
  std::unique_ptr<RaftServer> raft_server;
  std::unique_ptr<MetadataServer> meta_server;
  std::string raft_addr;
  std::string meta_addr;
};

class RaftForwardingTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_raftfwd_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_.fetch_add(1)));
    fs::remove_all(root_);

    const std::vector<std::string> ids = {"A", "B", "C"};

    // Phase 1: bring up each node's storage, repo, node, and both gRPC servers
    // on ephemeral ports.
    for (const std::string& id : ids) {
      auto n = std::make_unique<MetaNode>();
      n->id = id;
      auto st = consensus::RaftStorage::Open(root_ / id);
      n->storage = std::move(st).value();
      n->repo = std::make_unique<RaftMetadataRepository>();
      n->transport = std::make_unique<GrpcRaftTransport>(
          std::unordered_map<std::string, std::string>{}, 150ms);

      consensus::RaftConfig cfg;
      cfg.id = id;
      for (const std::string& other : ids) {
        if (other != id)
          cfg.peers.push_back(other);
      }
      cfg.election_min = 150ms;
      cfg.election_max = 300ms;
      cfg.heartbeat = 40ms;

      RaftMetadataRepository* repo_ptr = n->repo.get();
      n->raft = std::make_unique<consensus::RaftNode>(
          cfg, n->transport.get(), n->storage.get(),
          [repo_ptr](const consensus::LogEntry& e) { repo_ptr->Apply(e); });
      n->repo->AttachRaft(n->raft.get());

      n->raft_server = std::make_unique<RaftServer>(*n->raft);
      ASSERT_TRUE(n->raft_server->Start("127.0.0.1:0"));
      n->raft_addr = "127.0.0.1:" + std::to_string(n->raft_server->bound_port());

      n->meta_server = std::make_unique<MetadataServer>(*n->repo);
      ASSERT_TRUE(n->meta_server->Start("127.0.0.1:0"));
      n->meta_addr = "127.0.0.1:" + std::to_string(n->meta_server->bound_port());

      nodes_.push_back(std::move(n));
    }

    // Phase 2: now that all ports are known, wire peer raft + metadata addresses.
    for (auto& n : nodes_) {
      for (auto& other : nodes_) {
        if (other->id == n->id)
          continue;
        n->transport->SetPeer(other->id, other->raft_addr);
        n->repo->SetPeerMetadataAddress(other->id, other->meta_addr);
      }
    }

    // Phase 3: start the consensus loops.
    for (auto& n : nodes_) {
      n->raft->Start();
    }
  }

  void TearDown() override {
    for (auto& n : nodes_) {
      if (n->meta_server)
        n->meta_server->Shutdown();
      if (n->raft)
        n->raft->Stop();
      if (n->raft_server)
        n->raft_server->Shutdown();
    }
    nodes_.clear();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  MetaNode* WaitForLeader(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      for (auto& n : nodes_) {
        if (n->raft->is_leader())
          return n.get();
      }
      std::this_thread::sleep_for(10ms);
    }
    return nullptr;
  }

  MetaNode* AnyFollower(const MetaNode* leader) {
    for (auto& n : nodes_) {
      if (n.get() != leader)
        return n.get();
    }
    return nullptr;
  }

  static std::atomic<int> counter_;
  fs::path root_;
  std::vector<std::unique_ptr<MetaNode>> nodes_;
};

std::atomic<int> RaftForwardingTest::counter_{0};

TEST_F(RaftForwardingTest, FollowerForwardsWriteToLeader) {
  MetaNode* leader = WaitForLeader(3000ms);
  ASSERT_NE(leader, nullptr) << "no leader elected";

  MetaNode* follower = AnyFollower(leader);
  ASSERT_NE(follower, nullptr);

  // Client talks ONLY to the follower's metadata service.
  RemoteMetadataView client(follower->meta_addr, 2000ms);

  ObjectLocation loc;
  loc.key = "photos/cat.jpg";
  loc.version = 1;
  loc.checksum = "abc123";
  loc.size = 42;
  loc.replicas = {"node-a", "node-b", "node-c"};

  // The follower forwards this to the leader; it must succeed.
  Status s = client.RegisterObject(loc);
  ASSERT_TRUE(s.ok()) << "write to follower not forwarded: " << s.ToString();

  // And it is now visible when asked (leader has it; give followers a moment).
  std::this_thread::sleep_for(300ms);
  RemoteMetadataView leader_client(leader->meta_addr, 2000ms);
  auto got = leader_client.LookupObject("photos/cat.jpg");
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ(got.value().checksum, "abc123");
  EXPECT_EQ(got.value().version, 1u);
}

} // namespace
} // namespace dos
