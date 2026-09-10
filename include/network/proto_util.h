#ifndef DOS_NETWORK_PROTO_UTIL_H_
#define DOS_NETWORK_PROTO_UTIL_H_

#include "common/status.h"
#include "storage.pb.h"
#include "storage/object_metadata.h"

namespace dos {

// Maps between the domain StatusCode and the wire Code enum.
inline rpc::Code ToProtoCode(StatusCode code) {
  switch (code) {
  case StatusCode::kOk:
    return rpc::OK;
  case StatusCode::kNotFound:
    return rpc::NOT_FOUND;
  case StatusCode::kChecksumMismatch:
    return rpc::CHECKSUM_MISMATCH;
  case StatusCode::kConflict:
    return rpc::CONFLICT;
  case StatusCode::kInvalidArgument:
    return rpc::INVALID_ARGUMENT;
  case StatusCode::kAlreadyExists:
    return rpc::ALREADY_EXISTS;
  case StatusCode::kIoError:
    return rpc::IO_ERROR;
  case StatusCode::kUnavailable:
    return rpc::UNAVAILABLE;
  }
  return rpc::IO_ERROR;
}

inline StatusCode FromProtoCode(rpc::Code code) {
  switch (code) {
  case rpc::OK:
    return StatusCode::kOk;
  case rpc::NOT_FOUND:
    return StatusCode::kNotFound;
  case rpc::CHECKSUM_MISMATCH:
    return StatusCode::kChecksumMismatch;
  case rpc::CONFLICT:
    return StatusCode::kConflict;
  case rpc::INVALID_ARGUMENT:
    return StatusCode::kInvalidArgument;
  case rpc::ALREADY_EXISTS:
    return StatusCode::kAlreadyExists;
  case rpc::IO_ERROR:
    return StatusCode::kIoError;
  case rpc::UNAVAILABLE:
    return StatusCode::kUnavailable;
  default:
    return StatusCode::kIoError;
  }
}

inline void ToProtoMeta(const ObjectMetadata& m, rpc::ObjectMeta* out) {
  out->set_key(m.key);
  out->set_size(m.size);
  out->set_checksum(m.checksum);
  out->set_version(m.version);
  out->set_created_at(m.created_at);
  out->set_deleted(m.deleted);
}

inline ObjectMetadata FromProtoMeta(const rpc::ObjectMeta& m) {
  ObjectMetadata out;
  out.key = m.key();
  out.size = m.size();
  out.checksum = m.checksum();
  out.version = m.version();
  out.created_at = m.created_at();
  out.deleted = m.deleted();
  return out;
}

} // namespace dos

#endif // DOS_NETWORK_PROTO_UTIL_H_
