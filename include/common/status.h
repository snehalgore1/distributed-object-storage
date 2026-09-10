#ifndef DOS_COMMON_STATUS_H_
#define DOS_COMMON_STATUS_H_

#include <optional>
#include <string>
#include <utility>

namespace dos {

// Coarse error classification shared across the storage engine. These map
// cleanly onto gRPC / HTTP status codes in later milestones.
enum class StatusCode {
  kOk,
  kNotFound,
  kChecksumMismatch,
  kConflict,
  kInvalidArgument,
  kAlreadyExists,
  kIoError,
  kUnavailable, // overload / backpressure: work rejected, retry later
};

const char* StatusCodeName(StatusCode code);

// A lightweight (code, message) result type. An Ok status carries no message.
class Status {
public:
  Status() = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(); }
  static Status NotFound(std::string msg) { return {StatusCode::kNotFound, std::move(msg)}; }
  static Status ChecksumMismatch(std::string msg) {
    return {StatusCode::kChecksumMismatch, std::move(msg)};
  }
  static Status Conflict(std::string msg) { return {StatusCode::kConflict, std::move(msg)}; }
  static Status InvalidArgument(std::string msg) {
    return {StatusCode::kInvalidArgument, std::move(msg)};
  }
  static Status AlreadyExists(std::string msg) {
    return {StatusCode::kAlreadyExists, std::move(msg)};
  }
  static Status IoError(std::string msg) { return {StatusCode::kIoError, std::move(msg)}; }
  static Status Unavailable(std::string msg) { return {StatusCode::kUnavailable, std::move(msg)}; }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const;

private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// Either a value of type T or a non-Ok Status. Never holds a value when the
// status is not Ok.
template <typename T> class StatusOr {
public:
  StatusOr(Status status) : status_(std::move(status)) {}                // NOLINT(runtime/explicit)
  StatusOr(T value) : status_(Status::Ok()), value_(std::move(value)) {} // NOLINT

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }

  const T& value() const& { return *value_; }
  T& value() & { return *value_; }
  T&& value() && { return std::move(*value_); }

private:
  Status status_;
  std::optional<T> value_;
};

} // namespace dos

#endif // DOS_COMMON_STATUS_H_
