// HTTP gateway process: discovers storage nodes from the metadata service,
// wraps a coordinator (reading through an LRU metadata cache), and serves the
// object HTTP API.
//
// Usage: dos_gateway --address HOST:PORT --metadata HOST:PORT

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "cluster/caching_metadata_view.h"
#include "network/coordinator.h"
#include "network/http_gateway.h"
#include "network/metadata_client.h"
#include "network/storage_node_client.h"

namespace {

dos::HttpGateway* g_gateway = nullptr;
std::atomic<bool> g_running{true};
void HandleSignal(int) {
  if (g_gateway != nullptr)
    g_gateway->Shutdown();
  g_running.store(false);
}

std::string ArgValue(int argc, char** argv, const std::string& flag, const std::string& def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag == argv[i])
      return argv[i + 1];
  }
  return def;
}

std::pair<std::string, int> SplitHostPort(const std::string& hp, int default_port) {
  const auto c = hp.rfind(':');
  if (c == std::string::npos)
    return {hp, default_port};
  return {hp.substr(0, c), std::atoi(hp.substr(c + 1).c_str())};
}

} // namespace

int main(int argc, char** argv) {
  const std::string listen = ArgValue(argc, argv, "--address", "127.0.0.1:8080");
  const std::string metadata_addr = ArgValue(argc, argv, "--metadata", "127.0.0.1:9000");

  auto view = std::make_shared<dos::RemoteMetadataView>(metadata_addr);
  auto nodes = view->ListNodes();
  if (!nodes.ok() || nodes.value().empty()) {
    std::cerr << "no storage nodes registered with metadata service at " << metadata_addr << "\n";
    return 1;
  }
  std::map<std::string, std::shared_ptr<dos::StorageNodeClient>> clients;
  for (const auto& n : nodes.value()) {
    clients[n.id] = std::make_shared<dos::StorageNodeClient>(n.address);
    std::cout << "node " << n.id << " -> " << n.address << "\n";
  }

  auto cache = std::make_shared<dos::CachingMetadataView>(view, /*capacity=*/4096);
  auto coordinator =
      std::make_shared<dos::Coordinator>(cache, clients, dos::Coordinator::Options{});
  dos::HttpGateway gateway(coordinator);

  const auto [host, port] = SplitHostPort(listen, 8080);
  if (!gateway.Start(host, port)) {
    std::cerr << "failed to bind " << listen << "\n";
    return 1;
  }
  g_gateway = &gateway;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::cout << "gateway listening on " << host << ":" << gateway.bound_port() << std::endl;
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return 0;
}
