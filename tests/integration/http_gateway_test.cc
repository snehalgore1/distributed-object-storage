// End-to-end HTTP gateway test: a real HTTP server in front of a coordinator
// (backed by an in-process metadata repository + three storage nodes), driven
// over a socket with the HttpClient (spec Milestone 9).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "cluster/caching_metadata_view.h"
#include "cluster/metadata_repository.h"
#include "network/coordinator.h"
#include "network/http_client.h"
#include "network/http_gateway.h"
#include "network/storage_node_client.h"
#include "network/storage_node_server.h"
#include "storage/local_object_store.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

class HttpGatewayTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_http_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(root_);

    auto repo = std::make_shared<MetadataRepository>(150);
    std::map<std::string, std::shared_ptr<StorageNodeClient>> clients;
    for (const std::string id : {"node-a", "node-b", "node-c"}) {
      auto store = LocalObjectStore::Open(root_ / id);
      stores_.push_back(std::move(store).value());
      auto server = std::make_unique<StorageNodeServer>(*stores_.back());
      server->Start("127.0.0.1:0");
      const std::string addr = "127.0.0.1:" + std::to_string(server->bound_port());
      node_servers_.push_back(std::move(server));
      NodeInfo info;
      info.id = id;
      info.address = addr;
      repo->AddNode(info);
      clients[id] = std::make_shared<StorageNodeClient>(addr, std::chrono::milliseconds(300));
    }
    // Coordinator reads through an LRU metadata cache.
    cache_ = std::make_shared<CachingMetadataView>(repo, 1024);
    auto coordinator = std::make_shared<Coordinator>(cache_, clients, Coordinator::Options{});
    gateway_ = std::make_unique<HttpGateway>(coordinator);
    ASSERT_TRUE(gateway_->Start("127.0.0.1", 0));
    port_ = gateway_->bound_port();
  }

  void TearDown() override {
    if (gateway_)
      gateway_->Shutdown();
    for (auto& s : node_servers_) {
      if (s)
        s->Shutdown();
    }
    node_servers_.clear();
    stores_.clear();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  HttpClient Client() { return HttpClient("127.0.0.1", port_); }

  fs::path root_;
  std::vector<std::unique_ptr<LocalObjectStore>> stores_;
  std::vector<std::unique_ptr<StorageNodeServer>> node_servers_;
  std::shared_ptr<CachingMetadataView> cache_;
  std::unique_ptr<HttpGateway> gateway_;
  int port_ = 0;
};

TEST_F(HttpGatewayTest, PutGetRoundTrip) {
  auto client = Client();
  auto put = client.Request("PUT", "/objects/greeting.txt", "hello over http");
  ASSERT_TRUE(put.ok());
  EXPECT_EQ(put.value().status, 201);

  auto get = client.Request("GET", "/objects/greeting.txt");
  ASSERT_TRUE(get.ok());
  EXPECT_EQ(get.value().status, 200);
  EXPECT_EQ(get.value().body, "hello over http");
}

TEST_F(HttpGatewayTest, GetMissingIs404) {
  auto get = Client().Request("GET", "/objects/nope");
  ASSERT_TRUE(get.ok());
  EXPECT_EQ(get.value().status, 404);
}

TEST_F(HttpGatewayTest, DeleteThenGetIs404) {
  auto client = Client();
  ASSERT_EQ(client.Request("PUT", "/objects/tmp", "bytes").value().status, 201);
  EXPECT_EQ(client.Request("DELETE", "/objects/tmp").value().status, 204);
  EXPECT_EQ(client.Request("GET", "/objects/tmp").value().status, 404);
}

TEST_F(HttpGatewayTest, ListReturnsJson) {
  auto client = Client();
  ASSERT_EQ(client.Request("PUT", "/objects/a", "1").value().status, 201);
  ASSERT_EQ(client.Request("PUT", "/objects/b", "22").value().status, 201);

  auto list = client.Request("GET", "/objects");
  ASSERT_TRUE(list.ok());
  EXPECT_EQ(list.value().status, 200);
  EXPECT_NE(list.value().body.find("\"key\":\"a\""), std::string::npos);
  EXPECT_NE(list.value().body.find("\"key\":\"b\""), std::string::npos);
}

TEST_F(HttpGatewayTest, KeyWithSlashesWorks) {
  auto client = Client();
  ASSERT_EQ(client.Request("PUT", "/objects/photos/2026/cat.jpg", "meow").value().status, 201);
  auto get = client.Request("GET", "/objects/photos/2026/cat.jpg");
  EXPECT_EQ(get.value().body, "meow");
}

TEST_F(HttpGatewayTest, CacheAbsorbsRepeatedReads) {
  auto client = Client();
  ASSERT_EQ(client.Request("PUT", "/objects/hot", "v").value().status, 201);
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(client.Request("GET", "/objects/hot").value().status, 200);
  }
  // The PUT populated the cache and the reads hit it.
  EXPECT_GT(cache_->hits(), 0u);
}

} // namespace
} // namespace dos
