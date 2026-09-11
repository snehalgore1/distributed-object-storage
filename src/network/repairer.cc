#include "network/repairer.h"

#include <algorithm>
#include <vector>

#include "common/sha256.h"

namespace dos {
namespace {

struct Candidate {
  ObjectMetadata meta;   // best (highest-version) metadata seen among peers
  std::string source_id; // peer to pull the bytes from
};

} // namespace

Repairer::Repairer(const ClusterMap& cluster,
                   std::map<std::string, std::shared_ptr<StorageNodeClient>> clients,
                   std::size_t replication_factor)
    : cluster_(cluster), clients_(std::move(clients)), replication_factor_(replication_factor) {}

StatusOr<Repairer::Report> Repairer::RepairNode(const std::string& target_node_id) {
  auto target_it = clients_.find(target_node_id);
  if (target_it == clients_.end()) {
    return Status::InvalidArgument("unknown target node: " + target_node_id);
  }
  StorageNodeClient& target = *target_it->second;

  // 1) Survey peers: build the best-known version of every object, and remember
  //    which peer to pull it from.
  std::map<std::string, Candidate> candidates;
  for (auto& [node_id, client] : clients_) {
    if (node_id == target_node_id) {
      continue;
    }
    auto listing = client->List("");
    if (!listing.ok()) {
      continue; // peer unreachable; other peers may still cover these objects
    }
    for (const auto& meta : listing.value()) {
      auto it = candidates.find(meta.key);
      if (it == candidates.end() || meta.version > it->second.meta.version) {
        candidates[meta.key] = Candidate{meta, node_id};
      }
    }
  }

  // 2) For each object the target is responsible for, copy it over if missing or
  //    stale.
  Report report;
  for (const auto& [key, cand] : candidates) {
    const std::vector<std::string> placement = cluster_.PlacementFor(key, replication_factor_);
    if (std::find(placement.begin(), placement.end(), target_node_id) == placement.end()) {
      continue; // not this node's responsibility
    }

    auto head = target.Head(key);
    if (head.ok() && head.value().version >= cand.meta.version) {
      ++report.already_current;
      continue;
    }

    // Pull the bytes from the chosen source and verify integrity before writing.
    StorageNodeClient& source = *clients_[cand.source_id];
    auto data = source.Get(key);
    if (!data.ok()) {
      ++report.failed;
      continue;
    }
    if (Sha256Hex(data.value()) != cand.meta.checksum) {
      ++report.failed; // corrupt source; never propagate it
      continue;
    }
    auto put = target.Put(key, data.value(), cand.meta.version);
    if (put.ok()) {
      ++report.copied;
    } else {
      ++report.failed;
    }
  }
  return report;
}

} // namespace dos
