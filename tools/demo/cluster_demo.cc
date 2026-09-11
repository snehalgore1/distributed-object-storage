// Narrated end-to-end demo of the distributed object store: three storage nodes
// (in one process for a self-contained demo), a quorum coordinator, a simulated
// node failure with a surviving read, and anti-entropy repair on rejoin.
//
// Used both as a readable transcript and as the script recorded into the
// failover GIF. Usage: cluster_demo

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cluster/cluster_map.h"
#include "cluster/metadata_repository.h"
#include "network/coordinator.h"
#include "network/repairer.h"
#include "network/storage_node_client.h"
#include "network/storage_node_server.h"
#include "storage/local_object_store.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void Pause() { std::this_thread::sleep_for(650ms); }

void Step(const std::string& line) {
  std::cout << "\n\033[1;36m» " << line << "\033[0m\n";
  Pause();
}

void Info(const std::string& line) {
  std::cout << "  " << line << "\n";
  std::this_thread::sleep_for(250ms);
}

struct Node {
  std::string id;
  fs::path dir;
  std::unique_ptr<dos::LocalObjectStore> store;
  std::unique_ptr<dos::StorageNodeServer> server;
  std::string address;
};

} // namespace

int main() {
  const fs::path root = "/tmp/dos-cluster-demo";
  std::error_code ec;
  fs::remove_all(root, ec);

  std::vector<std::unique_ptr<Node>> nodes;
  auto metadata = std::make_shared<dos::MetadataRepository>(150);
  std::map<std::string, std::shared_ptr<dos::StorageNodeClient>> clients;

  std::cout << "\033[1mDistributed Object Store — live failover demo\033[0m\n";
  Step("Starting a 3-node cluster (RF=3, write quorum W=2)");
  for (const std::string id : {"node-a", "node-b", "node-c"}) {
    auto node = std::make_unique<Node>();
    node->id = id;
    node->dir = root / id;
    node->store = std::move(dos::LocalObjectStore::Open(node->dir).value());
    node->server = std::make_unique<dos::StorageNodeServer>(*node->store);
    node->server->Start("127.0.0.1:0");
    node->address = "127.0.0.1:" + std::to_string(node->server->bound_port());
    dos::NodeInfo info;
    info.id = id;
    info.address = node->address;
    metadata->AddNode(info);
    clients[id] = std::make_shared<dos::StorageNodeClient>(node->address);
    Info(id + " listening on " + node->address);
    nodes.push_back(std::move(node));
  }
  dos::Coordinator coordinator(metadata, clients, {});

  Step("PUT photos/sunset.jpg");
  auto put = coordinator.Put("photos/sunset.jpg", "<3.2 MB of pixels>");
  Info("committed version " + std::to_string(put.value().version) + ", replicated to all 3 nodes");

  Step("GET photos/sunset.jpg");
  Info("returned: \"" + coordinator.Get("photos/sunset.jpg").value() + "\"  (checksum verified)");

  Step("Simulating failure: killing node-c");
  nodes[2]->server->Shutdown();
  nodes[2]->server.reset();
  Info("node-c is DOWN");

  Step("GET photos/sunset.jpg  (with node-c down)");
  auto after = coordinator.Get("photos/sunset.jpg");
  Info("returned: \"" + after.value() + "\"  ✓ read survived the failure (served from a replica)");

  Step("PUT logs/app.log  (while node-c is down)");
  auto put2 = coordinator.Put("logs/app.log", "request_id=42 status=200");
  Info(put2.ok() ? "committed via quorum (2/3 acks); node-c now lagging"
                 : "FAILED: " + put2.status().ToString());

  Step("node-c rejoins the cluster");
  nodes[2]->server = std::make_unique<dos::StorageNodeServer>(*nodes[2]->store);
  nodes[2]->server->Start(nodes[2]->address);
  for (int i = 0; i < 40 && !clients["node-c"]->Health().ok(); ++i) {
    std::this_thread::sleep_for(50ms);
  }
  Info("node-c is UP again — but it missed logs/app.log while it was gone");

  Step("Anti-entropy repair of node-c");
  dos::Repairer repairer(metadata, clients, 3);
  auto report = repairer.RepairNode("node-c");
  Info("copied " + std::to_string(report.value().copied) + ", already current " +
       std::to_string(report.value().already_current) + ", failed " +
       std::to_string(report.value().failed));

  Step("Verify node-c is whole again");
  Info(std::string("node-c has photos/sunset.jpg: ") +
       (nodes[2]->store->Head("photos/sunset.jpg").ok() ? "yes" : "no"));
  Info(std::string("node-c has logs/app.log:     ") +
       (nodes[2]->store->Head("logs/app.log").ok() ? "yes  ✓ redundancy restored" : "no"));

  std::cout << "\n\033[1;32mDone.\033[0m\n";
  for (auto& n : nodes) {
    if (n->server)
      n->server->Shutdown();
  }
  fs::remove_all(root, ec);
  return 0;
}
