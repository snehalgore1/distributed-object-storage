#include "common/digest.h"

#include <gtest/gtest.h>

#include "common/sha256.h"

namespace dos {
namespace {

TEST(DigestTest, KeyDigestIsSha256OfKey) { EXPECT_EQ(KeyDigest("hello"), Sha256Hex("hello")); }

TEST(DigestTest, DeterministicForSameKey) {
  EXPECT_EQ(KeyDigest("some/key"), KeyDigest("some/key"));
}

TEST(DigestTest, DistinctKeysDistinctDigests) { EXPECT_NE(KeyDigest("a"), KeyDigest("b")); }

TEST(DigestTest, PhysicalPathFansOutTwoLevels) {
  const std::string digest = KeyDigest("greeting/hello.txt");
  auto path = PhysicalPath("/var/dos/data", digest);

  // Expected: /var/dos/data/<aa>/<bb>/<full-digest>
  EXPECT_EQ(path.filename().string(), digest);
  EXPECT_EQ(path.parent_path().filename().string(), digest.substr(2, 2));
  EXPECT_EQ(path.parent_path().parent_path().filename().string(), digest.substr(0, 2));
}

TEST(DigestTest, PhysicalPathUsesDigestNotKey) {
  // The logical key must never appear in the physical path.
  auto path = PhysicalPath("/data", KeyDigest("secret/key/name"));
  EXPECT_EQ(path.string().find("secret"), std::string::npos);
}

} // namespace
} // namespace dos
