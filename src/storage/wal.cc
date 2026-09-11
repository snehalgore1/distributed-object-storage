#include "storage/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>

namespace dos {
namespace {

// Standard CRC-32 (IEEE 802.3), computed on the fly (no static table so there
// is no initialization-order or threading concern).
uint32_t Crc32(const uint8_t* data, std::size_t len) {
  uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return ~crc;
}

void PutU32(std::string& out, uint32_t v) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}
void PutU64(std::string& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}
void PutStr(std::string& out, std::string_view s) {
  PutU32(out, static_cast<uint32_t>(s.size()));
  out.append(s.data(), s.size());
}

// Reads big-endian integers / strings from a buffer with bounds checks.
class Reader {
public:
  Reader(const char* p, std::size_t n) : p_(p), n_(n) {}
  bool U32(uint32_t* v) {
    if (pos_ + 4 > n_)
      return false;
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i)
      r = (r << 8) | static_cast<uint8_t>(p_[pos_++]);
    *v = r;
    return true;
  }
  bool U64(uint64_t* v) {
    if (pos_ + 8 > n_)
      return false;
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i)
      r = (r << 8) | static_cast<uint8_t>(p_[pos_++]);
    *v = r;
    return true;
  }
  bool U8(uint8_t* v) {
    if (pos_ + 1 > n_)
      return false;
    *v = static_cast<uint8_t>(p_[pos_++]);
    return true;
  }
  bool Str(std::string* s) {
    uint32_t len;
    if (!U32(&len))
      return false;
    if (pos_ + len > n_)
      return false;
    s->assign(p_ + pos_, len);
    pos_ += len;
    return true;
  }

private:
  const char* p_;
  std::size_t n_;
  std::size_t pos_ = 0;
};

// Encodes a record body (without the outer length/crc framing).
std::string EncodeBody(const WalRecord& rec) {
  std::string body;
  body.push_back(static_cast<char>(rec.is_commit ? 1 : 0));
  PutU64(body, rec.seq);
  if (!rec.is_commit) {
    body.push_back(static_cast<char>(static_cast<uint8_t>(rec.op)));
    PutU64(body, rec.version);
    PutU64(body, rec.size);
    PutStr(body, rec.key);
    PutStr(body, rec.checksum);
  }
  return body;
}

bool DecodeBody(const std::string& body, WalRecord* rec) {
  Reader r(body.data(), body.size());
  uint8_t is_commit = 0;
  if (!r.U8(&is_commit))
    return false;
  rec->is_commit = is_commit != 0;
  if (!r.U64(&rec->seq))
    return false;
  if (!rec->is_commit) {
    uint8_t op = 0;
    if (!r.U8(&op))
      return false;
    rec->op = static_cast<WalOp>(op);
    if (!r.U64(&rec->version))
      return false;
    if (!r.U64(&rec->size))
      return false;
    if (!r.Str(&rec->key))
      return false;
    if (!r.Str(&rec->checksum))
      return false;
  }
  return true;
}

} // namespace

StatusOr<std::unique_ptr<Wal>> Wal::Open(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return Status::IoError(std::string("open wal: ") + std::strerror(errno));
  }
  auto wal = std::unique_ptr<Wal>(new Wal(fd, path));
  // Continue the sequence past whatever is already on disk.
  auto existing = Replay(path);
  if (existing.ok()) {
    uint64_t max_seq = 0;
    for (const auto& r : existing.value()) {
      max_seq = std::max(max_seq, r.seq);
    }
    wal->next_seq_.store(max_seq + 1, std::memory_order_relaxed);
  }
  return wal;
}

Wal::~Wal() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Status Wal::AppendRecord(const WalRecord& rec) {
  const std::string body = EncodeBody(rec);
  const uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size());

  std::string frame;
  PutU32(frame, static_cast<uint32_t>(body.size()));
  frame += body;
  PutU32(frame, crc);

  std::lock_guard<std::mutex> lock(mu_);
  const char* p = frame.data();
  std::size_t remaining = frame.size();
  while (remaining > 0) {
    ssize_t w = ::write(fd_, p, remaining);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return Status::IoError(std::string("wal write: ") + std::strerror(errno));
    }
    p += w;
    remaining -= static_cast<std::size_t>(w);
  }
  if (::fsync(fd_) != 0) {
    return Status::IoError(std::string("wal fsync: ") + std::strerror(errno));
  }
  return Status::Ok();
}

Status Wal::AppendBegin(uint64_t seq, WalOp op, std::string_view key, uint64_t version,
                        std::string_view checksum, uint64_t size) {
  WalRecord rec;
  rec.seq = seq;
  rec.is_commit = false;
  rec.op = op;
  rec.key = std::string(key);
  rec.version = version;
  rec.checksum = std::string(checksum);
  rec.size = size;
  return AppendRecord(rec);
}

Status Wal::AppendCommit(uint64_t seq) {
  WalRecord rec;
  rec.seq = seq;
  rec.is_commit = true;
  return AppendRecord(rec);
}

Status Wal::Truncate() {
  std::lock_guard<std::mutex> lock(mu_);
  ::close(fd_);
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) {
    return Status::IoError(std::string("truncate wal: ") + std::strerror(errno));
  }
  next_seq_.store(1, std::memory_order_relaxed);
  return Status::Ok();
}

StatusOr<std::vector<WalRecord>> Wal::Replay(const std::filesystem::path& path) {
  std::vector<WalRecord> records;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return records; // no WAL yet
  }
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  std::size_t pos = 0;
  while (pos + 4 <= all.size()) {
    // Frame: [u32 body_len][body][u32 crc]
    uint32_t body_len = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      body_len = (body_len << 8) | static_cast<uint8_t>(all[pos + i]);
    }
    const std::size_t frame_end = pos + 4 + body_len + 4;
    if (body_len == 0 || frame_end > all.size()) {
      break; // torn/partial tail: stop
    }
    const std::string body = all.substr(pos + 4, body_len);
    uint32_t stored_crc = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      stored_crc = (stored_crc << 8) | static_cast<uint8_t>(all[pos + 4 + body_len + i]);
    }
    if (Crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size()) != stored_crc) {
      break; // corrupt/torn record: stop
    }
    WalRecord rec;
    if (!DecodeBody(body, &rec)) {
      break;
    }
    records.push_back(std::move(rec));
    pos = frame_end;
  }
  return records;
}

} // namespace dos
