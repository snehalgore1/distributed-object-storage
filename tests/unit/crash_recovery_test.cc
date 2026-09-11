#include "storage/local_object_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "common/digest.h"
#include "common/sha256.h"
#include "common/status.h"
#include "storage/wal.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

// These tests craft the exact on-disk state that a crash would leave at each
// point of the PUT lifecycle, then open the store and assert recovery behavior.
class CrashRecoveryTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_crash_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  // Writes the object payload to its content-addressed path, as an atomic
  // rename would have left it just before the metadata commit.
  void PlaceObjectBytes(const std::string& key, const std::string& bytes) {
    const fs::path p = PhysicalPath(root_ / "data", KeyDigest(key));
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  fs::path WalPath() const { return root_ / "wal" / "node.wal"; }

  fs::path root_;
};

// Crash after the object was renamed into place but before the metadata commit.
// Recovery must complete the commit from the WAL intent + durable bytes.
TEST_F(CrashRecoveryTest, CompletesCommitWhenBytesPresentButMetadataMissing) {
  const std::string key = "obj";
  const std::string bytes = "durable-payload";
  PlaceObjectBytes(key, bytes);
  {
    auto wal = Wal::Open(WalPath());
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(
        (*wal.value()).AppendBegin(1, WalOp::kPut, key, 1, Sha256Hex(bytes), bytes.size()).ok());
    // No COMMIT: the process "crashed" before finishing.
  }

  auto store_or = LocalObjectStore::Open(root_);
  ASSERT_TRUE(store_or.ok()) << store_or.status().ToString();
  auto store = std::move(store_or).value();

  auto got = store->Get(key);
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ(got.value(), bytes);
  EXPECT_EQ(store->Head(key).value().version, 1u);
}

// Crash after the WAL intent but before the payload was written. Recovery must
// discard it: the object never became a committed version.
TEST_F(CrashRecoveryTest, DiscardsIncompleteWhenBytesMissing) {
  const std::string key = "ghost";
  {
    auto wal = Wal::Open(WalPath());
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE((*wal.value()).AppendBegin(1, WalOp::kPut, key, 1, "deadbeef", 10).ok());
  }
  // No object bytes on disk.

  auto store_or = LocalObjectStore::Open(root_);
  ASSERT_TRUE(store_or.ok());
  auto store = std::move(store_or).value();

  EXPECT_EQ(store->Get(key).status().code(), StatusCode::kNotFound);
}

// A payload present on disk but whose bytes don't match the WAL checksum (a torn
// write) must not be committed.
TEST_F(CrashRecoveryTest, DiscardsWhenBytesCorrupt) {
  const std::string key = "torn";
  PlaceObjectBytes(key, "partially-written-gar"); // not what the checksum expects
  {
    auto wal = Wal::Open(WalPath());
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(
        (*wal.value()).AppendBegin(1, WalOp::kPut, key, 1, Sha256Hex("the-real-bytes"), 14).ok());
  }

  auto store_or = LocalObjectStore::Open(root_);
  ASSERT_TRUE(store_or.ok());
  auto store = std::move(store_or).value();

  EXPECT_EQ(store->Get(key).status().code(), StatusCode::kNotFound);
}

// A fully committed object (begin + commit) survives restart, and recovery is a
// no-op for it.
TEST_F(CrashRecoveryTest, CommittedObjectSurvivesRestart) {
  {
    auto store = LocalObjectStore::Open(root_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store.value()).Put("k", "value").ok());
  }
  auto store2 = LocalObjectStore::Open(root_);
  ASSERT_TRUE(store2.ok());
  EXPECT_EQ((*store2.value()).Get("k").value(), "value");
}

// Recovery is idempotent: opening twice yields the same committed state.
TEST_F(CrashRecoveryTest, RecoveryIsIdempotent) {
  const std::string key = "obj";
  const std::string bytes = "payload";
  PlaceObjectBytes(key, bytes);
  {
    auto wal = Wal::Open(WalPath());
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(
        (*wal.value()).AppendBegin(1, WalOp::kPut, key, 1, Sha256Hex(bytes), bytes.size()).ok());
  }

  {
    auto store = LocalObjectStore::Open(root_); // first recovery completes it
    ASSERT_TRUE(store.ok());
    EXPECT_EQ((*store.value()).Get(key).value(), bytes);
  }
  auto store2 = LocalObjectStore::Open(root_); // WAL now checkpointed; still fine
  ASSERT_TRUE(store2.ok());
  EXPECT_EQ((*store2.value()).Get(key).value(), bytes);
}

// A torn record at the tail of the WAL is ignored; intact intents still recover.
TEST_F(CrashRecoveryTest, TornWalTailDoesNotBlockRecovery) {
  const std::string key = "obj";
  const std::string bytes = "payload";
  PlaceObjectBytes(key, bytes);
  {
    auto wal = Wal::Open(WalPath());
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(
        (*wal.value()).AppendBegin(1, WalOp::kPut, key, 1, Sha256Hex(bytes), bytes.size()).ok());
  }
  {
    std::ofstream out(WalPath(), std::ios::binary | std::ios::app);
    const char junk[] = {0x00, 0x00, 0x01, 0x00, 0x7f}; // partial frame
    out.write(junk, sizeof(junk));
  }

  auto store_or = LocalObjectStore::Open(root_);
  ASSERT_TRUE(store_or.ok());
  EXPECT_EQ((*store_or.value()).Get(key).value(), bytes);
}

// A logged-but-uncommitted DELETE is applied on recovery.
TEST_F(CrashRecoveryTest, RecoversLoggedDelete) {
  {
    auto store = LocalObjectStore::Open(root_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE((*store.value()).Put("k", "value").ok());
  }
  // Append a delete intent (no commit), as a crash mid-delete would leave.
  {
    auto wal = Wal::Open(WalPath());
    ASSERT_TRUE(wal.ok());
    const uint64_t seq = (*wal.value()).NextSeq();
    ASSERT_TRUE((*wal.value()).AppendBegin(seq, WalOp::kDelete, "k", 0, "", 0).ok());
  }

  auto store2 = LocalObjectStore::Open(root_);
  ASSERT_TRUE(store2.ok());
  EXPECT_EQ((*store2.value()).Get("k").status().code(), StatusCode::kNotFound);
}

} // namespace
} // namespace dos
