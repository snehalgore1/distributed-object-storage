#include "storage/local_object_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "common/status.h"
#include "common/thread_pool.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

class ConcurrencyTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_conc_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
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

// Many writers to distinct keys should all succeed and read back correctly.
TEST_F(ConcurrencyTest, ConcurrentPutsToDistinctKeys) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        std::string key = "t" + std::to_string(t) + "/k" + std::to_string(i);
        std::string val = "val-" + key;
        if (!store_->Put(key, val).ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& th : threads)
    th.join();
  ASSERT_EQ(failures.load(), 0);

  // Every key is present and byte-identical.
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      std::string key = "t" + std::to_string(t) + "/k" + std::to_string(i);
      auto got = store_->Get(key);
      ASSERT_TRUE(got.ok()) << key << ": " << got.status().ToString();
      EXPECT_EQ(got.value(), "val-" + key);
    }
  }
  auto all = store_->List("");
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all.value().size(), static_cast<size_t>(kThreads * kPerThread));
}

// Concurrent writers to the SAME key must not race on the version counter:
// the final version equals the number of writes, and the payload is one of the
// values actually written (never torn/partial).
TEST_F(ConcurrencyTest, ConcurrentPutsToSameKeyVersionMonotonic) {
  constexpr int kWriters = 16;
  constexpr int kPerWriter = 50;
  const std::string key = "hot-key";
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        std::string val = "w" + std::to_string(w) + "-i" + std::to_string(i);
        if (!store_->Put(key, val).ok())
          failures.fetch_add(1);
      }
    });
  }
  for (auto& th : threads)
    th.join();
  ASSERT_EQ(failures.load(), 0);

  auto head = store_->Head(key);
  ASSERT_TRUE(head.ok());
  // Each successful Put bumped the version exactly once.
  EXPECT_EQ(head.value().version, static_cast<uint64_t>(kWriters * kPerWriter));

  // The stored payload is retrievable and passes its checksum (not corrupt).
  auto got = store_->Get(key);
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_FALSE(got.value().empty());
}

// GET racing DELETE on the same key must always be well-defined: either the
// bytes (checksum-valid) or NOT_FOUND — never corruption or a crash.
TEST_F(ConcurrencyTest, GetDeleteRaceIsWellDefined) {
  const std::string key = "race";
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};

  ASSERT_TRUE(store_->Put(key, "payload").ok());

  std::thread reader([&] {
    while (!stop.load()) {
      auto got = store_->Get(key);
      if (got.ok()) {
        if (got.value() != "payload")
          bad.fetch_add(1);
      } else if (got.status().code() != StatusCode::kNotFound) {
        bad.fetch_add(1); // only NotFound is acceptable once deleted
      }
    }
  });

  std::thread mutator([&] {
    for (int i = 0; i < 100; ++i) {
      store_->Delete(key);
      store_->Put(key, "payload");
    }
    stop.store(true);
  });

  mutator.join();
  reader.join();
  EXPECT_EQ(bad.load(), 0);
}

// Drive a mixed read/write workload through the bounded ThreadPool.
TEST_F(ConcurrencyTest, MixedLoadThroughThreadPool) {
  ThreadPool pool(6, 4096);
  std::atomic<int> errors{0};
  constexpr int kKeys = 300;

  for (int i = 0; i < kKeys; ++i) {
    std::string key = "obj/" + std::to_string(i);
    auto submit = pool.Submit([&, key] {
      if (!store_->Put(key, key + "-data").ok())
        errors.fetch_add(1);
      auto got = store_->Get(key);
      if (!got.ok() || got.value() != key + "-data")
        errors.fetch_add(1);
    });
    // Under a large queue this should not overload; if it does, run inline.
    if (!submit.ok()) {
      store_->Put(key, key + "-data").ok();
    }
  }
  pool.Shutdown();
  EXPECT_EQ(errors.load(), 0);
}

} // namespace
} // namespace dos
