// End-to-end test of the control/data-plane split (spec Milestone 8): a real
// Metadata gRPC service holds placement + object locations, three StorageNode
// services hold the bytes, and a Coordinator wired to the *remote* metadata
// service drives the full request path.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cluster/metadata_repository.h"
#include "common/sha256.h"
#include "network/coordinator.h"
#include "network/metadata_client.h"
#include "network/metadata_server.h"
#include "network/storage_node_client.h"
#include "network/storage_node_server.h"
#include "storage/local_object_store.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

class MetadataSplitTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_split_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(root_);

    // Data plane: three storage nodes.
    for (const std::string id : {"node-a", "node-b", "node-c"}) {
      auto store = LocalObjectStore::Open(root_ / id);
      ASSERT_TRUE(store.ok());
      stores_.push_back(std::move(store).value());
      auto server = std::make_unique<StorageNodeServer>(*stores_.back());
      ASSERT_TRUE(server->Start("127.0.0.1:0"));
      const std::string addr = "127.0.0.1:" + std::to_string(server->bound_port());
      node_servers_.push_back(std::move(server));

      NodeInfo info;
      info.id = id;
      info.address = addr;
      repo_.AddNode(info); // control plane learns membership
      clients_[id] = std::make_shared<StorageNodeClient>(addr, std::chrono::milliseconds(300));
    }

    // Control plane: a real metadata gRPC service in front of the repository.
    meta_server_ = std::make_unique<MetadataServer>(repo_);
    ASSERT_TRUE(meta_server_->Start("127.0.0.1:0"));
    meta_addr_ = "127.0.0.1:" + std::to_string(meta_server_->bound_port());
  }

  void TearDown() override {
    if (meta_server_)
      meta_server_->Shutdown();
    for (auto& s : node_servers_) {
      if (s)
        s->Shutdown();
    }
    node_servers_.clear();
    stores_.clear();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  fs::path root_;
  MetadataRepository repo_{150};
  std::vector<std::unique_ptr<LocalObjectStore>> stores_;
  std::vector<std::unique_ptr<StorageNodeServer>> node_servers_;
  std::map<std::string, std::shared_ptr<StorageNodeClient>> clients_;
  std::unique_ptr<MetadataServer> meta_server_;
  std::string meta_addr_;
};

TEST_F(MetadataSplitTest, FullPathThroughRemoteMetadataService) {
  // Coordinator sources placement + registers locations via the remote service.
  auto view = std::make_shared<RemoteMetadataView>(meta_addr_);
  Coordinator coordinator(view, clients_, {});

  auto put = coordinator.Put("photos/cat.jpg", "meow-bytes");
  ASSERT_TRUE(put.ok()) << put.status().ToString();

  auto got = coordinator.Get("photos/cat.jpg");
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ(got.value(), "meow-bytes");
}

TEST_F(MetadataSplitTest, MetadataLookupCarriesLocationNotPayload) {
  auto view = std::make_shared<RemoteMetadataView>(meta_addr_);
  Coordinator coordinator(view, clients_, {});
  ASSERT_TRUE(coordinator.Put("k", "the-actual-bytes").ok());

  // A separate metadata client asks the control plane about the object.
  RemoteMetadataView probe(meta_addr_);
  auto loc = probe.LookupObject("k");
  ASSERT_TRUE(loc.ok()) << loc.status().ToString();

  // The control plane returns *metadata only*: version, checksum, size, and the
  // replica set — never the payload. (ObjectLocation structurally has no data
  // field; we assert the metadata is present and correct.)
  EXPECT_EQ(loc.value().checksum, Sha256Hex("the-actual-bytes"));
  EXPECT_EQ(loc.value().size, 16u);
  EXPECT_EQ(loc.value().version, 1u);
  EXPECT_EQ(loc.value().replicas.size(), 3u);
}

TEST_F(MetadataSplitTest, PlacementComesFromTheService) {
  RemoteMetadataView view(meta_addr_);
  auto placement = view.PlacementFor("any/key", 3);
  EXPECT_EQ(placement.size(), 3u); // served by the remote metadata service
}

} // namespace
} // namespace dos
