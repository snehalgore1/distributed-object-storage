#include "storage/wal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace dos {
namespace {

namespace fs = std::filesystem;

class WalTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("dos_wal_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    path_ = dir_ / "node.wal";
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  fs::path dir_;
  fs::path path_;
};

TEST_F(WalTest, AppendAndReplayRoundTrips) {
  {
    auto wal = Wal::Open(path_);
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE((*wal.value()).AppendBegin(1, WalOp::kPut, "k1", 1, "abc", 3).ok());
    ASSERT_TRUE((*wal.value()).AppendCommit(1).ok());
    ASSERT_TRUE((*wal.value()).AppendBegin(2, WalOp::kDelete, "k2", 5, "", 0).ok());
  }
  auto recs = Wal::Replay(path_);
  ASSERT_TRUE(recs.ok());
  ASSERT_EQ(recs.value().size(), 3u);

  EXPECT_EQ(recs.value()[0].seq, 1u);
  EXPECT_FALSE(recs.value()[0].is_commit);
  EXPECT_EQ(recs.value()[0].op, WalOp::kPut);
  EXPECT_EQ(recs.value()[0].key, "k1");
  EXPECT_EQ(recs.value()[0].version, 1u);
  EXPECT_EQ(recs.value()[0].checksum, "abc");
  EXPECT_EQ(recs.value()[0].size, 3u);

  EXPECT_TRUE(recs.value()[1].is_commit);
  EXPECT_EQ(recs.value()[1].seq, 1u);

  EXPECT_EQ(recs.value()[2].op, WalOp::kDelete);
  EXPECT_EQ(recs.value()[2].key, "k2");
}

TEST_F(WalTest, ReplayOfMissingFileIsEmpty) {
  auto recs = Wal::Replay(dir_ / "does-not-exist.wal");
  ASSERT_TRUE(recs.ok());
  EXPECT_TRUE(recs.value().empty());
}

TEST_F(WalTest, TornTailRecordIsIgnored) {
  {
    auto wal = Wal::Open(path_);
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE((*wal.value()).AppendBegin(1, WalOp::kPut, "good", 1, "cs", 4).ok());
    ASSERT_TRUE((*wal.value()).AppendCommit(1).ok());
  }
  // Simulate a crash mid-append: garbage/partial bytes at the end.
  {
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    const char partial[] = {0x00, 0x00, 0x00, 0x40, 0x11, 0x22}; // len=64 but truncated
    out.write(partial, sizeof(partial));
  }
  auto recs = Wal::Replay(path_);
  ASSERT_TRUE(recs.ok());
  ASSERT_EQ(recs.value().size(), 2u); // the two intact records survive
  EXPECT_EQ(recs.value()[0].key, "good");
}

TEST_F(WalTest, TruncateResetsLog) {
  auto wal = Wal::Open(path_);
  ASSERT_TRUE(wal.ok());
  ASSERT_TRUE((*wal.value()).AppendBegin(1, WalOp::kPut, "k", 1, "cs", 1).ok());
  ASSERT_TRUE((*wal.value()).Truncate().ok());

  auto recs = Wal::Replay(path_);
  ASSERT_TRUE(recs.ok());
  EXPECT_TRUE(recs.value().empty());
}

TEST_F(WalTest, SeqContinuesAcrossReopen) {
  {
    auto wal = Wal::Open(path_);
    ASSERT_TRUE(wal.ok());
    EXPECT_EQ((*wal.value()).NextSeq(), 1u);
    ASSERT_TRUE((*wal.value()).AppendBegin(1, WalOp::kPut, "k", 1, "cs", 1).ok());
  }
  auto wal2 = Wal::Open(path_);
  ASSERT_TRUE(wal2.ok());
  EXPECT_EQ((*wal2.value()).NextSeq(), 2u); // continues past on-disk max seq
}

} // namespace
} // namespace dos
