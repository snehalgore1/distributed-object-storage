#include "cluster/failure_detector.h"

#include <gtest/gtest.h>

#include "cluster/node_info.h"

namespace dos {
namespace {

TEST(FailureDetectorTest, StartsHealthy) {
  FailureDetector fd(3);
  fd.AddNode("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kHealthy);
}

TEST(FailureDetectorTest, SingleMissIsSuspectNotDead) {
  FailureDetector fd(3);
  fd.AddNode("n1");
  fd.RecordFailure("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kSuspect); // one transient miss != dead
  EXPECT_EQ(fd.consecutive_misses("n1"), 1u);
}

TEST(FailureDetectorTest, ThresholdMissesMarkUnavailable) {
  FailureDetector fd(3);
  fd.AddNode("n1");
  fd.RecordFailure("n1");
  fd.RecordFailure("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kSuspect);
  fd.RecordFailure("n1"); // reaches threshold
  EXPECT_EQ(fd.GetState("n1"), NodeState::kUnavailable);
}

TEST(FailureDetectorTest, SuspectRecoversToHealthyOnSuccess) {
  FailureDetector fd(3);
  fd.AddNode("n1");
  fd.RecordFailure("n1");
  ASSERT_EQ(fd.GetState("n1"), NodeState::kSuspect);
  fd.RecordSuccess("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kHealthy);
  EXPECT_EQ(fd.consecutive_misses("n1"), 0u);
}

TEST(FailureDetectorTest, UnavailableComesBackAsRecoveringNotHealthy) {
  FailureDetector fd(2);
  fd.AddNode("n1");
  fd.RecordFailure("n1");
  fd.RecordFailure("n1");
  ASSERT_EQ(fd.GetState("n1"), NodeState::kUnavailable);

  // A returning node must not be trusted until repaired.
  fd.RecordSuccess("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kRecovering);
  fd.RecordSuccess("n1"); // healthy pings alone don't re-trust it
  EXPECT_EQ(fd.GetState("n1"), NodeState::kRecovering);

  fd.RecordRepairComplete("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kHealthy);
}

TEST(FailureDetectorTest, RecoveringNodeThatDiesAgainGoesUnavailable) {
  FailureDetector fd(2);
  fd.AddNode("n1");
  fd.RecordFailure("n1");
  fd.RecordFailure("n1");
  fd.RecordSuccess("n1");
  ASSERT_EQ(fd.GetState("n1"), NodeState::kRecovering);

  fd.RecordFailure("n1");
  fd.RecordFailure("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kUnavailable);
}

TEST(FailureDetectorTest, RepairCompleteOnlyAppliesWhileRecovering) {
  FailureDetector fd(3);
  fd.AddNode("n1");
  // Not recovering -> no-op.
  fd.RecordRepairComplete("n1");
  EXPECT_EQ(fd.GetState("n1"), NodeState::kHealthy);
}

} // namespace
} // namespace dos
