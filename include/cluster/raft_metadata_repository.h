#ifndef DOS_CLUSTER_RAFT_METADATA_REPOSITORY_H_
#define DOS_CLUSTER_RAFT_METADATA_REPOSITORY_H_

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "cluster/control_plane.h"
#include "cluster/metadata_repository.h"
#include "consensus/raft_types.h"

namespace dos {

namespace consensus {
class RaftNode;
}

// A control plane replicated with Raft (spec Milestone 15). Mutations are
// encoded as MetadataCommand log entries and proposed to the RaftNode; only
// once an entry is committed and applied on this leader does the mutation
// return success. Application is deterministic — the same committed command
// sequence produces an identical MetadataRepository on every replica — so a new
// or recovered follower rebuilds exact state by replaying the log.
//
// Reads are served from the locally-applied state. Callers that need
// linearizable reads should direct them at the leader (is_leader()).
//
// Wiring is two-step to break the apply/propose cycle:
//   auto cp = std::make_unique<RaftMetadataRepository>();
//   RaftNode node(cfg, transport, storage, [&](const LogEntry& e){ cp->Apply(e); });
//   cp->AttachRaft(&node);
//   node.Start();
class RaftMetadataRepository : public ControlPlane {
public:
  explicit RaftMetadataRepository(
      std::size_t virtual_nodes_per_node = 150,
      std::chrono::milliseconds commit_timeout = std::chrono::milliseconds(2000))
      : inner_(virtual_nodes_per_node), commit_timeout_(commit_timeout) {}

  void AttachRaft(consensus::RaftNode* raft) { raft_ = raft; }

  // Initial membership applied directly (identically on every replica from the
  // same static config), so it need not travel through the log.
  void AddBootstrapNode(const NodeInfo& node) { inner_.AddNode(node); }

  // Records a peer's *metadata service* address (distinct from its Raft peer
  // address) so a write received on a follower can be forwarded to the leader.
  void SetPeerMetadataAddress(const std::string& peer_id, const std::string& address) {
    peer_meta_addr_[peer_id] = address;
  }

  // Leadership / forwarding, driven by the attached RaftNode.
  Leadership leadership() const override;
  std::string leader_forward_address() const override;

  // Applies one committed log entry to the underlying repository. Invoked only
  // by the RaftNode, in log order, from a single thread.
  void Apply(const consensus::LogEntry& entry);

  // ControlPlane / MetadataView. Mutations go through Raft; reads are local.
  void AddNode(const NodeInfo& node) override;
  std::vector<NodeInfo> Nodes() const override;
  std::vector<std::string> PlacementFor(std::string_view key, std::size_t rf) override;
  Status RegisterObject(const ObjectLocation& loc) override;
  StatusOr<ObjectLocation> LookupObject(std::string_view key) override;
  Status RemoveObject(std::string_view key) override;
  StatusOr<std::vector<ObjectLocation>> ListObjects(std::string_view prefix) override;

private:
  // Proposes an encoded MetadataCommand and blocks until it is committed and
  // applied. Returns kUnavailable if this node is not the leader or the commit
  // does not land within the timeout.
  Status ProposeAndWait(const std::string& command);

  MetadataRepository inner_;
  consensus::RaftNode* raft_ = nullptr;
  std::chrono::milliseconds commit_timeout_;
  std::unordered_map<std::string, std::string> peer_meta_addr_; // peer id -> metadata addr
};

} // namespace dos

#endif // DOS_CLUSTER_RAFT_METADATA_REPOSITORY_H_
