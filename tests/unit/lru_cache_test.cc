#include "common/lru_cache.h"

#include <gtest/gtest.h>

#include <string>

namespace dos {
namespace {

TEST(LruCacheTest, HitAndMiss) {
  LruCache<std::string, int> c(2);
  EXPECT_FALSE(c.Get("a").has_value());
  c.Put("a", 1);
  auto v = c.Get("a");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 1);
  EXPECT_EQ(c.hits(), 1u);
  EXPECT_EQ(c.misses(), 1u);
}

TEST(LruCacheTest, UpdateExistingKeyDoesNotGrow) {
  LruCache<std::string, int> c(2);
  c.Put("a", 1);
  c.Put("a", 2);
  EXPECT_EQ(c.size(), 1u);
  EXPECT_EQ(*c.Get("a"), 2);
}

TEST(LruCacheTest, EvictsLeastRecentlyUsed) {
  LruCache<std::string, int> c(2);
  c.Put("a", 1);
  c.Put("b", 2);
  c.Put("c", 3); // evicts "a" (LRU)
  EXPECT_FALSE(c.Contains("a"));
  EXPECT_TRUE(c.Contains("b"));
  EXPECT_TRUE(c.Contains("c"));
  EXPECT_EQ(c.size(), 2u);
}

TEST(LruCacheTest, GetRefreshesRecency) {
  LruCache<std::string, int> c(2);
  c.Put("a", 1);
  c.Put("b", 2);
  ASSERT_TRUE(c.Get("a").has_value()); // "a" now most-recent
  c.Put("c", 3);                       // should evict "b", not "a"
  EXPECT_TRUE(c.Contains("a"));
  EXPECT_FALSE(c.Contains("b"));
  EXPECT_TRUE(c.Contains("c"));
}

TEST(LruCacheTest, EraseAndClear) {
  LruCache<std::string, int> c(4);
  c.Put("a", 1);
  c.Put("b", 2);
  c.Erase("a");
  EXPECT_FALSE(c.Contains("a"));
  EXPECT_EQ(c.size(), 1u);
  c.Clear();
  EXPECT_EQ(c.size(), 0u);
  EXPECT_FALSE(c.Contains("b"));
}

TEST(LruCacheTest, HitRate) {
  LruCache<int, int> c(10);
  c.Put(1, 1);
  c.Get(1); // hit
  c.Get(2); // miss
  c.Get(1); // hit
  EXPECT_DOUBLE_EQ(c.hit_rate(), 2.0 / 3.0);
}

} // namespace
} // namespace dos
