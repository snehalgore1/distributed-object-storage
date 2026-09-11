// The metadata / control-plane process: owns cluster membership, placement, and
// the object -> replica-set map, serving the Metadata gRPC service.
//
// Usage: dos_metadata --address HOST:PORT [--node id=host:port ...]

#include <csignal>
#include <iostream>
#include <string>

#include "cluster/metadata_repository.h"
#include "network/metadata_server.h"

namespace {

dos::MetadataServer* g_server = nullptr;
void HandleSignal(int) {
  if (g_server != nullptr)
    g_server->Shutdown();
}

std::string ArgValue(int argc, char** argv, const std::string& flag, const std::string& def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag == argv[i])
      return argv[i + 1];
  }
  return def;
}

} // namespace

int main(int argc, char** argv) {
  const std::string address = ArgValue(argc, argv, "--address", "0.0.0.0:9000");

  dos::MetadataRepository repo;
  // Register nodes passed as: --node node-a=127.0.0.1:9001
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--node") {
      const std::string spec = argv[i + 1];
      const auto eq = spec.find('=');
      if (eq != std::string::npos) {
        dos::NodeInfo info;
        info.id = spec.substr(0, eq);
        info.address = spec.substr(eq + 1);
        repo.AddNode(info);
        std::cout << "registered " << info.id << " -> " << info.address << "\n";
      }
    }
  }

  dos::MetadataServer server(repo);
  if (!server.Start(address)) {
    std::cerr << "failed to bind " << address << "\n";
    return 1;
  }
  g_server = &server;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::cout << "metadata service listening on port " << server.bound_port() << std::endl;
  server.Wait();
  return 0;
}
