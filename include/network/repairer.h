#ifndef DOS_NETWORK_REPAIRER_H_
#define DOS_NETWORK_REPAIRER_H_

#include <cstddef>
#include <map>
#include <memory>
#include <string>

#include "cluster/metadata_view.h"
#include "common/status.h"
#include "network/storage_node_client.h"

namespace dos {

// Anti-entropy repair (spec Milestone 6), driven by the authoritative control
// plane. For every object the target node *should* hold (per placement) but is
// not currently a registered replica of, the object is copied from a healthy
// replica, checksum-verified, written, and the metadata replica set is updated.
//
// Repair reads only checksum-valid bytes and re-verifies here, so it never
// propagates corruption.
class Repairer {
public:
  struct Report {
    std::size_t copied = 0;
    std::size_t already_current = 0;
    std::size_t failed = 0;
  };

  Repairer(std::shared_ptr<MetadataView> metadata,
           std::map<std::string, std::shared_ptr<StorageNodeClient>> clients,
           std::size_t replication_factor = 3);

  StatusOr<Report> RepairNode(const std::string& target_node_id);

private:
  std::shared_ptr<MetadataView> metadata_;
  std::map<std::string, std::shared_ptr<StorageNodeClient>> clients_;
  std::size_t replication_factor_;
};

} // namespace dos

#endif // DOS_NETWORK_REPAIRER_H_
