#include "common/status.h"

namespace dos {

const char* StatusCodeName(StatusCode code) {
  switch (code) {
  case StatusCode::kOk:
    return "OK";
  case StatusCode::kNotFound:
    return "NOT_FOUND";
  case StatusCode::kChecksumMismatch:
    return "CHECKSUM_MISMATCH";
  case StatusCode::kConflict:
    return "CONFLICT";
  case StatusCode::kInvalidArgument:
    return "INVALID_ARGUMENT";
  case StatusCode::kAlreadyExists:
    return "ALREADY_EXISTS";
  case StatusCode::kIoError:
    return "IO_ERROR";
  }
  return "UNKNOWN";
}

std::string Status::ToString() const {
  std::string out = StatusCodeName(code_);
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

} // namespace dos
