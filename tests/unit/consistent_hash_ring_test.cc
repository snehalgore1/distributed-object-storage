#include "cluster/consistent_hash_ring.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace dos {
namespace {

std::string Key(int i) { return "object-key-" + std::to_string(i); }

TEST(ConsistentHashRingTest, EmptyRing) {
  ConsistentHashRing ring(64);
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(ring.LookupPrimary("k"), "");
  EXPECT_TRUE(ring.LookupReplicas("k", 3).empty());
}

TEST(ConsistentHashRingTest, AddIsIdempotentAndCountsVirtualNodes) {
  ConsistentHashRing ring(100);
  ring.AddNode("node-a");
  ring.AddNode("node-a"); // no-op
  EXPECT_EQ(ring.NumPhysicalNodes(), 1u);
  EXPECT_EQ(ring.NumVirtualNodes(), 100u);
  EXPECT_TRUE(ring.Contains("node-a"));
}

TEST(ConsistentHashRingTest, PlacementIsDeterministicAcrossInsertionOrder) {
  ConsistentHashRing a(150);
  a.AddNode("node-a");
  a.AddNode("node-b");
  a.AddNode("node-c");

  ConsistentHashRing b(150);
  b.AddNode("node-c"); // different insertion order
  b.AddNode("node-a");
  b.AddNode("node-b");

  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(a.LookupPrimary(Key(i)), b.LookupPrimary(Key(i))) << "key " << i;
  }
}

TEST(ConsistentHashRingTest, ReplicasAreDistinctAndPrimaryFirst) {
  ConsistentHashRing ring(150);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");

  for (int i = 0; i < 500; ++i) {
    auto replicas = ring.LookupReplicas(Key(i), 3);
    ASSERT_EQ(replicas.size(), 3u);
    EXPECT_EQ(replicas.front(), ring.LookupPrimary(Key(i)));
    // All distinct.
    std::vector<std::string> sorted = replicas;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end());
  }
}

TEST(ConsistentHashRingTest, ReplicaCountClampedToPhysicalNodes) {
  ConsistentHashRing ring(150);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  auto replicas = ring.LookupReplicas("anything", 5);
  EXPECT_EQ(replicas.size(), 2u); // only two physical nodes exist
}

// With enough virtual nodes, 100k keys spread across 3 nodes fairly evenly.
TEST(ConsistentHashRingTest, DistributionIsBalanced) {
  ConsistentHashRing ring(200);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");

  constexpr int kKeys = 100000;
  std::map<std::string, int> counts;
  for (int i = 0; i < kKeys; ++i) {
    counts[ring.LookupPrimary(Key(i))]++;
  }
  ASSERT_EQ(counts.size(), 3u);
  for (const auto& [node, n] : counts) {
    const double share = static_cast<double>(n) / kKeys;
    // Ideal is 0.333; allow generous slack for hashing variance.
    EXPECT_GT(share, 0.28) << node;
    EXPECT_LT(share, 0.39) << node;
  }
}

// Adding a node moves only keys that land on the new node, and only ~1/N of
// them — the whole point of consistent hashing versus modulo.
TEST(ConsistentHashRingTest, AddingNodeMovesBoundedKeysToNewNode) {
  ConsistentHashRing ring(200);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");

  constexpr int kKeys = 100000;
  std::vector<std::string> before(kKeys);
  for (int i = 0; i < kKeys; ++i) {
    before[static_cast<size_t>(i)] = ring.LookupPrimary(Key(i));
  }

  ring.AddNode("node-d");

  int moved = 0;
  for (int i = 0; i < kKeys; ++i) {
    const std::string now = ring.LookupPrimary(Key(i));
    if (now != before[static_cast<size_t>(i)]) {
      ++moved;
      // Every moved key must now belong to the newly added node.
      EXPECT_EQ(now, "node-d") << "key " << i;
    }
  }
  const double frac = static_cast<double>(moved) / kKeys;
  // Expect roughly 1/4; bound loosely but well under modulo's ~3/4.
  EXPECT_GT(frac, 0.15);
  EXPECT_LT(frac, 0.40);
}

// Direct comparison: consistent hashing moves far fewer keys than modulo when
// the cluster grows from 3 to 4 nodes (spec benchmark experiment C).
TEST(ConsistentHashRingTest, MovesFarFewerKeysThanModulo) {
  const std::vector<std::string> nodes3 = {"node-a", "node-b", "node-c"};
  const std::vector<std::string> nodes4 = {"node-a", "node-b", "node-c", "node-d"};

  ConsistentHashRing r3(200);
  for (const auto& n : nodes3)
    r3.AddNode(n);
  ConsistentHashRing r4(200);
  for (const auto& n : nodes4)
    r4.AddNode(n);

  constexpr int kKeys = 100000;
  int consistent_moved = 0;
  int modulo_moved = 0;
  for (int i = 0; i < kKeys; ++i) {
    const std::string k = Key(i);
    if (r3.LookupPrimary(k) != r4.LookupPrimary(k))
      ++consistent_moved;

    const uint64_t h = std::hash<std::string>{}(k);
    if (nodes3[h % 3] != nodes4[h % 4])
      ++modulo_moved;
  }
  // Modulo reshuffles most keys; consistent hashing moves a small fraction.
  EXPECT_LT(consistent_moved, modulo_moved);
  EXPECT_LT(static_cast<double>(consistent_moved) / kKeys, 0.40);
  EXPECT_GT(static_cast<double>(modulo_moved) / kKeys, 0.60);
}

TEST(ConsistentHashRingTest, RemoveNodeRedistributesOnlyItsKeys) {
  ConsistentHashRing ring(200);
  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");

  constexpr int kKeys = 20000;
  std::vector<std::string> before(kKeys);
  for (int i = 0; i < kKeys; ++i) {
    before[static_cast<size_t>(i)] = ring.LookupPrimary(Key(i));
  }

  ring.RemoveNode("node-b");
  EXPECT_FALSE(ring.Contains("node-b"));

  for (int i = 0; i < kKeys; ++i) {
    const std::string now = ring.LookupPrimary(Key(i));
    EXPECT_NE(now, "node-b");
    if (before[static_cast<size_t>(i)] != "node-b") {
      // Keys not previously on node-b keep their owner.
      EXPECT_EQ(now, before[static_cast<size_t>(i)]) << "key " << i;
    }
  }
}

} // namespace
} // namespace dos
