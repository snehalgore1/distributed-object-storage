// The metadata / control-plane process: owns cluster membership, placement, and
// the object -> replica-set map, serving the Metadata gRPC service.
//
// Single-node (default):
//   dos_metadata --address HOST:PORT [--node id=host:port ...]
//
// Raft-replicated control plane (spec Milestone 15): run three of these, each
// with a distinct --id and the other two as --raft-peer. Metadata writes survive
// a leader failure and no committed mutation is lost.
//   dos_metadata --id A --address 0.0.0.0:9000 --raft-address 0.0.0.0:9100 \
//       --raft-dir /var/lib/dos/raft-A \
//       --raft-peer B=hostB:9101 --raft-peer C=hostC:9102 \
//       --peer-meta B=hostB:9000 --peer-meta C=hostC:9000 \
//       --node node-a=... --node node-b=... --node node-c=...
//
// --peer-meta gives each peer's *metadata* address (distinct from its Raft
// address) so a write received on a follower can be forwarded to the leader.

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cluster/metadata_repository.h"
#include "cluster/raft_metadata_repository.h"
#include "consensus/raft_node.h"
#include "consensus/raft_storage.h"
#include "network/metadata_server.h"
#include "network/raft_client.h"
#include "network/raft_server.h"

namespace {

dos::MetadataServer* g_meta_server = nullptr;

// Only unblock the serving Wait() here; joining Raft threads and tearing down
// the Raft gRPC server happen back in main() after Wait() returns, since those
// take locks and are not safe to run from a signal handler.
void HandleSignal(int) {
  if (g_meta_server != nullptr)
    g_meta_server->Shutdown();
}

std::string ArgValue(int argc, char** argv, const std::string& flag, const std::string& def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag == argv[i])
      return argv[i + 1];
  }
  return def;
}

// Collects all `id=value` pairs given under a repeated flag.
std::vector<std::pair<std::string, std::string>> ArgPairs(int argc, char** argv,
                                                          const std::string& flag) {
  std::vector<std::pair<std::string, std::string>> out;
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag != argv[i])
      continue;
    const std::string spec = argv[i + 1];
    const auto eq = spec.find('=');
    if (eq != std::string::npos) {
      out.emplace_back(spec.substr(0, eq), spec.substr(eq + 1));
    }
  }
  return out;
}

void InstallSignalHandlers() {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
}

} // namespace

int main(int argc, char** argv) {
  const std::string address = ArgValue(argc, argv, "--address", "0.0.0.0:9000");
  const std::string id = ArgValue(argc, argv, "--id", "");
  const auto peers = ArgPairs(argc, argv, "--raft-peer");
  const auto peer_meta = ArgPairs(argc, argv, "--peer-meta");
  const auto nodes = ArgPairs(argc, argv, "--node");

  // --- Raft-replicated control plane --------------------------------------
  if (!id.empty() && !peers.empty()) {
    const std::string raft_address = ArgValue(argc, argv, "--raft-address", "0.0.0.0:9100");
    const std::string raft_dir = ArgValue(argc, argv, "--raft-dir", "raft-" + id);

    auto storage = dos::consensus::RaftStorage::Open(raft_dir);
    if (!storage.ok()) {
      std::cerr << "failed to open raft dir " << raft_dir << ": "
                << storage.status().ToString() << "\n";
      return 1;
    }
    auto raft_storage = std::move(storage).value();

    std::unordered_map<std::string, std::string> peer_addresses;
    std::vector<std::string> peer_ids;
    for (const auto& [pid, paddr] : peers) {
      peer_addresses[pid] = paddr;
      peer_ids.push_back(pid);
    }
    dos::GrpcRaftTransport transport(peer_addresses, std::chrono::milliseconds(150));

    auto repo = std::make_unique<dos::RaftMetadataRepository>();
    for (const auto& [nid, naddr] : nodes) {
      dos::NodeInfo info;
      info.id = nid;
      info.address = naddr;
      repo->AddBootstrapNode(info); // identical static membership on every replica
      std::cout << "registered " << nid << " -> " << naddr << "\n";
    }
    for (const auto& [pid, paddr] : peer_meta) {
      repo->SetPeerMetadataAddress(pid, paddr); // for follower->leader write forwarding
    }

    dos::consensus::RaftConfig config;
    config.id = id;
    config.peers = peer_ids;
    config.election_min = std::chrono::milliseconds(400);
    config.election_max = std::chrono::milliseconds(800);
    config.heartbeat = std::chrono::milliseconds(100);

    dos::RaftMetadataRepository* repo_ptr = repo.get();
    dos::consensus::RaftNode node(
        config, &transport, raft_storage.get(),
        [repo_ptr](const dos::consensus::LogEntry& e) { repo_ptr->Apply(e); });
    repo->AttachRaft(&node);

    dos::RaftServer raft_server(node);
    if (!raft_server.Start(raft_address)) {
      std::cerr << "failed to bind raft address " << raft_address << "\n";
      return 1;
    }
    node.Start();

    dos::MetadataServer meta_server(*repo);
    if (!meta_server.Start(address)) {
      std::cerr << "failed to bind " << address << "\n";
      return 1;
    }

    g_meta_server = &meta_server;
    InstallSignalHandlers();

    std::cout << "raft metadata node " << id << ": metadata on port " << meta_server.bound_port()
              << ", raft on port " << raft_server.bound_port() << " (" << peer_ids.size()
              << " peers)" << std::endl;
    meta_server.Wait();
    node.Stop();
    raft_server.Shutdown();
    return 0;
  }

  // --- Single-node control plane (default) --------------------------------
  dos::MetadataRepository repo;
  for (const auto& [nid, naddr] : nodes) {
    dos::NodeInfo info;
    info.id = nid;
    info.address = naddr;
    repo.AddNode(info);
    std::cout << "registered " << nid << " -> " << naddr << "\n";
  }

  dos::MetadataServer server(repo);
  if (!server.Start(address)) {
    std::cerr << "failed to bind " << address << "\n";
    return 1;
  }
  g_meta_server = &server;
  InstallSignalHandlers();

  std::cout << "metadata service listening on port " << server.bound_port() << std::endl;
  server.Wait();
  return 0;
}
