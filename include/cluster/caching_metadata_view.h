#ifndef DOS_CLUSTER_CACHING_METADATA_VIEW_H_
#define DOS_CLUSTER_CACHING_METADATA_VIEW_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/metadata_view.h"
#include "common/lru_cache.h"

namespace dos {

// A MetadataView decorator that caches object-location lookups in an LRU (spec
// Milestone 9). Reads hit the cache; mutations keep it coherent — RegisterObject
// refreshes the entry and RemoveObject invalidates it — so a cached lookup never
// returns a stale replica set or version. Placement and List pass through
// (membership-dependent / rarely hot).
//
// Thread-safe: the cache is guarded by an internal mutex.
class CachingMetadataView : public MetadataView {
public:
  CachingMetadataView(std::shared_ptr<MetadataView> inner, std::size_t capacity)
      : inner_(std::move(inner)), cache_(capacity) {}

  std::vector<std::string> PlacementFor(std::string_view key, std::size_t rf) override {
    return inner_->PlacementFor(key, rf);
  }

  StatusOr<ObjectLocation> LookupObject(std::string_view key) override {
    const std::string k(key);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (auto hit = cache_.Get(k)) {
        return *hit;
      }
    }
    auto result = inner_->LookupObject(key);
    if (result.ok()) {
      std::lock_guard<std::mutex> lock(mu_);
      cache_.Put(k, result.value());
    }
    return result;
  }

  Status RegisterObject(const ObjectLocation& loc) override {
    Status s = inner_->RegisterObject(loc);
    if (s.ok()) {
      std::lock_guard<std::mutex> lock(mu_);
      cache_.Put(loc.key, loc); // keep the cache coherent with the write
    }
    return s;
  }

  Status RemoveObject(std::string_view key) override {
    Status s = inner_->RemoveObject(key);
    std::lock_guard<std::mutex> lock(mu_);
    cache_.Erase(std::string(key)); // invalidate regardless of remove outcome
    return s;
  }

  StatusOr<std::vector<ObjectLocation>> ListObjects(std::string_view prefix) override {
    return inner_->ListObjects(prefix);
  }

  double hit_rate() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.hit_rate();
  }
  uint64_t hits() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.hits();
  }
  uint64_t misses() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.misses();
  }

private:
  std::shared_ptr<MetadataView> inner_;
  mutable std::mutex mu_;
  LruCache<std::string, ObjectLocation> cache_;
};

} // namespace dos

#endif // DOS_CLUSTER_CACHING_METADATA_VIEW_H_
