#include "network/repairer.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "common/sha256.h"

namespace dos {

Repairer::Repairer(std::shared_ptr<MetadataView> metadata,
                   std::map<std::string, std::shared_ptr<StorageNodeClient>> clients,
                   std::size_t replication_factor)
    : metadata_(std::move(metadata)), clients_(std::move(clients)),
      replication_factor_(replication_factor) {}

StatusOr<Repairer::Report> Repairer::RepairNode(const std::string& target_node_id) {
  auto target_it = clients_.find(target_node_id);
  if (target_it == clients_.end()) {
    return Status::InvalidArgument("unknown target node: " + target_node_id);
  }
  StorageNodeClient& target = *target_it->second;

  auto objects = metadata_->ListObjects("");
  if (!objects.ok()) {
    return objects.status();
  }

  Report report;
  for (const auto& loc : objects.value()) {
    const std::vector<std::string> placement =
        metadata_->PlacementFor(loc.key, replication_factor_);
    if (std::find(placement.begin(), placement.end(), target_node_id) == placement.end()) {
      continue; // not this node's responsibility
    }
    if (std::find(loc.replicas.begin(), loc.replicas.end(), target_node_id) != loc.replicas.end()) {
      ++report.already_current;
      continue;
    }

    // Pull from any registered replica that answers with checksum-valid bytes.
    bool repaired = false;
    for (const auto& source_id : loc.replicas) {
      auto sit = clients_.find(source_id);
      if (sit == clients_.end()) {
        continue;
      }
      auto data = sit->second->Get(loc.key);
      if (!data.ok() || Sha256Hex(data.value()) != loc.checksum) {
        continue; // unreachable or corrupt source; try another
      }
      if (target.Put(loc.key, data.value(), loc.version).ok()) {
        ObjectLocation updated = loc;
        updated.replicas.push_back(target_node_id);
        metadata_->RegisterObject(updated);
        ++report.copied;
        repaired = true;
      }
      break;
    }
    if (!repaired) {
      ++report.failed;
    }
  }
  return report;
}

} // namespace dos
