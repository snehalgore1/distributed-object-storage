#ifndef DOS_NETWORK_COORDINATOR_H_
#define DOS_NETWORK_COORDINATOR_H_

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cluster/cluster_map.h"
#include "common/status.h"
#include "network/storage_node_client.h"
#include "storage/object_metadata.h"

namespace dos {

// Last-known health of a replica, from the coordinator's point of view.
enum class ReplicaHealth {
  kHealthy, // last operation acknowledged
  kLagging, // last write did not ack; needs repair (Milestone 6)
  kFailed,  // unreachable / RPC failure
};

const char* ReplicaHealthName(ReplicaHealth h);

// Coordinates reads and writes across a replica set using consistent-hash
// placement (spec Milestone 4).
//
// Write path: assign a version, fan out to all RF replicas in parallel, and ACK
// once the write quorum W succeeds. Read path: try replicas in placement order,
// falling through to the next on failure or checksum mismatch (each node
// verifies integrity, so a successful read is checksum-valid).
class Coordinator {
public:
  struct Options {
    std::size_t replication_factor = 3;
    std::size_t write_quorum = 2; // W: acks required before a write is durable
  };

  Coordinator(ClusterMap cluster, std::map<std::string, std::shared_ptr<StorageNodeClient>> clients,
              Options opts);

  // Writes to the replica set. Succeeds only once W replicas acknowledge.
  StatusOr<ObjectMetadata> Put(const std::string& key, const std::string& data);

  // Reads from the first replica that returns checksum-valid data.
  StatusOr<std::string> Get(const std::string& key);

  // Tombstones the object across the replica set (quorum of acks required).
  Status Delete(const std::string& key);

  // Snapshot of last-known replica health (for metrics / debugging).
  std::map<std::string, ReplicaHealth> ReplicaHealthSnapshot() const;

private:
  StorageNodeClient* ClientFor(const std::string& node_id);
  uint64_t NextVersion(const std::string& key, const std::vector<std::string>& replicas);
  void MarkHealth(const std::string& node_id, ReplicaHealth h);

  ClusterMap cluster_;
  std::map<std::string, std::shared_ptr<StorageNodeClient>> clients_;
  Options opts_;

  mutable std::mutex health_mu_;
  std::map<std::string, ReplicaHealth> health_;
};

} // namespace dos

#endif // DOS_NETWORK_COORDINATOR_H_
