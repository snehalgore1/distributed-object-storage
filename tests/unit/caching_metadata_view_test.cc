#include "cluster/caching_metadata_view.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "cluster/metadata_view.h"
#include "common/status.h"

namespace dos {
namespace {

// A minimal MetadataView that counts LookupObject calls, so we can prove the
// cache actually absorbs reads.
class CountingMetadataView : public MetadataView {
public:
  int lookups = 0;

  std::vector<std::string> PlacementFor(std::string_view, std::size_t) override {
    return {"n1", "n2", "n3"};
  }
  Status RegisterObject(const ObjectLocation& loc) override {
    store_[loc.key] = loc;
    return Status::Ok();
  }
  StatusOr<ObjectLocation> LookupObject(std::string_view key) override {
    ++lookups;
    auto it = store_.find(std::string(key));
    if (it == store_.end() || it->second.deleted) {
      return Status::NotFound("no metadata");
    }
    return it->second;
  }
  Status RemoveObject(std::string_view key) override {
    auto it = store_.find(std::string(key));
    if (it == store_.end() || it->second.deleted) {
      return Status::NotFound("absent");
    }
    it->second.deleted = true;
    return Status::Ok();
  }
  StatusOr<std::vector<ObjectLocation>> ListObjects(std::string_view) override {
    return std::vector<ObjectLocation>{};
  }

private:
  std::map<std::string, ObjectLocation> store_;
};

ObjectLocation Loc(const std::string& key, uint64_t version) {
  ObjectLocation l;
  l.key = key;
  l.version = version;
  l.replicas = {"n1", "n2"};
  return l;
}

TEST(CachingMetadataViewTest, SecondLookupIsServedFromCache) {
  auto inner = std::make_shared<CountingMetadataView>();
  inner->RegisterObject(Loc("k", 1)); // seed inner directly (bypassing cache)
  CachingMetadataView cache(inner, 16);

  ASSERT_TRUE(cache.LookupObject("k").ok()); // miss -> inner
  ASSERT_TRUE(cache.LookupObject("k").ok()); // hit -> cache
  ASSERT_TRUE(cache.LookupObject("k").ok()); // hit -> cache
  EXPECT_EQ(inner->lookups, 1);              // inner consulted only once
  EXPECT_EQ(cache.hits(), 2u);
  EXPECT_EQ(cache.misses(), 1u);
}

TEST(CachingMetadataViewTest, RegisterPopulatesCache) {
  auto inner = std::make_shared<CountingMetadataView>();
  CachingMetadataView cache(inner, 16);

  ASSERT_TRUE(cache.RegisterObject(Loc("k", 1)).ok());
  auto got = cache.LookupObject("k"); // should be a cache hit, no inner call
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value().version, 1u);
  EXPECT_EQ(inner->lookups, 0);
}

TEST(CachingMetadataViewTest, RegisterRefreshesStaleEntry) {
  auto inner = std::make_shared<CountingMetadataView>();
  CachingMetadataView cache(inner, 16);
  ASSERT_TRUE(cache.RegisterObject(Loc("k", 1)).ok());
  ASSERT_EQ(cache.LookupObject("k").value().version, 1u);

  ASSERT_TRUE(cache.RegisterObject(Loc("k", 2)).ok()); // overwrite
  auto got = cache.LookupObject("k");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value().version, 2u); // cache reflects the new version
}

TEST(CachingMetadataViewTest, RemoveInvalidatesCache) {
  auto inner = std::make_shared<CountingMetadataView>();
  CachingMetadataView cache(inner, 16);
  ASSERT_TRUE(cache.RegisterObject(Loc("k", 1)).ok());
  ASSERT_TRUE(cache.LookupObject("k").ok());

  ASSERT_TRUE(cache.RemoveObject("k").ok());
  auto got = cache.LookupObject("k");
  EXPECT_EQ(got.status().code(), StatusCode::kNotFound); // not served stale from cache
}

} // namespace
} // namespace dos
