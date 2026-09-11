#include "storage/local_object_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <set>
#include <shared_mutex>

#include "common/digest.h"
#include "common/sha256.h"
#include "storage/sqlite_metadata_store.h"

namespace dos {
namespace {

namespace fs = std::filesystem;

std::string ErrnoMsg(std::string_view what) {
  return std::string(what) + ": " + std::strerror(errno);
}

// fsync a directory so a preceding rename/create is durable.
Status FsyncDir(const fs::path& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IoError(ErrnoMsg("open dir for fsync"));
  }
  int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return Status::IoError(ErrnoMsg("fsync dir"));
  }
  return Status::Ok();
}

// Generates a unique staging filename within the tmp directory.
fs::path UniqueTmpPath(const fs::path& tmp_dir) {
  static std::atomic<uint64_t> counter{0};
  uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  std::string name = ".put-" + std::to_string(::getpid()) + "-" + std::to_string(::time(nullptr)) +
                     "-" + std::to_string(n) + ".tmp";
  return tmp_dir / name;
}

// Writes `data` durably to `final_path`: staged temp file -> fsync -> atomic
// rename -> fsync parent dir. Never leaves a partially written `final_path`.
Status WriteFileAtomic(const fs::path& tmp_dir, const fs::path& final_path, std::string_view data) {
  std::error_code ec;
  fs::create_directories(final_path.parent_path(), ec);
  if (ec) {
    return Status::IoError("create object dir: " + ec.message());
  }

  fs::path tmp = UniqueTmpPath(tmp_dir);
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::IoError(ErrnoMsg("open temp file"));
  }

  const char* p = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0) {
    ssize_t written = ::write(fd, p, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      ::unlink(tmp.c_str());
      return Status::IoError(ErrnoMsg("write temp file"));
    }
    p += written;
    remaining -= static_cast<std::size_t>(written);
  }

  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(tmp.c_str());
    return Status::IoError(ErrnoMsg("fsync temp file"));
  }
  if (::close(fd) != 0) {
    ::unlink(tmp.c_str());
    return Status::IoError(ErrnoMsg("close temp file"));
  }

  if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::IoError(ErrnoMsg("rename temp file"));
  }

  // Persist the rename by fsync'ing the containing directory.
  return FsyncDir(final_path.parent_path());
}

StatusOr<std::string> ReadWholeFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Status::IoError("open for read: " + path.string());
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return Status::IoError("read failed: " + path.string());
  }
  return data;
}

} // namespace

StatusOr<std::unique_ptr<LocalObjectStore>> LocalObjectStore::Open(const fs::path& root) {
  std::error_code ec;
  fs::create_directories(root / "data" / "tmp", ec);
  if (ec) {
    return Status::IoError("create data dir: " + ec.message());
  }
  fs::create_directories(root / "metadata", ec);
  if (ec) {
    return Status::IoError("create metadata dir: " + ec.message());
  }

  auto meta = SqliteMetadataStore::Open(root / "metadata" / "objects.sqlite");
  if (!meta.ok()) {
    return meta.status();
  }

  // Read whatever the WAL holds from a prior run before opening it for appends.
  const fs::path wal_path = root / "wal" / "node.wal";
  auto records = Wal::Replay(wal_path);
  if (!records.ok()) {
    return records.status();
  }
  auto wal = Wal::Open(wal_path);
  if (!wal.ok()) {
    return wal.status();
  }

  auto store = std::unique_ptr<LocalObjectStore>(
      new LocalObjectStore(root, std::move(meta).value(), std::move(wal).value()));

  Status r = store->Recover(records.value());
  if (!r.ok()) {
    return r;
  }
  return store;
}

Status LocalObjectStore::Recover(const std::vector<WalRecord>& records) {
  // A committed sequence means the metadata write already landed durably.
  std::set<uint64_t> committed;
  for (const auto& rec : records) {
    if (rec.is_commit) {
      committed.insert(rec.seq);
    }
  }

  for (const auto& rec : records) {
    if (rec.is_commit || committed.count(rec.seq) != 0) {
      continue; // completed operations are already reflected in metadata
    }
    // An incomplete operation: finish it if its effect is durably present,
    // otherwise discard it (it never became a committed version).
    auto peek = metadata_->Peek(rec.key);
    const uint64_t current = peek.ok() ? peek.value().version : 0;

    if (rec.op == WalOp::kPut) {
      if (peek.ok() && current >= rec.version) {
        continue; // metadata already at/after this version
      }
      auto data = ReadWholeFile(PhysicalPath(DataRoot(), KeyDigest(rec.key)));
      if (data.ok() && Sha256Hex(data.value()) == rec.checksum) {
        // Bytes are durably on disk and intact -> complete the commit.
        ObjectMetadata meta;
        meta.key = rec.key;
        meta.size = rec.size;
        meta.checksum = rec.checksum;
        meta.version = rec.version;
        meta.created_at = static_cast<int64_t>(::time(nullptr));
        meta.deleted = false;
        Status c = metadata_->Put(meta);
        if (!c.ok()) {
          return c;
        }
      }
      // else: payload missing/torn -> object never committed; leave it out.
    } else { // kDelete
      if (peek.ok() && !peek.value().deleted) {
        Status d = metadata_->Delete(rec.key);
        if (!d.ok() && d.code() != StatusCode::kNotFound) {
          return d;
        }
      }
    }
  }

  // Remove orphaned staging files from interrupted writes.
  std::error_code ec;
  if (fs::exists(TmpDir())) {
    for (const auto& entry : fs::directory_iterator(TmpDir(), ec)) {
      fs::remove(entry.path(), ec);
    }
  }

  // Checkpoint: metadata is now the source of truth, so the WAL can be reset.
  return wal_->Truncate();
}

