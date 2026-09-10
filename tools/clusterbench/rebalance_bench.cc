// Rebalancing benchmark (spec Section 24.3, experiment C).
//
// Places N keys across 3 nodes, then adds a 4th, and reports how many keys move
// under consistent hashing versus modulo hashing. Also prints the per-node
// distribution to show virtual-node balance.
//
// Usage: rebalance_bench [num_keys] [vnodes]

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "cluster/consistent_hash_ring.h"

namespace {

std::string Key(int i) { return "object-key-" + std::to_string(i); }

void PrintDistribution(const dos::ConsistentHashRing& ring, int num_keys) {
  std::map<std::string, int> counts;
  for (int i = 0; i < num_keys; ++i) {
    counts[ring.LookupPrimary(Key(i))]++;
  }
  std::cout << "Distribution across " << counts.size() << " nodes (" << num_keys << " keys):\n";
  for (const auto& [node, n] : counts) {
    const double pct = 100.0 * n / num_keys;
    std::cout << "  " << node << ": " << n << "  (" << std::fixed << std::setprecision(2) << pct
              << "%)\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  const int num_keys = argc > 1 ? std::atoi(argv[1]) : 100000;
  const int vnodes = argc > 2 ? std::atoi(argv[2]) : 200;

  const std::vector<std::string> nodes3 = {"node-a", "node-b", "node-c"};
  const std::vector<std::string> nodes4 = {"node-a", "node-b", "node-c", "node-d"};

  dos::ConsistentHashRing r3(static_cast<std::size_t>(vnodes));
  for (const auto& n : nodes3)
    r3.AddNode(n);

  std::cout << "=== Consistent hashing (vnodes=" << vnodes << ") ===\n";
  PrintDistribution(r3, num_keys);

  dos::ConsistentHashRing r4(static_cast<std::size_t>(vnodes));
  for (const auto& n : nodes4)
    r4.AddNode(n);

  int consistent_moved = 0;
  int modulo_moved = 0;
  for (int i = 0; i < num_keys; ++i) {
    const std::string k = Key(i);
    if (r3.LookupPrimary(k) != r4.LookupPrimary(k))
      ++consistent_moved;

    const uint64_t h = std::hash<std::string>{}(k);
    if (nodes3[h % 3] != nodes4[h % 4])
      ++modulo_moved;
  }

  auto pct = [&](int moved) { return 100.0 * moved / num_keys; };
  std::cout << "\n=== Adding a 4th node: keys that move (of " << num_keys << ") ===\n";
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  consistent hashing: " << consistent_moved << "  (" << pct(consistent_moved)
            << "%)  ideal ~25%\n";
  std::cout << "  modulo hashing:     " << modulo_moved << "  (" << pct(modulo_moved)
            << "%)  ideal ~75%\n";
  std::cout << "  consistent moves " << std::setprecision(1)
            << static_cast<double>(modulo_moved) / std::max(1, consistent_moved)
            << "x fewer keys than modulo.\n";
  return 0;
}
