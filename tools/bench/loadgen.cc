// Concurrent HTTP load generator (spec Milestone 13). Drives the gateway with a
// configurable number of client threads, object size, and read/write mix, and
// reports throughput and latency percentiles.
//
// Usage:
//   loadgen [--gateway H:P] [--threads N] [--duration S] [--size BYTES]
//           [--read-ratio 0..1] [--keyspace K]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "network/http_client.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Args {
  std::string gateway = "127.0.0.1:8080";
  int threads = 8;
  double duration = 10.0;
  std::size_t size = 4096;
  double read_ratio = 0.8;
  int keyspace = 1000;
};

std::string Value(int argc, char** argv, const std::string& flag, const std::string& def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag == argv[i])
      return argv[i + 1];
  }
  return def;
}

std::pair<std::string, int> SplitHostPort(const std::string& hp) {
  const auto c = hp.rfind(':');
  return c == std::string::npos
             ? std::make_pair(hp, 8080)
             : std::make_pair(hp.substr(0, c), std::atoi(hp.substr(c + 1).c_str()));
}

double Percentile(std::vector<double>& v, double p) {
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  return v[static_cast<std::size_t>(p / 100.0 * (static_cast<double>(v.size()) - 1))];
}

} // namespace

int main(int argc, char** argv) {
  Args a;
  a.gateway = Value(argc, argv, "--gateway", a.gateway);
  a.threads = std::atoi(Value(argc, argv, "--threads", std::to_string(a.threads)).c_str());
  a.duration = std::atof(Value(argc, argv, "--duration", std::to_string(a.duration)).c_str());
  a.size = static_cast<std::size_t>(
      std::atoll(Value(argc, argv, "--size", std::to_string(a.size)).c_str()));
  a.read_ratio = std::atof(Value(argc, argv, "--read-ratio", std::to_string(a.read_ratio)).c_str());
  a.keyspace = std::atoi(Value(argc, argv, "--keyspace", std::to_string(a.keyspace)).c_str());

  const auto [host, port] = SplitHostPort(a.gateway);
  const std::string payload(a.size, 'x');

  // Pre-populate the keyspace so reads hit existing objects.
  {
    dos::HttpClient c(host, port);
    for (int i = 0; i < a.keyspace; ++i) {
      c.Request("PUT", "/objects/bench/" + std::to_string(i), payload);
    }
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> ok{0};
  std::atomic<uint64_t> failed{0};
  std::vector<std::vector<double>> per_thread_lat(static_cast<std::size_t>(a.threads));

  const auto start = Clock::now();
  std::vector<std::thread> workers;
  for (int t = 0; t < a.threads; ++t) {
    workers.emplace_back([&, t] {
      dos::HttpClient client(host, port);
      std::mt19937 rng(static_cast<uint32_t>(t) * 2654435761u + 1u);
      std::uniform_int_distribution<int> key_dist(0, a.keyspace - 1);
      std::uniform_real_distribution<double> coin(0.0, 1.0);
      auto& lat = per_thread_lat[static_cast<std::size_t>(t)];
      while (!stop.load(std::memory_order_relaxed)) {
        const std::string key = "/objects/bench/" + std::to_string(key_dist(rng));
        const bool read = coin(rng) < a.read_ratio;
        const auto t0 = Clock::now();
        auto r = read ? client.Request("GET", key) : client.Request("PUT", key, payload);
        const auto t1 = Clock::now();
        const bool good = r.ok() && r.value().status >= 200 && r.value().status < 300;
        if (good) {
          ok.fetch_add(1, std::memory_order_relaxed);
          lat.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        } else {
          failed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::duration<double>(a.duration));
  stop.store(true);
  for (auto& w : workers)
    w.join();

  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  std::vector<double> all;
  for (auto& v : per_thread_lat)
    all.insert(all.end(), v.begin(), v.end());

  const uint64_t total = ok.load();
  const double tput = elapsed > 0 ? static_cast<double>(total) / elapsed : 0;
  const double mbps = tput * static_cast<double>(a.size) / (1024.0 * 1024.0);

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "gateway=" << a.gateway << " threads=" << a.threads << " size=" << a.size
            << "B read_ratio=" << a.read_ratio << "\n";
  std::cout << "requests: " << total << " ok, " << failed.load() << " failed in " << elapsed
            << "s\n";
  std::cout << "throughput: " << tput << " req/s  (" << mbps << " MB/s)\n";
  std::cout << "latency ms: p50=" << Percentile(all, 50) << "  p95=" << Percentile(all, 95)
            << "  p99=" << Percentile(all, 99) << "\n";
  return 0;
}
