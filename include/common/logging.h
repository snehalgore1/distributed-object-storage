#ifndef DOS_COMMON_LOGGING_H_
#define DOS_COMMON_LOGGING_H_

#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace dos {

enum class LogLevel { kInfo, kWarn, kError };

// Emits one structured JSON object per line (spec Milestone 10), e.g.:
//   {"ts_ms":1699,"level":"info","msg":"http_request","request_id":"a1b2",...}
// A request_id field threaded through log lines lets you trace one request end
// to end by grepping the id. Thread-safe; the sink is swappable for tests.
class Logger {
public:
  using Fields = std::vector<std::pair<std::string, std::string>>;

  static Logger& Default();

  void SetSink(std::ostream* sink);
  void Log(LogLevel level, const std::string& msg, const Fields& fields = {});

private:
  std::mutex mu_;
  std::ostream* sink_ = nullptr; // nullptr -> std::clog
};

// Convenience free functions logging to Logger::Default().
void LogInfo(const std::string& msg, const Logger::Fields& fields = {});
void LogWarn(const std::string& msg, const Logger::Fields& fields = {});
void LogError(const std::string& msg, const Logger::Fields& fields = {});

} // namespace dos

#endif // DOS_COMMON_LOGGING_H_
