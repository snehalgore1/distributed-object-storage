#include "network/coordinator.h"

#include <future>
#include <utility>

#include "common/sha256.h"

namespace dos {

const char* ReplicaHealthName(ReplicaHealth h) {
  switch (h) {
  case ReplicaHealth::kHealthy:
    return "HEALTHY";
  case ReplicaHealth::kLagging:
    return "LAGGING";
  case ReplicaHealth::kFailed:
    return "FAILED";
  }
  return "UNKNOWN";
}

Coordinator::Coordinator(std::shared_ptr<MetadataView> metadata,
                         std::map<std::string, std::shared_ptr<StorageNodeClient>> clients,
                         Options opts)
    : metadata_(std::move(metadata)), clients_(std::move(clients)), opts_(opts) {
  for (const auto& [id, client] : clients_) {
    health_[id] = ReplicaHealth::kHealthy;
  }
}

StorageNodeClient* Coordinator::ClientFor(const std::string& node_id) {
  auto it = clients_.find(node_id);
  return it == clients_.end() ? nullptr : it->second.get();
}

void Coordinator::MarkHealth(const std::string& node_id, ReplicaHealth h) {
  std::lock_guard<std::mutex> lock(health_mu_);
  health_[node_id] = h;
}

StatusOr<ObjectMetadata> Coordinator::Put(const std::string& key, const std::string& data) {
  const std::vector<std::string> replicas = metadata_->PlacementFor(key, opts_.replication_factor);
  if (replicas.size() < opts_.write_quorum) {
    return Status::Unavailable("cluster too small to satisfy write quorum");
  }

  // The metadata service is the version authority.
  uint64_t version = 1;
  auto current = metadata_->LookupObject(key);
  if (current.ok()) {
    version = current.value().version + 1;
  }

  // Fan out writes in parallel; each future yields (node_id, ok).
  std::vector<std::future<std::pair<std::string, bool>>> futures;
  futures.reserve(replicas.size());
  for (const auto& node_id : replicas) {
    futures.push_back(std::async(std::launch::async, [this, node_id, &key, &data, version] {
      StorageNodeClient* client = ClientFor(node_id);
      if (client == nullptr) {
        return std::make_pair(node_id, false);
      }
      return std::make_pair(node_id, client->Put(key, data, version).status().ok());
    }));
  }

  std::vector<std::string> acked; // in placement order
  std::vector<bool> ok_by_index(replicas.size(), false);
  for (std::size_t i = 0; i < futures.size(); ++i) {
    auto [node_id, ok] = futures[i].get();
    ok_by_index[i] = ok;
    MarkHealth(node_id, ok ? ReplicaHealth::kHealthy : ReplicaHealth::kLagging);
  }
  for (std::size_t i = 0; i < replicas.size(); ++i) {
    if (ok_by_index[i])
      acked.push_back(replicas[i]);
  }

  if (acked.size() < opts_.write_quorum) {
    return Status::Unavailable("write quorum not met: " + std::to_string(acked.size()) + "/" +
                               std::to_string(opts_.write_quorum) + " acks");
  }

  ObjectMetadata meta;
  meta.key = key;
  meta.size = data.size();
  meta.checksum = Sha256Hex(data);
  meta.version = version;
  meta.deleted = false;

  ObjectLocation loc;
  loc.key = key;
  loc.version = version;
  loc.checksum = meta.checksum;
  loc.size = meta.size;
  loc.deleted = false;
  loc.replicas = acked;
  Status reg = metadata_->RegisterObject(loc);
  if (!reg.ok()) {
    return reg;
  }
  return meta;
}

StatusOr<ObjectMetadata> Coordinator::PutConditional(const std::string& key,
                                                     const std::string& data,
                                                     uint64_t expected_version,
                                                     const std::string& request_id) {
  const std::vector<std::string> replicas = metadata_->PlacementFor(key, opts_.replication_factor);
  if (replicas.size() < opts_.write_quorum) {
    return Status::Unavailable("cluster too small to satisfy write quorum");
  }
  const uint64_t version = expected_version + 1;

  std::vector<std::future<std::pair<std::string, StatusCode>>> futures;
  futures.reserve(replicas.size());
  for (const auto& node_id : replicas) {
    futures.push_back(std::async(std::launch::async, [this, node_id, &key, &data, expected_version,
                                                      &request_id] {
      StorageNodeClient* client = ClientFor(node_id);
      if (client == nullptr) {
        return std::make_pair(node_id, StatusCode::kUnavailable);
      }
      return std::make_pair(
          node_id, client->PutConditional(key, data, expected_version, request_id).status().code());
    }));
  }

  std::vector<std::string> acked;
  std::vector<StatusCode> code_by_index(replicas.size(), StatusCode::kUnavailable);
  bool saw_conflict = false;
  for (std::size_t i = 0; i < futures.size(); ++i) {
    auto [node_id, code] = futures[i].get();
    code_by_index[i] = code;
    if (code == StatusCode::kConflict)
      saw_conflict = true;
    MarkHealth(node_id, code == StatusCode::kOk         ? ReplicaHealth::kHealthy
                        : code == StatusCode::kConflict ? ReplicaHealth::kHealthy
                                                        : ReplicaHealth::kLagging);
  }
  for (std::size_t i = 0; i < replicas.size(); ++i) {
    if (code_by_index[i] == StatusCode::kOk)
      acked.push_back(replicas[i]);
  }

  if (acked.size() >= opts_.write_quorum) {
    ObjectMetadata meta;
    meta.key = key;
    meta.size = data.size();
    meta.checksum = Sha256Hex(data);
    meta.version = version;
    meta.request_id = request_id;
    ObjectLocation loc;
    loc.key = key;
    loc.version = version;
    loc.checksum = meta.checksum;
    loc.size = meta.size;
    loc.replicas = acked;
    Status reg = metadata_->RegisterObject(loc);
    if (!reg.ok())
      return reg;
    return meta;
  }
  if (saw_conflict) {
    return Status::Conflict("conditional write rejected: expected version " +
                            std::to_string(expected_version));
  }
  return Status::Unavailable("write quorum not met for conditional put");
}

StatusOr<std::string> Coordinator::Get(const std::string& key) {
  auto loc = metadata_->LookupObject(key);
  if (!loc.ok()) {
    return loc.status(); // kNotFound if absent/tombstoned
  }
  const std::vector<std::string>& replicas = loc.value().replicas;
  if (replicas.empty()) {
    return Status::Unavailable("no replicas recorded for key");
  }

  Status last = Status::NotFound("object not found on any replica: " + key);
  for (const auto& node_id : replicas) {
    StorageNodeClient* client = ClientFor(node_id);
    if (client == nullptr) {
      continue;
    }
    auto data = client->Get(key);
    if (data.ok()) {
      MarkHealth(node_id, ReplicaHealth::kHealthy);
      return data; // node already verified the checksum
    }
    last = data.status();
    if (data.status().code() == StatusCode::kUnavailable) {
      MarkHealth(node_id, ReplicaHealth::kFailed);
    } else if (data.status().code() == StatusCode::kChecksumMismatch) {
      MarkHealth(node_id, ReplicaHealth::kLagging);
    }
  }
  return last;
}

Status Coordinator::Delete(const std::string& key) {
  const std::vector<std::string> replicas = metadata_->PlacementFor(key, opts_.replication_factor);
  std::size_t acks = 0;
  for (const auto& node_id : replicas) {
    StorageNodeClient* client = ClientFor(node_id);
    if (client == nullptr) {
      continue;
    }
    Status s = client->Delete(key);
    if (s.ok() || s.code() == StatusCode::kNotFound) {
      ++acks;
      MarkHealth(node_id, ReplicaHealth::kHealthy);
    } else {
      MarkHealth(node_id, ReplicaHealth::kFailed);
    }
  }
  if (acks < opts_.write_quorum) {
    return Status::Unavailable("delete quorum not met");
  }
  return metadata_->RemoveObject(key);
}

std::map<std::string, ReplicaHealth> Coordinator::ReplicaHealthSnapshot() const {
  std::lock_guard<std::mutex> lock(health_mu_);
  return health_;
}

} // namespace dos