StatusOr<ObjectMetadata> LocalObjectStore::Put(std::string_view key, std::string_view data) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  // Exclusive on this key's shard: serializes the version read/write below and
  // excludes concurrent readers of the same key.
  std::unique_lock<std::shared_mutex> lock(ShardFor(key));
  return DoPut(key, data, /*explicit_version=*/0, /*conditional=*/false, /*expected_version=*/0,
               /*request_id=*/"");
}

StatusOr<ObjectMetadata> LocalObjectStore::PutWithVersion(std::string_view key,
                                                          std::string_view data, uint64_t version) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  if (version == 0) {
    return Status::InvalidArgument("explicit version must be non-zero");
  }
  std::unique_lock<std::shared_mutex> lock(ShardFor(key));
  return DoPut(key, data, version, /*conditional=*/false, /*expected_version=*/0,
               /*request_id=*/"");
}

StatusOr<ObjectMetadata> LocalObjectStore::PutConditional(std::string_view key,
                                                          std::string_view data,
                                                          uint64_t expected_version,
                                                          std::string_view request_id) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }
  std::unique_lock<std::shared_mutex> lock(ShardFor(key));
  return DoPut(key, data, /*explicit_version=*/0, /*conditional=*/true, expected_version,
               request_id);
}

StatusOr<ObjectMetadata> LocalObjectStore::DoPut(std::string_view key, std::string_view data,
                                                 uint64_t explicit_version, bool conditional,
                                                 uint64_t expected_version,
                                                 std::string_view request_id) {
  const fs::path object_path = PhysicalPath(DataRoot(), KeyDigest(key));

  // Read the current row (including tombstones) once, for both idempotency and
  // the conditional version check.
  uint64_t current_version = 0;
  auto peek = metadata_->Peek(key);
  if (peek.ok()) {
    current_version = peek.value().version;
    // Idempotency: a non-empty request_id that already produced the current
    // version means this is a recognized retry -> return the committed result.
    if (!request_id.empty() && peek.value().request_id == std::string(request_id)) {
      return peek.value();
    }
  } else if (peek.status().code() != StatusCode::kNotFound) {
    return peek.status();
  }

  if (conditional && current_version != expected_version) {
    return Status::Conflict("version conflict for '" + std::string(key) + "': expected " +
                            std::to_string(expected_version) + ", have " +
                            std::to_string(current_version));
  }

  uint64_t version;
  if (explicit_version != 0) {
    version = explicit_version;
  } else if (conditional) {
    version = expected_version + 1;
  } else {
    version = current_version + 1;
  }

  ObjectMetadata meta;
  meta.key = std::string(key);
  meta.size = data.size();
  meta.checksum = Sha256Hex(data);
  meta.version = version;
  meta.created_at = static_cast<int64_t>(::time(nullptr));
  meta.deleted = false;
  meta.request_id = std::string(request_id);

  // 1) Log write intent to the WAL before any visible change (spec Milestone 7).
  const uint64_t seq = wal_->NextSeq();
  Status wb = wal_->AppendBegin(seq, WalOp::kPut, key, version, meta.checksum, meta.size);
  if (!wb.ok()) {
    return wb;
  }

  // 2) Stage + durably place the payload BEFORE committing metadata. A crash
  //    before the metadata Put leaves an orphan file; recovery completes the
  //    commit (bytes intact) or discards it (bytes missing/torn).
  Status w = WriteFileAtomic(TmpDir(), object_path, data);
  if (!w.ok()) {
    return w;
  }

  // 3) Commit metadata (the atomic visibility point).
  Status c = metadata_->Put(meta);
  if (!c.ok()) {
    return c;
  }

  // 4) Mark the WAL record complete. The object is already durable, so a failure
  //    here is non-fatal: recovery would see a begin-without-commit whose effect
  //    is already present and treat it as applied.
  wal_->AppendCommit(seq);
  return meta;
}

StatusOr<std::string> LocalObjectStore::Get(std::string_view key) {
  // Shared on this key's shard: many readers proceed together; a concurrent
  // Put/Delete to the same key is excluded.
  std::shared_lock<std::shared_mutex> lock(ShardFor(key));

  auto meta = metadata_->Get(key);
  if (!meta.ok()) {
    return meta.status(); // kNotFound for missing or tombstoned
  }

  const fs::path object_path = PhysicalPath(DataRoot(), KeyDigest(key));
  auto data = ReadWholeFile(object_path);
  if (!data.ok()) {
    return data.status();
  }

  // Integrity check: the committed checksum must match the bytes on disk.
  const std::string actual = Sha256Hex(data.value());
  if (actual != meta.value().checksum) {
    return Status::ChecksumMismatch("checksum mismatch for key: " + std::string(key));
  }
  return data;
}

StatusOr<ObjectMetadata> LocalObjectStore::Head(std::string_view key) {
  std::shared_lock<std::shared_mutex> lock(ShardFor(key));
  return metadata_->Get(key);
}

Status LocalObjectStore::Delete(std::string_view key) {
  std::unique_lock<std::shared_mutex> lock(ShardFor(key));
  const uint64_t seq = wal_->NextSeq();
  Status wb = wal_->AppendBegin(seq, WalOp::kDelete, key, 0, "", 0);
  if (!wb.ok()) {
    return wb;
  }
  Status s = metadata_->Delete(key);
  if (!s.ok()) {
    return s; // e.g. NotFound; recovery no-ops the begin-without-commit
  }
  wal_->AppendCommit(seq);
  return s;
}

StatusOr<std::vector<ObjectMetadata>> LocalObjectStore::List(std::string_view prefix) {
  return metadata_->List(prefix);
}

} // namespace dos
