#include "common/metrics.h"

#include <algorithm>
#include <atomic>
#include <random>
#include <sstream>

namespace dos {
namespace {

// Serializes labels to Prometheus form: {k="v",k2="v2"} (empty -> "").
std::string LabelKey(const MetricsRegistry::Labels& labels) {
  if (labels.empty())
    return "";
  MetricsRegistry::Labels sorted = labels;
  std::sort(sorted.begin(), sorted.end());
  std::string out = "{";
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i)
      out += ",";
    out += sorted[i].first + "=\"" + sorted[i].second + "\"";
  }
  out += "}";
  return out;
}

const std::vector<double>& DefaultBuckets() {
  static const std::vector<double> kBuckets = {0.0005, 0.001, 0.005, 0.01, 0.05,
                                               0.1,    0.5,   1.0,   5.0};
  return kBuckets;
}

} // namespace

MetricsRegistry::Family& MetricsRegistry::GetOrCreate(const std::string& name, Type type,
                                                      const std::string& help) {
  auto it = families_.find(name);
  if (it == families_.end()) {
    Family f;
    f.type = type;
    f.help = help;
    it = families_.emplace(name, std::move(f)).first;
  }
  return it->second;
}

void MetricsRegistry::IncCounter(const std::string& name, const std::string& help, double amount,
                                 const Labels& labels) {
  std::lock_guard<std::mutex> lock(mu_);
  Family& f = GetOrCreate(name, Type::kCounter, help);
  f.samples[LabelKey(labels)].value += amount;
}

void MetricsRegistry::SetGauge(const std::string& name, const std::string& help, double value,
                               const Labels& labels) {
  std::lock_guard<std::mutex> lock(mu_);
  Family& f = GetOrCreate(name, Type::kGauge, help);
  f.samples[LabelKey(labels)].value = value;
}

void MetricsRegistry::ObserveHistogram(const std::string& name, const std::string& help,
                                       double value, const Labels& labels,
                                       const std::vector<double>& buckets) {
  std::lock_guard<std::mutex> lock(mu_);
  Family& f = GetOrCreate(name, Type::kHistogram, help);
  if (f.buckets.empty()) {
    f.buckets = buckets.empty() ? DefaultBuckets() : buckets;
  }
  Sample& s = f.samples[LabelKey(labels)];
  if (s.bucket_counts.empty()) {
    s.bucket_counts.assign(f.buckets.size() + 1, 0); // +1 for +Inf
  }
  // Count into the single bucket the value falls into; Serialize() cumulates.
  bool placed = false;
  for (std::size_t i = 0; i < f.buckets.size(); ++i) {
    if (value <= f.buckets[i]) {
      ++s.bucket_counts[i];
      placed = true;
      break;
    }
  }
  if (!placed) {
    ++s.bucket_counts.back(); // exceeds all finite buckets (+Inf only)
  }
  s.sum += value;
  ++s.count;
}

std::string MetricsRegistry::Serialize() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ostringstream out;
  for (const auto& [name, f] : families_) {
    if (!f.help.empty()) {
      out << "# HELP " << name << " " << f.help << "\n";
    }
    const char* type = f.type == Type::kCounter ? "counter"
                       : f.type == Type::kGauge ? "gauge"
                                                : "histogram";
    out << "# TYPE " << name << " " << type << "\n";

    for (const auto& [labelkey, s] : f.samples) {
      if (f.type == Type::kHistogram) {
        // Cumulative bucket counts.
        uint64_t cumulative = 0;
        for (std::size_t i = 0; i < f.buckets.size(); ++i) {
          cumulative += s.bucket_counts[i];
          std::ostringstream le;
          le << f.buckets[i];
          const std::string bucket_labels =
              labelkey.empty()
                  ? "{le=\"" + le.str() + "\"}"
                  : labelkey.substr(0, labelkey.size() - 1) + ",le=\"" + le.str() + "\"}";
          out << name << "_bucket" << bucket_labels << " " << cumulative << "\n";
        }
        const std::string inf_labels =
            labelkey.empty() ? "{le=\"+Inf\"}"
                             : labelkey.substr(0, labelkey.size() - 1) + ",le=\"+Inf\"}";
        out << name << "_bucket" << inf_labels << " " << s.count << "\n";
        out << name << "_sum" << labelkey << " " << s.sum << "\n";
        out << name << "_count" << labelkey << " " << s.count << "\n";
      } else {
        out << name << labelkey << " " << s.value << "\n";
      }
    }
  }
  return out.str();
}

std::string NewRequestId() {
  static std::atomic<uint64_t> counter{0};
  static std::mt19937_64 rng(std::random_device{}());
  const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  const uint64_t r = rng();
  std::ostringstream out;
  out << std::hex << (r ^ (n * 0x9e3779b97f4a7c15ULL));
  return out.str();
}

} // namespace dos
