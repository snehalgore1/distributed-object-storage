// A single storage node process: opens a local object store and serves the
// StorageNode gRPC service on the given address.
//
// Usage: dos_node --address HOST:PORT --data-dir PATH [--id NODE_ID]

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include "network/storage_node_server.h"
#include "storage/local_object_store.h"

namespace {

dos::StorageNodeServer* g_server = nullptr;

void HandleSignal(int) {
  if (g_server != nullptr) {
    g_server->Shutdown();
  }
}

std::string ArgValue(int argc, char** argv, const std::string& flag, const std::string& def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag == argv[i]) {
      return argv[i + 1];
    }
  }
  return def;
}

} // namespace

int main(int argc, char** argv) {
  const std::string address = ArgValue(argc, argv, "--address", "0.0.0.0:9001");
  const std::string data_dir = ArgValue(argc, argv, "--data-dir", "");
  const std::string node_id = ArgValue(argc, argv, "--id", address);

  if (data_dir.empty()) {
    std::cerr << "error: --data-dir is required\n";
    return 2;
  }

  auto store_or = dos::LocalObjectStore::Open(data_dir);
  if (!store_or.ok()) {
    std::cerr << "failed to open store at " << data_dir << ": " << store_or.status().ToString()
              << "\n";
    return 1;
  }
  auto store = std::move(store_or).value();

  dos::StorageNodeServer server(*store);
  if (!server.Start(address)) {
    std::cerr << "failed to bind " << address << "\n";
    return 1;
  }
  g_server = &server;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::cout << "node " << node_id << " serving on port " << server.bound_port() << " (data-dir "
            << data_dir << ")" << std::endl;
  server.Wait();
  std::cout << "node " << node_id << " shut down\n";
  return 0;
}
