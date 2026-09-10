#include "common/hash.h"

#include <gtest/gtest.h>

namespace dos {
namespace {

// Hash64 is the top 8 bytes of SHA-256, big-endian. Pinned to known SHA-256
// vectors so a regression in the derivation is caught immediately.
TEST(Hash64Test, MatchesSha256TopBytes) {
  EXPECT_EQ(Hash64(""), 0xe3b0c44298fc1c14ULL);
  EXPECT_EQ(Hash64("abc"), 0xba7816bf8f01cfeaULL);
}

TEST(Hash64Test, Deterministic) { EXPECT_EQ(Hash64("some/object/key"), Hash64("some/object/key")); }

TEST(Hash64Test, DistinctInputsDifferentHashes) {
  EXPECT_NE(Hash64("node-a#0"), Hash64("node-a#1"));
  EXPECT_NE(Hash64("node-a#0"), Hash64("node-b#0"));
}

} // namespace
} // namespace dos
