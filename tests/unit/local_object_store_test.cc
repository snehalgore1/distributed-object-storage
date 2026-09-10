#include "storage/local_object_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "common/digest.h"
#include "common/status.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

// Creates each store under a fresh unique temp directory that is removed when
// the fixture is torn down.
class LocalObjectStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("dos_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
             std::to_string(reinterpret_cast<uintptr_t>(this)));
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

  fs::path ObjectPathFor(std::string_view key) {
    return PhysicalPath(root_ / "data", KeyDigest(key));
  }

  fs::path root_;
  std::unique_ptr<LocalObjectStore> store_;
};

TEST_F(LocalObjectStoreTest, PutThenGetIsByteIdentical) {
  // Explicit length so the embedded NUL is part of the payload.
  const std::string data("arbitrary\0bytes\x01\x02 with nul", 26);
  auto put = store_->Put("k1", data);
  ASSERT_TRUE(put.ok()) << put.status().ToString();

  auto got = store_->Get("k1");
  ASSERT_TRUE(got.ok()) << got.status().ToString();
  EXPECT_EQ(got.value(), data);
}

TEST_F(LocalObjectStoreTest, EmptyObjectRoundTrips) {
  auto put = store_->Put("empty", "");
  ASSERT_TRUE(put.ok()) << put.status().ToString();
  EXPECT_EQ(put.value().size, 0u);

  auto got = store_->Get("empty");
  ASSERT_TRUE(got.ok());
  EXPECT_TRUE(got.value().empty());
}

TEST_F(LocalObjectStoreTest, LargeObjectRoundTrips) {
  std::string big(1u << 20, '\xab'); // ~1 MB
  ASSERT_TRUE(store_->Put("big", big).ok());
  auto got = store_->Get("big");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), big);
}

TEST_F(LocalObjectStoreTest, MissingKeyIsNotFound) {
  auto got = store_->Get("nope");
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.status().code(), StatusCode::kNotFound);

  auto head = store_->Head("nope");
  ASSERT_FALSE(head.ok());
  EXPECT_EQ(head.status().code(), StatusCode::kNotFound);
}

TEST_F(LocalObjectStoreTest, HeadReturnsMetadataWithoutPayload) {
  ASSERT_TRUE(store_->Put("k", "some-bytes").ok());
  auto head = store_->Head("k");
  ASSERT_TRUE(head.ok());
  EXPECT_EQ(head.value().key, "k");
  EXPECT_EQ(head.value().size, 10u);
  EXPECT_FALSE(head.value().checksum.empty());
  // Head returns ObjectMetadata, which structurally carries no payload field.
}

TEST_F(LocalObjectStoreTest, OverwriteBumpsVersionMonotonically) {
  auto v1 = store_->Put("k", "one");
  ASSERT_TRUE(v1.ok());
  EXPECT_EQ(v1.value().version, 1u);

  auto v2 = store_->Put("k", "two");
  ASSERT_TRUE(v2.ok());
  EXPECT_EQ(v2.value().version, 2u);

  auto got = store_->Get("k");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), "two");
}

TEST_F(LocalObjectStoreTest, CorruptedObjectIsDetected) {
  ASSERT_TRUE(store_->Put("k", "trusted-content").ok());

  // Corrupt the bytes on disk behind the store's back.
  const fs::path path = ObjectPathFor("k");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "tampered!";
  }

  auto got = store_->Get("k");
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.status().code(), StatusCode::kChecksumMismatch);
}

TEST_F(LocalObjectStoreTest, StrayTempFileIsNeverCommitted) {
  // Simulate a crash mid-PUT: a leftover file in the tmp staging area.
  const fs::path tmp = root_ / "data" / "tmp" / ".put-crash-leftover.tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out << "half-written garbage";
  }

  // The orphan is invisible to the store.
  auto got = store_->Get("anything");
  EXPECT_EQ(got.status().code(), StatusCode::kNotFound);

  auto list = store_->List("");
  ASSERT_TRUE(list.ok());
  EXPECT_TRUE(list.value().empty());
}

TEST_F(LocalObjectStoreTest, DeleteTombstonesAndHidesObject) {
  ASSERT_TRUE(store_->Put("k", "bytes").ok());
  ASSERT_TRUE(store_->Delete("k").ok());

  EXPECT_EQ(store_->Get("k").status().code(), StatusCode::kNotFound);
  EXPECT_EQ(store_->Head("k").status().code(), StatusCode::kNotFound);

  auto list = store_->List("");
  ASSERT_TRUE(list.ok());
  EXPECT_TRUE(list.value().empty());
}

TEST_F(LocalObjectStoreTest, DeleteMissingKeyIsNotFound) {
  EXPECT_EQ(store_->Delete("ghost").code(), StatusCode::kNotFound);
}

TEST_F(LocalObjectStoreTest, ListReflectsStoredKeysAndPrefix) {
  ASSERT_TRUE(store_->Put("images/a.png", "a").ok());
  ASSERT_TRUE(store_->Put("images/b.png", "b").ok());
  ASSERT_TRUE(store_->Put("docs/readme.md", "c").ok());

  auto all = store_->List("");
  ASSERT_TRUE(all.ok());
  EXPECT_EQ(all.value().size(), 3u);

  auto images = store_->List("images/");
  ASSERT_TRUE(images.ok());
  EXPECT_EQ(images.value().size(), 2u);
  for (const auto& m : images.value()) {
    EXPECT_EQ(m.key.rfind("images/", 0), 0u);
  }
}

TEST_F(LocalObjectStoreTest, ReopenPreservesCommittedObjects) {
  ASSERT_TRUE(store_->Put("persistent", "durable-bytes").ok());
  store_.reset();

  auto reopened = LocalObjectStore::Open(root_);
  ASSERT_TRUE(reopened.ok());
  store_ = std::move(reopened).value();

  auto got = store_->Get("persistent");
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), "durable-bytes");
}

TEST_F(LocalObjectStoreTest, EmptyKeyIsRejected) {
  EXPECT_EQ(store_->Put("", "x").status().code(), StatusCode::kInvalidArgument);
}

} // namespace
} // namespace dos
