#include "storage/local_object_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "common/status.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

class VersioningTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_ver_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(root_);
    auto s = LocalObjectStore::Open(root_);
    ASSERT_TRUE(s.ok()) << s.status().ToString();
    store_ = std::move(s).value();
  }
  void TearDown() override {
    store_.reset();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  fs::path root_;
  std::unique_ptr<LocalObjectStore> store_;
};

// expected_version = 0 means "create only": succeeds once, then a stale writer
// with the same expectation is rejected deterministically.
TEST_F(VersioningTest, ConditionalCreateThenStaleConflict) {
  auto v1 = store_->PutConditional("k", "v1", /*expected=*/0, /*rid=*/"");
  ASSERT_TRUE(v1.ok()) << v1.status().ToString();
  EXPECT_EQ(v1.value().version, 1u);

  auto stale = store_->PutConditional("k", "v2", /*expected=*/0, "");
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.status().code(), StatusCode::kConflict);

  // Correct expectation advances the version.
  auto v2 = store_->PutConditional("k", "v2", /*expected=*/1, "");
  ASSERT_TRUE(v2.ok());
  EXPECT_EQ(v2.value().version, 2u);
  EXPECT_EQ(store_->Get("k").value(), "v2");
}

TEST_F(VersioningTest, ConditionalWrongVersionConflicts) {
  ASSERT_TRUE(store_->PutConditional("k", "v1", 0, "").ok()); // version 1
  auto bad = store_->PutConditional("k", "v2", /*expected=*/5, "");
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.status().code(), StatusCode::kConflict);
  // The committed value is unchanged.
  EXPECT_EQ(store_->Get("k").value(), "v1");
  EXPECT_EQ(store_->Head("k").value().version, 1u);
}

// A retried PUT carrying the same request_id must not create a second version.
TEST_F(VersioningTest, IdempotentRetryReturnsSameVersion) {
  auto first = store_->PutConditional("k", "payload", /*expected=*/0, "req-1");
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(first.value().version, 1u);

  // Client timed out and retries the identical request.
  auto retry = store_->PutConditional("k", "payload", /*expected=*/0, "req-1");
  ASSERT_TRUE(retry.ok()) << retry.status().ToString();
  EXPECT_EQ(retry.value().version, 1u); // same version, no new write

  EXPECT_EQ(store_->Head("k").value().version, 1u);

  // A genuinely new writer with the stale expectation still conflicts.
  auto other = store_->PutConditional("k", "other", /*expected=*/0, "req-2");
  EXPECT_EQ(other.status().code(), StatusCode::kConflict);
}

// Concurrent writers racing on the same expected_version: exactly one wins.
TEST_F(VersioningTest, ConcurrentConditionalWritersExactlyOneWins) {
  constexpr int kWriters = 16;
  std::atomic<int> wins{0};
  std::atomic<int> conflicts{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kWriters; ++i) {
    threads.emplace_back([&, i] {
      auto r = store_->PutConditional("hot", "w" + std::to_string(i), /*expected=*/0,
                                      "rid-" + std::to_string(i));
      if (r.ok()) {
        wins.fetch_add(1);
      } else if (r.status().code() == StatusCode::kConflict) {
        conflicts.fetch_add(1);
      }
    });
  }
  for (auto& t : threads)
    t.join();

  EXPECT_EQ(wins.load(), 1);
  EXPECT_EQ(conflicts.load(), kWriters - 1);
  EXPECT_EQ(store_->Head("hot").value().version, 1u);
}

TEST_F(VersioningTest, RegularPutStillAutoVersions) {
  EXPECT_EQ(store_->Put("k", "a").value().version, 1u);
  EXPECT_EQ(store_->Put("k", "b").value().version, 2u);
}

} // namespace
} // namespace dos
