// Unit tests for the disk-persisted Raft storage (spec Milestone 15): hard
// state and log survive reopen, and a torn tail from a crash mid-append is
// detected and dropped rather than corrupting recovery.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "consensus/raft_storage.h"

namespace dos::consensus {
namespace {

namespace fs = std::filesystem;

class RaftStorageTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("dos_raftstore_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path dir_;
};

TEST_F(RaftStorageTest, HardStateSurvivesReopen) {
  {
    auto s = RaftStorage::Open(dir_);
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(s.value()->SaveHardState(HardState{7, "node-b"}).ok());
  }
  auto s2 = RaftStorage::Open(dir_);
  ASSERT_TRUE(s2.ok());
  EXPECT_EQ(s2.value()->LoadedHardState().current_term, 7u);
  EXPECT_EQ(s2.value()->LoadedHardState().voted_for, "node-b");
}

TEST_F(RaftStorageTest, LogAppendAndReopen) {
  {
    auto s = RaftStorage::Open(dir_);
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(s.value()->AppendEntry(LogEntry{1, 1, "alpha"}).ok());
    ASSERT_TRUE(s.value()->AppendEntry(LogEntry{1, 2, "beta"}).ok());
    ASSERT_TRUE(s.value()->AppendEntry(LogEntry{2, 3, "gamma"}).ok());
  }
  auto s2 = RaftStorage::Open(dir_);
  ASSERT_TRUE(s2.ok());
  const auto& log = s2.value()->LoadedLog();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0].term, 1u);
  EXPECT_EQ(log[0].index, 1u);
  EXPECT_EQ(log[0].command, "alpha");
  EXPECT_EQ(log[2].term, 2u);
  EXPECT_EQ(log[2].command, "gamma");
}

TEST_F(RaftStorageTest, RewriteReplacesLog) {
  auto s = RaftStorage::Open(dir_);
  ASSERT_TRUE(s.ok());
  ASSERT_TRUE(s.value()->AppendEntry(LogEntry{1, 1, "a"}).ok());
  ASSERT_TRUE(s.value()->AppendEntry(LogEntry{1, 2, "b"}).ok());
  ASSERT_TRUE(s.value()->AppendEntry(LogEntry{1, 3, "c"}).ok());

  // Drop the conflicting suffix (keep only index 1), then append a fresh entry.
  ASSERT_TRUE(s.value()->RewriteLog({LogEntry{1, 1, "a"}}).ok());
  ASSERT_TRUE(s.value()->AppendEntry(LogEntry{2, 2, "b2"}).ok());

  auto s2 = RaftStorage::Open(dir_);
  ASSERT_TRUE(s2.ok());
  const auto& log = s2.value()->LoadedLog();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0].command, "a");
  EXPECT_EQ(log[1].term, 2u);
  EXPECT_EQ(log[1].command, "b2");
}

TEST_F(RaftStorageTest, TornTailIsDropped) {
  {
    auto s = RaftStorage::Open(dir_);
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(s.value()->AppendEntry(LogEntry{1, 1, "intact"}).ok());
    ASSERT_TRUE(s.value()->AppendEntry(LogEntry{1, 2, "also-intact"}).ok());
  }
  // Simulate a crash mid-append: append a few garbage bytes to the log file.
  {
    std::ofstream out(dir_ / "raft-log", std::ios::binary | std::ios::app);
    const char junk[] = {0x00, 0x00, 0x00, 0x40, 0x11, 0x22}; // bogus length + partial body
    out.write(junk, sizeof(junk));
  }
  auto s2 = RaftStorage::Open(dir_);
  ASSERT_TRUE(s2.ok());
  const auto& log = s2.value()->LoadedLog();
  ASSERT_EQ(log.size(), 2u); // torn tail ignored, intact prefix preserved
  EXPECT_EQ(log[1].command, "also-intact");
}

TEST_F(RaftStorageTest, FreshDirectoryIsEmpty) {
  auto s = RaftStorage::Open(dir_);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(s.value()->LoadedHardState().current_term, 0u);
  EXPECT_TRUE(s.value()->LoadedHardState().voted_for.empty());
  EXPECT_TRUE(s.value()->LoadedLog().empty());
}

} // namespace
} // namespace dos::consensus
