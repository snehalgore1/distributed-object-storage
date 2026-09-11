#ifndef DOS_NETWORK_COORDINATOR_H_
#define DOS_NETWORK_COORDINATOR_H_

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cluster/metadata_view.h"
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

// Orchestrates reads and writes across the data plane, using the control plane
// (MetadataView) for placement and the authoritative object -> replica map
// (spec Milestones 4-8). Placement and versioning live in the metadata service,
// not in the coordinator: the coordinator asks where to write, writes to the
// data-plane storage nodes under a quorum, and registers the resulting replica
// set back with the metadata service.
class Coordinator {
public:
  struct Options {
    std::size_t replication_factor = 3;
    std::size_t write_quorum = 2; // W: acks required before a write is durable
  };

  Coordinator(std::shared_ptr<MetadataView> metadata,
              std::map<std::string, std::shared_ptr<StorageNodeClient>> clients, Options opts);

  StatusOr<ObjectMetadata> Put(const std::string& key, const std::string& data);

  StatusOr<ObjectMetadata> PutConditional(const std::string& key, const std::string& data,
                                          uint64_t expected_version, const std::string& request_id);

  StatusOr<std::string> Get(const std::string& key);
  Status Delete(const std::string& key);

  std::map<std::string, ReplicaHealth> ReplicaHealthSnapshot() const;

private:
  StorageNodeClient* ClientFor(const std::string& node_id);
  void MarkHealth(const std::string& node_id, ReplicaHealth h);

  std::shared_ptr<MetadataView> metadata_;
  std::map<std::string, std::shared_ptr<StorageNodeClient>> clients_;
  Options opts_;

  mutable std::mutex health_mu_;
  std::map<std::string, ReplicaHealth> health_;
};

} // namespace dos

#endif // DOS_NETWORK_COORDINATOR_H_
