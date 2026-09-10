#include "cluster/cluster_map.h"

#include <gtest/gtest.h>

#include "cluster/node_info.h"

namespace dos {
namespace {

NodeInfo MakeNode(std::string id, std::string addr) {
  NodeInfo n;
  n.id = std::move(id);
  n.address = std::move(addr);
  n.state = NodeState::kHealthy;
  return n;
}

TEST(ClusterMapTest, AddNodesAndQuery) {
  ClusterMap cm(64);
  cm.AddOrUpdateNode(MakeNode("node-a", "10.0.0.1:9000"));
  cm.AddOrUpdateNode(MakeNode("node-b", "10.0.0.2:9000"));
  EXPECT_EQ(cm.Size(), 2u);

  auto a = cm.GetNode("node-a");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->address, "10.0.0.1:9000");
  EXPECT_FALSE(cm.GetNode("ghost").has_value());
  EXPECT_EQ(cm.Nodes().size(), 2u);
}

TEST(ClusterMapTest, UpdateExistingNodeKeepsPlacement) {
  ClusterMap cm(150);
  cm.AddOrUpdateNode(MakeNode("node-a", "addr-1"));
  cm.AddOrUpdateNode(MakeNode("node-b", "addr-2"));
  cm.AddOrUpdateNode(MakeNode("node-c", "addr-3"));

  const std::string primary_before = cm.PrimaryFor("some-key");

  // Update node-a's address/state; placement must not shift.
  NodeInfo updated = MakeNode("node-a", "addr-1-new");
  updated.state = NodeState::kSuspect;
  cm.AddOrUpdateNode(updated);

  EXPECT_EQ(cm.Size(), 3u);
  EXPECT_EQ(cm.GetNode("node-a")->address, "addr-1-new");
  EXPECT_EQ(cm.GetNode("node-a")->state, NodeState::kSuspect);
  EXPECT_EQ(cm.PrimaryFor("some-key"), primary_before);
}

TEST(ClusterMapTest, PlacementSizeMatchesReplicationFactor) {
  ClusterMap cm(150);
  cm.AddOrUpdateNode(MakeNode("node-a", "a"));
  cm.AddOrUpdateNode(MakeNode("node-b", "b"));
  cm.AddOrUpdateNode(MakeNode("node-c", "c"));

  auto placement = cm.PlacementFor("obj", 3);
  EXPECT_EQ(placement.size(), 3u);
  EXPECT_EQ(placement.front(), cm.PrimaryFor("obj"));

  // RF larger than cluster size is clamped.
  EXPECT_EQ(cm.PlacementFor("obj", 9).size(), 3u);
}

TEST(ClusterMapTest, RemoveNode) {
  ClusterMap cm(64);
  cm.AddOrUpdateNode(MakeNode("node-a", "a"));
  cm.AddOrUpdateNode(MakeNode("node-b", "b"));

  EXPECT_FALSE(cm.RemoveNode("ghost"));
  EXPECT_TRUE(cm.RemoveNode("node-a"));
  EXPECT_EQ(cm.Size(), 1u);
  EXPECT_FALSE(cm.GetNode("node-a").has_value());
  EXPECT_FALSE(cm.ring().Contains("node-a"));
}

TEST(ClusterMapTest, EmptyClusterHasNoPrimary) {
  ClusterMap cm(64);
  EXPECT_EQ(cm.PrimaryFor("k"), "");
  EXPECT_TRUE(cm.PlacementFor("k", 3).empty());
}

} // namespace
} // namespace dos
