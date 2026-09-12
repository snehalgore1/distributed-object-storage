#ifndef DOS_CLUSTER_CONTROL_PLANE_H_
#define DOS_CLUSTER_CONTROL_PLANE_H_

#include <string>
#include <vector>

#include "cluster/metadata_view.h"
#include "cluster/node_info.h"

namespace dos {

// Where a mutation should be executed. A single-node control plane is always
// the leader and serves everything locally. A Raft-replicated one accepts
// mutations only on its leader, so the Metadata service can forward a write it
// receives on a follower to the leader (etcd-style), keeping clients able to
// target any replica.
enum class Leadership {
  kLeader,   // serve the mutation here
  kFollower, // forward to leader_forward_address()
  kNoLeader, // no leader currently known; reject with UNAVAILABLE
};

// The full control-plane surface the Metadata gRPC service sits in front of:
// the object-location MetadataView plus cluster-membership admin. Implemented
// in-process by MetadataRepository (single node) or by RaftMetadataRepository
// (a replicated group, spec Milestone 15). Keeping this an interface is what
// lets the same MetadataServiceImpl serve either backend unchanged.
class ControlPlane : public MetadataView {
public:
  virtual void AddNode(const NodeInfo& node) = 0;
  virtual std::vector<NodeInfo> Nodes() const = 0;

  // Leadership / forwarding hints. Defaults suit a single-node control plane:
  // always the leader, so mutations are always served locally.
  virtual Leadership leadership() const { return Leadership::kLeader; }
  virtual std::string leader_forward_address() const { return ""; }
};

} // namespace dos

#endif // DOS_CLUSTER_CONTROL_PLANE_H_
