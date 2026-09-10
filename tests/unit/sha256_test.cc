#include "common/sha256.h"

#include <gtest/gtest.h>

#include <string>

namespace dos {
namespace {

// FIPS 180-4 / NIST known-answer vectors.
TEST(Sha256Test, EmptyInput) {
  EXPECT_EQ(Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, Abc) {
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, LongerMessage) {
  EXPECT_EQ(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, StreamingMatchesOneShot) {
  const std::string full = "the quick brown fox jumps over the lazy dog";
  Sha256 h;
  h.Update(full.substr(0, 10));
  h.Update(full.substr(10));
  EXPECT_EQ(h.HexDigest(), Sha256Hex(full));
}

TEST(Sha256Test, CrossesBlockBoundary) {
  // Larger than one 64-byte block to exercise multi-block processing.
  std::string big(1000, 'x');
  // Known value computed independently (shasum -a 256) for 1000 'x' bytes.
  EXPECT_EQ(Sha256Hex(big), "44f8354494a5ba03ba1792a8d3e9c534c47a9181980fde7a3f44b06ef2ae7c7f");
}

} // namespace
} // namespace dos
