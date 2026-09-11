#include "network/coordinator.h"

#include <future>

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

Coordinator::Coordinator(ClusterMap cluster,
                         std::map<std::string, std::shared_ptr<StorageNodeClient>> clients,
                         Options opts)
    : cluster_(std::move(cluster)), clients_(std::move(clients)), opts_(opts) {
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

// Determines the next version by taking the max committed version seen across
// the replicas plus one. Replicas that don't answer are simply ignored here;
// the write path below still needs a quorum to commit.
uint64_t Coordinator::NextVersion(const std::string& key,
                                  const std::vector<std::string>& replicas) {
  uint64_t max_version = 0;
  for (const auto& node_id : replicas) {
    StorageNodeClient* client = ClientFor(node_id);
    if (client == nullptr) {
      continue;
    }
    auto head = client->Head(key);
    if (head.ok()) {
      max_version = std::max(max_version, head.value().version);
    }
  }
  return max_version + 1;
}

StatusOr<ObjectMetadata> Coordinator::Put(const std::string& key, const std::string& data) {
  const std::vector<std::string> replicas = cluster_.PlacementFor(key, opts_.replication_factor);
  if (replicas.size() < opts_.write_quorum) {
    return Status::Unavailable("cluster too small to satisfy write quorum");
  }

  const uint64_t version = NextVersion(key, replicas);

  // Fan out writes in parallel; each future yields (node_id, ok).
  std::vector<std::future<std::pair<std::string, bool>>> futures;
  futures.reserve(replicas.size());
  for (const auto& node_id : replicas) {
    futures.push_back(std::async(std::launch::async, [this, node_id, &key, &data, version] {
      StorageNodeClient* client = ClientFor(node_id);
      if (client == nullptr) {
        return std::make_pair(node_id, false);
      }
      Status s = client->Put(key, data, version).status();
      return std::make_pair(node_id, s.ok());
    }));
  }

  std::size_t acks = 0;
  for (auto& f : futures) {
    auto [node_id, ok] = f.get();
    if (ok) {
      ++acks;
      MarkHealth(node_id, ReplicaHealth::kHealthy);
    } else {
      // Wrote to a quorum elsewhere but not here -> this replica lags and is a
      // repair candidate (Milestone 6 acts on this).
      MarkHealth(node_id, ReplicaHealth::kLagging);
    }
  }

  if (acks < opts_.write_quorum) {
    return Status::Unavailable("write quorum not met: " + std::to_string(acks) + "/" +
                               std::to_string(opts_.write_quorum) + " acks");
  }

  ObjectMetadata meta;
  meta.key = key;
  meta.size = data.size();
  meta.checksum = Sha256Hex(data);
  meta.version = version;
  meta.deleted = false;
  return meta;
}

StatusOr<ObjectMetadata> Coordinator::PutConditional(const std::string& key,
                                                     const std::string& data,
                                                     uint64_t expected_version,
                                                     const std::string& request_id) {
  const std::vector<std::string> replicas = cluster_.PlacementFor(key, opts_.replication_factor);
  if (replicas.size() < opts_.write_quorum) {
    return Status::Unavailable("cluster too small to satisfy write quorum");
  }

  // The version is fully determined by the expectation, so every replica (and
  // every retry) computes the same version -> no drift, retries are idempotent.
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

  std::size_t acks = 0;
  bool saw_conflict = false;
  for (auto& f : futures) {
    auto [node_id, code] = f.get();
    if (code == StatusCode::kOk) {
      ++acks;
      MarkHealth(node_id, ReplicaHealth::kHealthy);
    } else {
      if (code == StatusCode::kConflict) {
        saw_conflict = true;
      }
      MarkHealth(node_id,
                 code == StatusCode::kConflict ? ReplicaHealth::kHealthy : ReplicaHealth::kLagging);
    }
  }

  if (acks >= opts_.write_quorum) {
    ObjectMetadata meta;
    meta.key = key;
    meta.size = data.size();
    meta.checksum = Sha256Hex(data);
    meta.version = version;
    meta.deleted = false;
    meta.request_id = request_id;
    return meta;
  }
  if (saw_conflict) {
    return Status::Conflict("conditional write rejected: expected version " +
                            std::to_string(expected_version));
  }
  return Status::Unavailable("write quorum not met for conditional put");
}

StatusOr<std::string> Coordinator::Get(const std::string& key) {
  const std::vector<std::string> replicas = cluster_.PlacementFor(key, opts_.replication_factor);
  if (replicas.empty()) {
    return Status::Unavailable("no replicas for key");
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
    // Integrity failure or unreachable: fall through to the next replica.
    if (data.status().code() == StatusCode::kUnavailable ||
        data.status().code() == StatusCode::kChecksumMismatch) {
      MarkHealth(node_id, data.status().code() == StatusCode::kUnavailable
                              ? ReplicaHealth::kFailed
                              : ReplicaHealth::kLagging);
    }
  }
  return last;
}

Status Coordinator::Delete(const std::string& key) {
  const std::vector<std::string> replicas = cluster_.PlacementFor(key, opts_.replication_factor);
  if (replicas.size() < opts_.write_quorum) {
    return Status::Unavailable("cluster too small to satisfy quorum");
  }

  std::size_t acks = 0;
  for (const auto& node_id : replicas) {
    StorageNodeClient* client = ClientFor(node_id);
    if (client == nullptr) {
      continue;
    }
    Status s = client->Delete(key);
    // OK (tombstoned) or NotFound (already absent) both count as success.
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
  return Status::Ok();
}

std::map<std::string, ReplicaHealth> Coordinator::ReplicaHealthSnapshot() const {
  std::lock_guard<std::mutex> lock(health_mu_);
  return health_;
}

} // namespace dos
