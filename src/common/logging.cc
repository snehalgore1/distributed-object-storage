#include "common/logging.h"

#include <chrono>
#include <iostream>
#include <sstream>

namespace dos {
namespace {

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out.push_back(c);
    }
  }
  return out;
}

const char* LevelName(LogLevel level) {
  switch (level) {
  case LogLevel::kInfo:
    return "info";
  case LogLevel::kWarn:
    return "warn";
  case LogLevel::kError:
    return "error";
  }
  return "info";
}

} // namespace

Logger& Logger::Default() {
  static Logger instance;
  return instance;
}

void Logger::SetSink(std::ostream* sink) {
  std::lock_guard<std::mutex> lock(mu_);
  sink_ = sink;
}

void Logger::Log(LogLevel level, const std::string& msg, const Fields& fields) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

  std::ostringstream line;
  line << "{\"ts_ms\":" << ms << ",\"level\":\"" << LevelName(level) << "\",\"msg\":\""
       << JsonEscape(msg) << "\"";
  for (const auto& [k, v] : fields) {
    line << ",\"" << JsonEscape(k) << "\":\"" << JsonEscape(v) << "\"";
  }
  line << "}\n";

  std::lock_guard<std::mutex> lock(mu_);
  std::ostream& out = sink_ != nullptr ? *sink_ : std::clog;
  out << line.str();
  out.flush();
}

void LogInfo(const std::string& msg, const Logger::Fields& fields) {
  Logger::Default().Log(LogLevel::kInfo, msg, fields);
}
void LogWarn(const std::string& msg, const Logger::Fields& fields) {
  Logger::Default().Log(LogLevel::kWarn, msg, fields);
}
void LogError(const std::string& msg, const Logger::Fields& fields) {
  Logger::Default().Log(LogLevel::kError, msg, fields);
}

} // namespace dos
