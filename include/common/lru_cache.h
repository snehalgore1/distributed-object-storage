#ifndef DOS_COMMON_LRU_CACHE_H_
#define DOS_COMMON_LRU_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

namespace dos {

// Fixed-capacity LRU cache with O(1)-average Get/Put (spec Milestone 9). A
// doubly linked list orders entries by recency (front = most recent); an
// unordered_map indexes into list nodes for constant-time lookup and splice.
//
// Not thread-safe; guard externally if shared.
template <typename K, typename V> class LruCache {
public:
  explicit LruCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  // Returns the value and marks it most-recently-used, or nullopt on a miss.
  std::optional<V> Get(const K& key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
      ++misses_;
      return std::nullopt;
    }
    ++hits_;
    order_.splice(order_.begin(), order_, it->second); // move to front
    return it->second->second;
  }

  // Inserts or updates `key`, marking it most-recently-used. Evicts the
  // least-recently-used entry if over capacity.
  void Put(const K& key, V value) {
    auto it = index_.find(key);
    if (it != index_.end()) {
      it->second->second = std::move(value);
      order_.splice(order_.begin(), order_, it->second);
      return;
    }
    order_.emplace_front(key, std::move(value));
    index_[key] = order_.begin();
    if (index_.size() > capacity_) {
      const K& evict = order_.back().first;
      index_.erase(evict);
      order_.pop_back();
    }
  }

  // Removes an entry if present (e.g. on invalidation).
  void Erase(const K& key) {
    auto it = index_.find(key);
    if (it != index_.end()) {
      order_.erase(it->second);
      index_.erase(it);
    }
  }

  void Clear() {
    order_.clear();
    index_.clear();
  }

  bool Contains(const K& key) const { return index_.find(key) != index_.end(); }
  std::size_t size() const { return index_.size(); }
  std::size_t capacity() const { return capacity_; }

  uint64_t hits() const { return hits_; }
  uint64_t misses() const { return misses_; }
  double hit_rate() const {
    const uint64_t total = hits_ + misses_;
    return total == 0 ? 0.0 : static_cast<double>(hits_) / static_cast<double>(total);
  }

private:
  using Entry = std::pair<K, V>;
  std::size_t capacity_;
  std::list<Entry> order_; // front = most recently used
  std::unordered_map<K, typename std::list<Entry>::iterator> index_;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

} // namespace dos

#endif // DOS_COMMON_LRU_CACHE_H_
