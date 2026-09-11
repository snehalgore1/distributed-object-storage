#include "cluster/metadata_repository.h"

#include <gtest/gtest.h>

#include "cluster/node_info.h"
#include "common/status.h"

namespace dos {
namespace {

// MetadataRepository owns a mutex, so it is neither copyable nor movable; add
// nodes into a caller-owned instance rather than returning one by value.
void AddThreeNodes(MetadataRepository& repo) {
  for (const auto& id : {"node-a", "node-b", "node-c"}) {
    NodeInfo n;
    n.id = id;
    n.address = std::string(id) + ":9000";
    repo.AddNode(n);
  }
}

TEST(MetadataRepositoryTest, PlacementReturnsReplicaSet) {
  MetadataRepository repo(150);
  AddThreeNodes(repo);
  auto placement = repo.PlacementFor("some/key", 3);
  EXPECT_EQ(placement.size(), 3u);
  EXPECT_EQ(repo.PlacementFor("some/key", 5).size(), 3u); // clamped to cluster size
}

TEST(MetadataRepositoryTest, RegisterThenLookup) {
  MetadataRepository repo(150);
  AddThreeNodes(repo);
  ObjectLocation loc;
  loc.key = "k";
  loc.version = 7;
  loc.checksum = "abc";
  loc.size = 123;
  loc.replicas = {"node-a", "node-b"};
  ASSERT_TRUE(repo.RegisterObject(loc).ok());

  auto got = repo.LookupObject("k");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value().version, 7u);
  EXPECT_EQ(got.value().checksum, "abc");
  EXPECT_EQ(got.value().replicas.size(), 2u);
}

TEST(MetadataRepositoryTest, LookupMissingIsNotFound) {
  MetadataRepository repo(150);
  AddThreeNodes(repo);
  EXPECT_EQ(repo.LookupObject("ghost").status().code(), StatusCode::kNotFound);
}

TEST(MetadataRepositoryTest, RemoveTombstonesAndHides) {
  MetadataRepository repo(150);
  AddThreeNodes(repo);
  ObjectLocation loc;
  loc.key = "k";
  loc.version = 1;
  loc.replicas = {"node-a"};
  ASSERT_TRUE(repo.RegisterObject(loc).ok());

  ASSERT_TRUE(repo.RemoveObject("k").ok());
  EXPECT_EQ(repo.LookupObject("k").status().code(), StatusCode::kNotFound);
  EXPECT_EQ(repo.RemoveObject("k").code(), StatusCode::kNotFound); // already gone
}

TEST(MetadataRepositoryTest, ListReflectsPrefixAndExcludesTombstones) {
  MetadataRepository repo(150);
  AddThreeNodes(repo);
  for (const std::string key : {"a/1", "a/2", "b/1"}) {
    ObjectLocation loc;
    loc.key = key;
    loc.version = 1;
    loc.replicas = {"node-a"};
    ASSERT_TRUE(repo.RegisterObject(loc).ok());
  }
  ASSERT_TRUE(repo.RemoveObject("a/2").ok());

  EXPECT_EQ(repo.ListObjects("").value().size(), 2u);   // a/1, b/1
  EXPECT_EQ(repo.ListObjects("a/").value().size(), 1u); // a/1 (a/2 tombstoned)
}

TEST(MetadataRepositoryTest, RegisterOverwritesPriorVersion) {
  MetadataRepository repo(150);
  AddThreeNodes(repo);
  ObjectLocation v1;
  v1.key = "k";
  v1.version = 1;
  v1.replicas = {"node-a"};
  ASSERT_TRUE(repo.RegisterObject(v1).ok());

  ObjectLocation v2 = v1;
  v2.version = 2;
  v2.replicas = {"node-a", "node-b", "node-c"};
  ASSERT_TRUE(repo.RegisterObject(v2).ok());

  auto got = repo.LookupObject("k");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value().version, 2u);
  EXPECT_EQ(got.value().replicas.size(), 3u);
}

} // namespace
} // namespace dos
