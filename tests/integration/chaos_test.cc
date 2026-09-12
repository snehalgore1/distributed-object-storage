// Chaos / failure-injection tests (spec Milestone 14): faults are injected
// while real traffic runs against an in-process 3-node cluster, and the
// documented guarantees are asserted. Faults from threads are recorded in
// atomics and checked on the main thread (GoogleTest fatal assertions are not
// thread-safe).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cluster/metadata_repository.h"
#include "common/digest.h"
#include "network/coordinator.h"
#include "network/repairer.h"
#include "network/storage_node_client.h"
#include "network/storage_node_server.h"
#include "storage/local_object_store.h"

namespace dos {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Node {
  std::string id;
  fs::path dir;
  std::unique_ptr<LocalObjectStore> store;
  std::unique_ptr<StorageNodeServer> server;
  std::string address;
};

class ChaosTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_chaos_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(root_);
    repo_ = std::make_shared<MetadataRepository>(150);
    for (const std::string id : {"node-a", "node-b", "node-c"}) {
      auto n = std::make_unique<Node>();
      n->id = id;
      n->dir = root_ / id;
      n->store = std::move(LocalObjectStore::Open(n->dir).value());
      n->server = std::make_unique<StorageNodeServer>(*n->store);
      n->server->Start("127.0.0.1:0");
      n->address = "127.0.0.1:" + std::to_string(n->server->bound_port());
      NodeInfo info;
      info.id = id;
      info.address = n->address;
      repo_->AddNode(info);
      clients_[id] = std::make_shared<StorageNodeClient>(n->address, 300ms);
      nodes_.push_back(std::move(n));
    }
    coordinator_ = std::make_unique<Coordinator>(repo_, clients_, Coordinator::Options{});
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

  std::string Key(int i) { return "obj/" + std::to_string(i); }
  std::string Val(int i) { return "value-" + std::to_string(i); }

  void Prewrite(int n) {
    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(coordinator_->Put(Key(i), Val(i)).ok());
    }
  }

  fs::path root_;
  std::shared_ptr<MetadataRepository> repo_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::map<std::string, std::shared_ptr<StorageNodeClient>> clients_;
  std::unique_ptr<Coordinator> coordinator_;
};

// Kill a node during steady GET traffic: every read still returns correct bytes
// from a surviving replica.
TEST_F(ChaosTest, ReadsSurviveNodeKilledDuringTraffic) {
  constexpr int kKeys = 60;
  Prewrite(kKeys);

  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0};
  std::atomic<int> ok{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 6; ++t) {
    readers.emplace_back([&, t] {
      int i = t;
      while (!stop.load()) {
        const int k = i++ % kKeys;
        auto r = coordinator_->Get(Key(k));
        if (r.ok()) {
          if (r.value() != Val(k))
            wrong.fetch_add(1);
          ok.fetch_add(1);
        }
      }
    });
  }

  std::this_thread::sleep_for(150ms);
  nodes_.back()->server->Shutdown(); // kill node-c mid-traffic
  nodes_.back()->server.reset();
  std::this_thread::sleep_for(300ms);
  stop.store(true);
  for (auto& r : readers)
    r.join();

  EXPECT_EQ(wrong.load(), 0); // never served incorrect bytes
  EXPECT_GT(ok.load(), 0);    // reads kept succeeding through the failure
}

// Writes continue to meet the W=2 quorum with one node down; on rejoin, repair
// restores full redundancy.
TEST_F(ChaosTest, WritesMeetQuorumWithNodeDownThenRepairRestores) {
  nodes_.back()->server->Shutdown(); // node-c down
  nodes_.back()->server.reset();

  int committed = 0;
  for (int i = 0; i < 30; ++i) {
    if (coordinator_->Put(Key(i), Val(i)).ok())
      ++committed;
  }
  EXPECT_EQ(committed, 30); // quorum still satisfiable with 2 of 3

  // node-c rejoins and is repaired.
  Node* c = nodes_.back().get();
  c->server = std::make_unique<StorageNodeServer>(*c->store);
  ASSERT_TRUE(c->server->Start(c->address));
  for (int i = 0; i < 40 && !clients_["node-c"]->Health().ok(); ++i) {
    std::this_thread::sleep_for(50ms);
  }
  Repairer repairer(repo_, clients_, 3);
  auto report = repairer.RepairNode("node-c");
  ASSERT_TRUE(report.ok());
  EXPECT_EQ(report.value().failed, 0u);

  for (int i = 0; i < 30; ++i) {
    EXPECT_TRUE(c->store->Head(Key(i)).ok()) << "node-c missing " << Key(i);
  }
}

// Corrupting a replica's on-disk copy during traffic never yields wrong bytes:
// the node detects the checksum mismatch and the coordinator falls back.
TEST_F(ChaosTest, CorruptionNeverServedWrong) {
  constexpr int kKeys = 40;
  Prewrite(kKeys);

  // Corrupt the primary copy of half the keys.
  for (int i = 0; i < kKeys; i += 2) {
    const std::string primary = repo_->PlacementFor(Key(i), 3).front();
    const fs::path p = PhysicalPath(NodeById(primary)->dir / "data", KeyDigest(Key(i)));
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "corrupted";
  }

  std::atomic<int> wrong{0};
  std::atomic<int> notfound{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 6; ++t) {
    readers.emplace_back([&, t] {
      for (int i = t; i < kKeys; i += 6) {
        auto r = coordinator_->Get(Key(i));
        if (r.ok()) {
          if (r.value() != Val(i))
            wrong.fetch_add(1);
        } else {
          notfound.fetch_add(1);
        }
      }
    });
  }
  for (auto& r : readers)
    r.join();

  EXPECT_EQ(wrong.load(), 0);    // corrupt bytes never returned
  EXPECT_EQ(notfound.load(), 0); // healthy replicas covered every key
}

// A write that cannot reach a quorum (two nodes down) fails and is never
// observable as committed.
TEST_F(ChaosTest, NoFalseCommitWhenQuorumLost) {
  nodes_[1]->server->Shutdown();
  nodes_[2]->server->Shutdown();

  auto put = coordinator_->Put("k", "v");
  ASSERT_FALSE(put.ok());
  EXPECT_EQ(put.status().code(), StatusCode::kUnavailable);

  auto got = coordinator_->Get("k");
  EXPECT_EQ(got.status().code(), StatusCode::kNotFound); // never committed
}

} // namespace
} // namespace dos
