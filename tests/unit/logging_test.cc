#include "common/logging.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <string>

namespace dos {
namespace {

TEST(LoggingTest, EmitsOneJsonLineWithFields) {
  std::ostringstream sink;
  Logger logger;
  logger.SetSink(&sink);
  logger.Log(LogLevel::kInfo, "http_request",
             {{"request_id", "abc123"}, {"method", "GET"}, {"status", "200"}});

  const std::string out = sink.str();
  // Exactly one line.
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1);
  EXPECT_NE(out.find("\"level\":\"info\""), std::string::npos);
  EXPECT_NE(out.find("\"msg\":\"http_request\""), std::string::npos);
  EXPECT_NE(out.find("\"request_id\":\"abc123\""), std::string::npos);
  EXPECT_NE(out.find("\"method\":\"GET\""), std::string::npos);
  EXPECT_NE(out.find("\"ts_ms\":"), std::string::npos);
}

TEST(LoggingTest, EscapesQuotesAndNewlines) {
  std::ostringstream sink;
  Logger logger;
  logger.SetSink(&sink);
  logger.Log(LogLevel::kError, "bad \"input\"\nsecond line");
  const std::string out = sink.str();
  EXPECT_NE(out.find("\\\"input\\\""), std::string::npos);
  EXPECT_NE(out.find("\\n"), std::string::npos);
  EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1); // still one physical line
}

} // namespace
} // namespace dos
