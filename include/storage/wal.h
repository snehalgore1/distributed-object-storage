#ifndef DOS_STORAGE_WAL_H_
#define DOS_STORAGE_WAL_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace dos {

enum class WalOp : uint8_t { kPut = 1, kDelete = 2 };

// One write-ahead log record. A mutation is logged as a BEGIN (intent) before
// any visible state change, then a COMMIT once the metadata is durably written.
struct WalRecord {
  uint64_t seq = 0;
  bool is_commit = false; // false = BEGIN (fields below valid); true = COMMIT
  WalOp op = WalOp::kPut;
  std::string key;
  uint64_t version = 0;
  std::string checksum; // sha256 hex of the payload (for PUT)
  uint64_t size = 0;
};

// Append-only write-ahead log (spec Milestone 7). Records are length-prefixed
// and CRC32-checked so a torn tail from a crash mid-append is detected and
// ignored on replay. Appends are fsync'd and serialized across threads.
class Wal {
public:
  static StatusOr<std::unique_ptr<Wal>> Open(const std::filesystem::path& path);

  // Reads every intact record from `path` (empty if the file does not exist).
  // Stops at the first torn/partial record rather than failing.
  static StatusOr<std::vector<WalRecord>> Replay(const std::filesystem::path& path);

  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  uint64_t NextSeq() { return next_seq_.fetch_add(1, std::memory_order_relaxed); }

  Status AppendBegin(uint64_t seq, WalOp op, std::string_view key, uint64_t version,
                     std::string_view checksum, uint64_t size);
  Status AppendCommit(uint64_t seq);

  // Discards all records (checkpoint after recovery). Resets the sequence.
  Status Truncate();

private:
  Wal(int fd, std::filesystem::path path) : fd_(fd), path_(std::move(path)) {}
  Status AppendRecord(const WalRecord& rec);

  std::mutex mu_;
  int fd_;
  std::filesystem::path path_;
  std::atomic<uint64_t> next_seq_{1};
};

} // namespace dos

#endif // DOS_STORAGE_WAL_H_
