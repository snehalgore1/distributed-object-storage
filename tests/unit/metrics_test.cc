#include "common/metrics.h"

#include <gtest/gtest.h>

#include <string>

namespace dos {
namespace {

TEST(MetricsTest, CounterSerializes) {
  MetricsRegistry reg;
  reg.IncCounter("requests_total", "total requests", 1);
  reg.IncCounter("requests_total", "total requests", 2);
  const std::string out = reg.Serialize();
  EXPECT_NE(out.find("# TYPE requests_total counter"), std::string::npos);
  EXPECT_NE(out.find("requests_total 3"), std::string::npos);
}

TEST(MetricsTest, LabeledCounters) {
  MetricsRegistry reg;
  reg.IncCounter("http_total", "h", 1, {{"method", "GET"}});
  reg.IncCounter("http_total", "h", 1, {{"method", "GET"}});
  reg.IncCounter("http_total", "h", 1, {{"method", "PUT"}});
  const std::string out = reg.Serialize();
  EXPECT_NE(out.find("http_total{method=\"GET\"} 2"), std::string::npos);
  EXPECT_NE(out.find("http_total{method=\"PUT\"} 1"), std::string::npos);
}

TEST(MetricsTest, GaugeTracksLatestValue) {
  MetricsRegistry reg;
  reg.SetGauge("in_flight", "requests in flight", 5);
  reg.SetGauge("in_flight", "requests in flight", 2);
  EXPECT_NE(reg.Serialize().find("in_flight 2"), std::string::npos);
}

TEST(MetricsTest, HistogramBucketsAreCumulative) {
  MetricsRegistry reg;
  const std::vector<double> buckets = {1.0, 10.0};
  reg.ObserveHistogram("lat", "latency", 0.5, {}, buckets);  // <= 1, <= 10
  reg.ObserveHistogram("lat", "latency", 5.0, {}, buckets);  // <= 10
  reg.ObserveHistogram("lat", "latency", 50.0, {}, buckets); // +Inf only
  const std::string out = reg.Serialize();
  EXPECT_NE(out.find("# TYPE lat histogram"), std::string::npos);
  EXPECT_NE(out.find("lat_bucket{le=\"1\"} 1"), std::string::npos);  // one <= 1
  EXPECT_NE(out.find("lat_bucket{le=\"10\"} 2"), std::string::npos); // two <= 10 (cumulative)
  EXPECT_NE(out.find("lat_bucket{le=\"+Inf\"} 3"), std::string::npos);
  EXPECT_NE(out.find("lat_count 3"), std::string::npos);
  EXPECT_NE(out.find("lat_sum 55.5"), std::string::npos);
}

TEST(MetricsTest, RequestIdsAreDistinct) { EXPECT_NE(NewRequestId(), NewRequestId()); }

} // namespace
} // namespace dos
