#ifndef DOS_COMMON_METRICS_H_
#define DOS_COMMON_METRICS_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dos {

// A small, self-contained Prometheus-style metrics registry (spec Milestone 10)
// — no external client library. Supports counters, gauges, and histograms with
// labels, and renders the Prometheus text exposition format for a `/metrics`
// endpoint. Thread-safe.
//
// Labels are passed as (name, value) pairs; a metric is identified by its name
// plus its label set.
class MetricsRegistry {
public:
  using Labels = std::vector<std::pair<std::string, std::string>>;

  void IncCounter(const std::string& name, const std::string& help, double amount = 1.0,
                  const Labels& labels = {});
  void SetGauge(const std::string& name, const std::string& help, double value,
                const Labels& labels = {});
  // Observes a value into a histogram. Uses `buckets` (upper bounds, ascending)
  // the first time the metric is seen; a default latency-oriented set otherwise.
  void ObserveHistogram(const std::string& name, const std::string& help, double value,
                        const Labels& labels = {}, const std::vector<double>& buckets = {});

  // Prometheus text exposition of every metric.
  std::string Serialize() const;

private:
  enum class Type { kCounter, kGauge, kHistogram };

  struct Sample {
    double value = 0.0;                  // counter/gauge
    std::vector<uint64_t> bucket_counts; // histogram: per-bucket (incl +Inf)
    double sum = 0.0;                    // histogram
    uint64_t count = 0;                  // histogram
  };

  struct Family {
    Type type;
    std::string help;
    std::vector<double> buckets;           // histogram bucket upper bounds
    std::map<std::string, Sample> samples; // label-key -> sample
  };

  Family& GetOrCreate(const std::string& name, Type type, const std::string& help);

  mutable std::mutex mu_;
  std::map<std::string, Family> families_;
};

// Generates a short random hex request id for end-to-end tracing.
std::string NewRequestId();

} // namespace dos

#endif // DOS_COMMON_METRICS_H_
