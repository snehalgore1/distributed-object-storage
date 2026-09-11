#include "cluster/metadata_repository.h"

namespace dos {

void MetadataRepository::AddNode(const NodeInfo& node) {
  std::lock_guard<std::mutex> lock(mu_);
  cluster_.AddOrUpdateNode(node);
}

std::vector<NodeInfo> MetadataRepository::Nodes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return cluster_.Nodes();
}

std::vector<std::string> MetadataRepository::PlacementFor(std::string_view key, std::size_t rf) {
  std::lock_guard<std::mutex> lock(mu_);
  return cluster_.PlacementFor(key, rf);
}

Status MetadataRepository::RegisterObject(const ObjectLocation& loc) {
  std::lock_guard<std::mutex> lock(mu_);
  objects_[loc.key] = loc;
  return Status::Ok();
}

StatusOr<ObjectLocation> MetadataRepository::LookupObject(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = objects_.find(std::string(key));
  if (it == objects_.end() || it->second.deleted) {
    return Status::NotFound("no metadata for key: " + std::string(key));
  }
  return it->second;
}

Status MetadataRepository::RemoveObject(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = objects_.find(std::string(key));
  if (it == objects_.end() || it->second.deleted) {
    return Status::NotFound("no metadata for key: " + std::string(key));
  }
  it->second.deleted = true;
  it->second.replicas.clear();
  return Status::Ok();
}

StatusOr<std::vector<ObjectLocation>> MetadataRepository::ListObjects(std::string_view prefix) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<ObjectLocation> out;
  for (const auto& [key, loc] : objects_) {
    if (!loc.deleted && key.rfind(std::string(prefix), 0) == 0) {
      out.push_back(loc);
    }
  }
  return out;
}

} // namespace dos
