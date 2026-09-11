#ifndef DOS_NETWORK_REPAIRER_H_
#define DOS_NETWORK_REPAIRER_H_

#include <cstddef>
#include <map>
#include <memory>
#include <string>

#include "cluster/cluster_map.h"
#include "common/status.h"
#include "network/storage_node_client.h"

namespace dos {

// Anti-entropy repair (spec Milestone 6). Reconciles one node's local data
// against its healthy peers: for every object the node *should* hold (per
// consistent-hash placement) but is missing or holds at an older version, the
// object is copied from a peer and checksum-verified before being committed.
//
// Repair reads only from peers whose bytes pass their own integrity check and
// re-verifies the checksum here, so it never propagates corruption.
class Repairer {
public:
  struct Report {
    std::size_t copied = 0;          // objects (re)written onto the target
    std::size_t already_current = 0; // objects the target already held
    std::size_t failed = 0;          // objects that could not be repaired
  };

  Repairer(const ClusterMap& cluster,
           std::map<std::string, std::shared_ptr<StorageNodeClient>> clients,
           std::size_t replication_factor = 3);

  // Brings `target_node_id` back to target redundancy by pulling any objects it
  // is responsible for from healthy peers.
  StatusOr<Report> RepairNode(const std::string& target_node_id);

private:
  const ClusterMap& cluster_;
  std::map<std::string, std::shared_ptr<StorageNodeClient>> clients_;
  std::size_t replication_factor_;
};

} // namespace dos

#endif // DOS_NETWORK_REPAIRER_H_
