#include "consensus/raft_storage.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>

#include "common/wire.h"

namespace dos {
namespace consensus {
namespace {

// Encodes one log entry body (without the outer length/crc framing).
std::string EncodeEntry(const LogEntry& e) {
  std::string body;
  wire::PutU64(body, e.term);
  wire::PutU64(body, e.index);
  wire::PutStr(body, e.command);
  return body;
}

bool DecodeEntry(const std::string& body, LogEntry* e) {
  wire::Reader r(body.data(), body.size());
  return r.U64(&e->term) && r.U64(&e->index) && r.Str(&e->command);
}

// Frames a body as [u32 len][body][u32 crc].
std::string Frame(const std::string& body) {
  std::string frame;
  wire::PutU32(frame, static_cast<uint32_t>(body.size()));
  frame += body;
  wire::PutU32(frame, wire::Crc32(body.data(), body.size()));
  return frame;
}

// Reads all intact CRC-framed records from a file, stopping at the first
// torn/corrupt record (a crash mid-append leaves a partial tail).
std::vector<std::string> ReadFramedRecords(const std::filesystem::path& path) {
  std::vector<std::string> out;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return out;
  }
  const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  std::size_t pos = 0;
  while (pos + 4 <= all.size()) {
    uint32_t body_len = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      body_len = (body_len << 8) | static_cast<uint8_t>(all[pos + i]);
    }
    const std::size_t frame_end = pos + 4 + body_len + 4;
    if (body_len == 0 || frame_end > all.size()) {
      break; // torn/partial tail
    }
    const std::string body = all.substr(pos + 4, body_len);
    uint32_t stored_crc = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      stored_crc = (stored_crc << 8) | static_cast<uint8_t>(all[pos + 4 + body_len + i]);
    }
    if (wire::Crc32(body.data(), body.size()) != stored_crc) {
      break; // corrupt/torn record
    }
    out.push_back(body);
    pos = frame_end;
  }
  return out;
}

} // namespace

StatusOr<std::unique_ptr<RaftStorage>> RaftStorage::Open(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return Status::IoError("create raft dir: " + ec.message());
  }

  const std::filesystem::path log_path = dir / "raft-log";
  const std::filesystem::path meta_path = dir / "raft-state";

  int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return Status::IoError(std::string("open raft-log: ") + std::strerror(errno));
  }
  auto storage = std::unique_ptr<RaftStorage>(new RaftStorage(dir, fd));

  // Recover the log (index order; entries with a torn tail are dropped).
  for (const std::string& body : ReadFramedRecords(log_path)) {
    LogEntry e;
    if (!DecodeEntry(body, &e)) {
      break;
    }
    storage->loaded_log_.push_back(std::move(e));
  }

  // Recover the hard state (last intact record wins; absent = defaults).
  const auto meta_records = ReadFramedRecords(meta_path);
  if (!meta_records.empty()) {
    wire::Reader r(meta_records.back().data(), meta_records.back().size());
    HardState hs;
    if (r.U64(&hs.current_term) && r.Str(&hs.voted_for)) {
      storage->loaded_hard_ = std::move(hs);
    }
  }

  return storage;
}

RaftStorage::~RaftStorage() {
  if (log_fd_ >= 0) {
    ::close(log_fd_);
  }
}

Status RaftStorage::WriteAll(const char* p, std::size_t n) {
  std::size_t remaining = n;
  while (remaining > 0) {
    ssize_t w = ::write(log_fd_, p, remaining);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IoError(std::string("raft-log write: ") + std::strerror(errno));
    }
    p += w;
    remaining -= static_cast<std::size_t>(w);
  }
  if (::fsync(log_fd_) != 0) {
    return Status::IoError(std::string("raft-log fsync: ") + std::strerror(errno));
  }
  return Status::Ok();
}

Status RaftStorage::WriteFileAtomic(const std::filesystem::path& path, const std::string& bytes) {
  const std::filesystem::path tmp = path.string() + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::IoError(std::string("open tmp: ") + std::strerror(errno));
  }
  const char* p = bytes.data();
  std::size_t remaining = bytes.size();
  while (remaining > 0) {
    ssize_t w = ::write(fd, p, remaining);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return Status::IoError(std::string("write tmp: ") + std::strerror(errno));
    }
    p += w;
    remaining -= static_cast<std::size_t>(w);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return Status::IoError(std::string("fsync tmp: ") + std::strerror(errno));
  }
  ::close(fd);
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec); // atomic replace on POSIX
  if (ec) {
    return Status::IoError("rename: " + ec.message());
  }
  return Status::Ok();
}

Status RaftStorage::SaveHardState(const HardState& hs) {
  std::string body;
  wire::PutU64(body, hs.current_term);
  wire::PutStr(body, hs.voted_for);
  const std::string framed = Frame(body);

  std::lock_guard<std::mutex> lock(mu_);
  return WriteFileAtomic(meta_path_, framed);
}

Status RaftStorage::AppendEntry(const LogEntry& entry) {
  const std::string framed = Frame(EncodeEntry(entry));
  std::lock_guard<std::mutex> lock(mu_);
  return WriteAll(framed.data(), framed.size());
}

Status RaftStorage::RewriteLog(const std::vector<LogEntry>& entries) {
  std::string all;
  for (const LogEntry& e : entries) {
    all += Frame(EncodeEntry(e));
  }
  std::lock_guard<std::mutex> lock(mu_);
  // Atomically replace the log file, then reopen the append fd on the new file.
  Status s = WriteFileAtomic(log_path_, all);
  if (!s.ok()) {
    return s;
  }
  ::close(log_fd_);
  log_fd_ = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (log_fd_ < 0) {
    return Status::IoError(std::string("reopen raft-log: ") + std::strerror(errno));
  }
  return Status::Ok();
}

} // namespace consensus
} // namespace dos
