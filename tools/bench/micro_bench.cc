// Single-node micro-benchmark: measures PUT and GET throughput and latency
// percentiles for the local storage engine across a few object sizes.
//
// This is deliberately honest and reproducible: single node, single thread,
// warm OS cache. It is NOT a distributed or concurrency benchmark (those are a
// later milestone). Run it and quote the numbers it prints, with your hardware.
//
// Usage: micro_bench [ops_per_size]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "storage/local_object_store.h"

namespace {

using Clock = std::chrono::steady_clock;

double PercentileMicros(std::vector<double>& lat, double p) {
  if (lat.empty())
    return 0.0;
  std::sort(lat.begin(), lat.end());
  const auto idx = static_cast<std::size_t>(p / 100.0 * (static_cast<double>(lat.size()) - 1));
  return lat[idx];
}

struct Result {
  double throughput_ops = 0;
  double throughput_mb = 0;
  double p50 = 0, p95 = 0, p99 = 0;
};

Result Summarize(std::vector<double>& lat_us, std::size_t object_size) {
  double total_us = 0;
  for (double v : lat_us)
    total_us += v;
  Result r;
  const double total_s = total_us / 1e6;
  r.throughput_ops = total_s > 0 ? static_cast<double>(lat_us.size()) / total_s : 0;
  r.throughput_mb = r.throughput_ops * static_cast<double>(object_size) / (1024.0 * 1024.0);
  r.p50 = PercentileMicros(lat_us, 50);
  r.p95 = PercentileMicros(lat_us, 95);
  r.p99 = PercentileMicros(lat_us, 99);
  return r;
}

} // namespace

int main(int argc, char** argv) {
  const int ops = argc > 1 ? std::atoi(argv[1]) : 2000;
  const std::vector<std::size_t> sizes = {4 * 1024, 64 * 1024, 1024 * 1024};

  const std::string root = "/tmp/dos-micro-bench";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  auto store_or = dos::LocalObjectStore::Open(root);
  if (!store_or.ok()) {
    std::cerr << "open failed: " << store_or.status().ToString() << "\n";
    return 1;
  }
  auto store = std::move(store_or).value();

  std::cout << "Single-node micro-benchmark (" << ops << " ops/size, single thread)\n\n";
  std::cout << std::left << std::setw(8) << "size" << std::setw(6) << "op" << std::right
            << std::setw(12) << "ops/s" << std::setw(12) << "MB/s" << std::setw(11) << "p50(us)"
            << std::setw(11) << "p95(us)" << std::setw(11) << "p99(us)" << "\n";
  std::cout << std::string(71, '-') << "\n";

  for (std::size_t size : sizes) {
    const std::string payload(size, 'x');

    std::vector<double> put_lat;
    put_lat.reserve(static_cast<std::size_t>(ops));
    for (int i = 0; i < ops; ++i) {
      const std::string key = "obj/" + std::to_string(size) + "/" + std::to_string(i);
      const auto t0 = Clock::now();
      auto r = store->Put(key, payload);
      const auto t1 = Clock::now();
      if (!r.ok()) {
        std::cerr << "put failed: " << r.status().ToString() << "\n";
        return 1;
      }
      put_lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::vector<double> get_lat;
    get_lat.reserve(static_cast<std::size_t>(ops));
    for (int i = 0; i < ops; ++i) {
      const std::string key = "obj/" + std::to_string(size) + "/" + std::to_string(i);
      const auto t0 = Clock::now();
      auto r = store->Get(key);
      const auto t1 = Clock::now();
      if (!r.ok()) {
        std::cerr << "get failed: " << r.status().ToString() << "\n";
        return 1;
      }
      get_lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    auto human = [](std::size_t n) {
      return n >= 1024 * 1024 ? std::to_string(n / (1024 * 1024)) + "MB"
                              : std::to_string(n / 1024) + "KB";
    };
    auto row = [&](const char* op, Result r) {
      std::cout << std::left << std::setw(8) << human(size) << std::setw(6) << op << std::right
                << std::fixed << std::setprecision(0) << std::setw(12) << r.throughput_ops
                << std::setprecision(1) << std::setw(12) << r.throughput_mb << std::setprecision(1)
                << std::setw(11) << r.p50 << std::setw(11) << r.p95 << std::setw(11) << r.p99
                << "\n";
    };
    row("PUT", Summarize(put_lat, size));
    row("GET", Summarize(get_lat, size));
  }

  std::filesystem::remove_all(root, ec);
  return 0;
}
