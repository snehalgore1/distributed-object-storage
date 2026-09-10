#include "storage/local_object_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
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
  return std::unique_ptr<LocalObjectStore>(new LocalObjectStore(root, std::move(meta).value()));
}

StatusOr<ObjectMetadata> LocalObjectStore::Put(std::string_view key, std::string_view data) {
  if (key.empty()) {
    return Status::InvalidArgument("empty key");
  }

  // Exclusive on this key's shard: serializes the version read/write below and
  // excludes concurrent readers of the same key.
  std::unique_lock<std::shared_mutex> lock(ShardFor(key));

  const std::string digest = KeyDigest(key);
  const fs::path object_path = PhysicalPath(DataRoot(), digest);

  auto cur = metadata_->CurrentVersion(key);
  if (!cur.ok()) {
    return cur.status();
  }

  ObjectMetadata meta;
  meta.key = std::string(key);
  meta.size = data.size();
  meta.checksum = Sha256Hex(data);
  meta.version = cur.value() + 1;
  meta.created_at = static_cast<int64_t>(::time(nullptr));
  meta.deleted = false;

  // Stage + durably place the payload BEFORE committing metadata. If we crash
  // before the metadata Put, the object is invisible and gets overwritten on
  // the next successful write to this key.
  Status w = WriteFileAtomic(TmpDir(), object_path, data);
  if (!w.ok()) {
    return w;
  }

  Status c = metadata_->Put(meta);
  if (!c.ok()) {
    return c;
  }
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
  return metadata_->Delete(key);
}

StatusOr<std::vector<ObjectMetadata>> LocalObjectStore::List(std::string_view prefix) {
  return metadata_->List(prefix);
}

} // namespace dos
