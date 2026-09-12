#ifndef DOS_CONSENSUS_RAFT_STORAGE_H_
#define DOS_CONSENSUS_RAFT_STORAGE_H_

#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "common/status.h"
#include "consensus/raft_types.h"

namespace dos {
namespace consensus {

// Crash-safe durable backing for a Raft node (spec Milestone 15, disk-persisted
// variant). Two files under a per-node directory:
//
//   raft-state   the hard state (currentTerm, votedFor), rewritten atomically
//                (write temp + fsync + rename) on every change.
//   raft-log     the replicated log: length-prefixed, CRC-32-framed entries,
//                fsync'd on append. A torn tail from a crash mid-append is
//                detected by the CRC and dropped on load (like the WAL, M7).
//
// The node owns the in-memory log vector and drives persistence through this
// class; on restart it reloads via LoadedHardState()/LoadedLog(). This is what
// makes "no committed metadata mutation is lost" hold across a full restart,
// not just a leader failover.
class RaftStorage {
public:
  // Opens (creating if needed) the state directory and loads existing state.
  static StatusOr<std::unique_ptr<RaftStorage>> Open(const std::filesystem::path& dir);

  ~RaftStorage();
  RaftStorage(const RaftStorage&) = delete;
  RaftStorage& operator=(const RaftStorage&) = delete;

  // State recovered at Open (empty log / zero term on a fresh directory).
  const HardState& LoadedHardState() const { return loaded_hard_; }
  const std::vector<LogEntry>& LoadedLog() const { return loaded_log_; }

  // Durably persist the hard state (atomic rewrite + fsync).
  Status SaveHardState(const HardState& hs);

  // Append one entry to the log and fsync.
  Status AppendEntry(const LogEntry& entry);

  // Replace the entire on-disk log with `entries` (used when a follower must
  // drop conflicting suffix entries, paper §5.3). Atomic rewrite + fsync.
  Status RewriteLog(const std::vector<LogEntry>& entries);

private:
  RaftStorage(std::filesystem::path dir, int log_fd)
      : dir_(std::move(dir)), log_path_(dir_ / "raft-log"), meta_path_(dir_ / "raft-state"),
        log_fd_(log_fd) {}

  static Status WriteFileAtomic(const std::filesystem::path& path, const std::string& bytes);
  Status WriteAll(const char* p, std::size_t n); // to log_fd_, then fsync

  std::mutex mu_;
  std::filesystem::path dir_;
  std::filesystem::path log_path_;
  std::filesystem::path meta_path_;
  int log_fd_ = -1;

  HardState loaded_hard_;
  std::vector<LogEntry> loaded_log_;
};

} // namespace consensus
} // namespace dos

#endif // DOS_CONSENSUS_RAFT_STORAGE_H_
