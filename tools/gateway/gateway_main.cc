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

bool HasFlag(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc; ++i) {
    if (flag == argv[i])
      return true;
  }
  return false;
}

int main(int argc, char** argv) {
  const std::string listen = ArgValue(argc, argv, "--address", "127.0.0.1:8080");
  const std::string metadata_addr = ArgValue(argc, argv, "--metadata", "127.0.0.1:9000");
  const int rf = std::atoi(ArgValue(argc, argv, "--rf", "3").c_str());
  const bool no_cache = HasFlag(argc, argv, "--no-cache");

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

  // RF drives a majority write quorum: rf=1 -> W=1, rf=3 -> W=2, rf=5 -> W=3.
  dos::Coordinator::Options opts;
  opts.replication_factor = static_cast<std::size_t>(rf < 1 ? 1 : rf);
  opts.write_quorum = opts.replication_factor / 2 + 1;

  std::shared_ptr<dos::CachingMetadataView> cache;
  std::shared_ptr<dos::MetadataView> meta_view = view;
  if (!no_cache) {
    cache = std::make_shared<dos::CachingMetadataView>(view, /*capacity=*/4096);
    meta_view = cache;
  }
  auto coordinator = std::make_shared<dos::Coordinator>(meta_view, clients, opts);
  dos::HttpGateway gateway(coordinator);
  if (cache) {
    gateway.SetCacheView(cache); // expose cache hit/miss at /metrics
  }
  std::cout << "config: rf=" << opts.replication_factor << " W=" << opts.write_quorum
            << " cache=" << (no_cache ? "off" : "on") << "\n";

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
