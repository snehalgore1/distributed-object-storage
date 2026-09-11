#include "storage/sqlite_metadata_store.h"

#include <sqlite3.h>

#include <string>

namespace dos {
namespace {

// Prepared-statement guard: finalizes on scope exit.
class Stmt {
public:
  Stmt() = default;
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Status Prepare(sqlite3* db, std::string_view sql) {
    int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
      return Status::IoError(std::string("prepare failed: ") + sqlite3_errmsg(db));
    }
    return Status::Ok();
  }

  sqlite3_stmt* get() const { return stmt_; }

private:
  sqlite3_stmt* stmt_ = nullptr;
};

void BindText(sqlite3_stmt* s, int idx, std::string_view v) {
  sqlite3_bind_text(s, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

// Columns must be selected in this order:
//   key, size, checksum, version, created_at, deleted, last_request_id
constexpr const char* kSelectColumns =
    "key, size, checksum, version, created_at, deleted, last_request_id";

ObjectMetadata ReadRow(sqlite3_stmt* q) {
  ObjectMetadata meta;
  meta.key = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
  meta.size = static_cast<uint64_t>(sqlite3_column_int64(q, 1));
  meta.checksum = reinterpret_cast<const char*>(sqlite3_column_text(q, 2));
  meta.version = static_cast<uint64_t>(sqlite3_column_int64(q, 3));
  meta.created_at = sqlite3_column_int64(q, 4);
  meta.deleted = sqlite3_column_int(q, 5) != 0;
  meta.request_id = reinterpret_cast<const char*>(sqlite3_column_text(q, 6));
  return meta;
}

} // namespace

StatusOr<std::unique_ptr<SqliteMetadataStore>>
SqliteMetadataStore::Open(const std::filesystem::path& db_path) {
  sqlite3* db = nullptr;
  // FULLMUTEX = serialized threading mode: the single connection is safe to use
  // concurrently from multiple threads (spec Milestone 2). App-level sharded
  // locks still guard the version read/write sequence per key.
  int rc =
      sqlite3_open_v2(db_path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = db ? sqlite3_errmsg(db) : "unknown";
    sqlite3_close(db);
    return Status::IoError("sqlite open failed: " + msg);
  }

  // WAL journaling gives durable, crash-safe commits for metadata rows.
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

  static constexpr const char* kSchema = "CREATE TABLE IF NOT EXISTS objects ("
                                         "  key TEXT PRIMARY KEY,"
                                         "  size BIGINT NOT NULL,"
                                         "  checksum TEXT NOT NULL,"
                                         "  version BIGINT NOT NULL,"
                                         "  created_at BIGINT NOT NULL,"
                                         "  deleted INTEGER NOT NULL DEFAULT 0,"
                                         "  last_request_id TEXT NOT NULL DEFAULT ''"
                                         ");";
  char* err = nullptr;
  rc = sqlite3_exec(db, kSchema, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : "schema failed";
    sqlite3_free(err);
    sqlite3_close(db);
    return Status::IoError("sqlite schema failed: " + msg);
  }

  return std::unique_ptr<SqliteMetadataStore>(new SqliteMetadataStore(db));
}

SqliteMetadataStore::~SqliteMetadataStore() { sqlite3_close(db_); }

Status SqliteMetadataStore::Put(const ObjectMetadata& meta) {
  Stmt stmt;
  Status s = stmt.Prepare(
      db_, "INSERT INTO objects(key, size, checksum, version, created_at, deleted, last_request_id)"
           " VALUES(?,?,?,?,?,?,?)"
           " ON CONFLICT(key) DO UPDATE SET"
           "  size=excluded.size, checksum=excluded.checksum,"
           "  version=excluded.version, created_at=excluded.created_at,"
           "  deleted=excluded.deleted, last_request_id=excluded.last_request_id;");
  if (!s.ok()) {
    return s;
  }
  sqlite3_stmt* q = stmt.get();
  BindText(q, 1, meta.key);
  sqlite3_bind_int64(q, 2, static_cast<sqlite3_int64>(meta.size));
  BindText(q, 3, meta.checksum);
  sqlite3_bind_int64(q, 4, static_cast<sqlite3_int64>(meta.version));
  sqlite3_bind_int64(q, 5, meta.created_at);
  sqlite3_bind_int(q, 6, meta.deleted ? 1 : 0);
  BindText(q, 7, meta.request_id);
  if (sqlite3_step(q) != SQLITE_DONE) {
    return Status::IoError(std::string("put failed: ") + sqlite3_errmsg(db_));
  }
  return Status::Ok();
}

StatusOr<ObjectMetadata> SqliteMetadataStore::Get(std::string_view key) {
  Stmt stmt;
  Status s =
      stmt.Prepare(db_, std::string("SELECT ") + kSelectColumns + " FROM objects WHERE key=?;");
  if (!s.ok()) {
    return s;
  }
  sqlite3_stmt* q = stmt.get();
  BindText(q, 1, key);
  int rc = sqlite3_step(q);
  if (rc == SQLITE_DONE) {
    return Status::NotFound(std::string("no such key: ") + std::string(key));
  }
  if (rc != SQLITE_ROW) {
    return Status::IoError(std::string("get failed: ") + sqlite3_errmsg(db_));
  }
  ObjectMetadata meta = ReadRow(q);
  if (meta.deleted) {
    return Status::NotFound(std::string("tombstoned: ") + std::string(key));
  }
  return meta;
}

StatusOr<ObjectMetadata> SqliteMetadataStore::Peek(std::string_view key) {
  // Like Get but returns tombstoned rows too (the write path needs the current
  // version and request_id even when the key is logically deleted).
  Stmt stmt;
  Status s =
      stmt.Prepare(db_, std::string("SELECT ") + kSelectColumns + " FROM objects WHERE key=?;");
  if (!s.ok()) {
    return s;
  }
  sqlite3_stmt* q = stmt.get();
  BindText(q, 1, key);
  int rc = sqlite3_step(q);
  if (rc == SQLITE_DONE) {
    return Status::NotFound(std::string("no such key: ") + std::string(key));
  }
  if (rc != SQLITE_ROW) {
    return Status::IoError(std::string("peek failed: ") + sqlite3_errmsg(db_));
  }
  return ReadRow(q);
}

StatusOr<uint64_t> SqliteMetadataStore::CurrentVersion(std::string_view key) {
  Stmt stmt;
  Status s = stmt.Prepare(db_, "SELECT version FROM objects WHERE key=?;");
  if (!s.ok()) {
    return s;
  }
  sqlite3_stmt* q = stmt.get();
  BindText(q, 1, key);
  int rc = sqlite3_step(q);
  if (rc == SQLITE_DONE) {
    return uint64_t{0};
  }
  if (rc != SQLITE_ROW) {
    return Status::IoError(std::string("version lookup failed: ") + sqlite3_errmsg(db_));
  }
  return static_cast<uint64_t>(sqlite3_column_int64(q, 0));
}

Status SqliteMetadataStore::Delete(std::string_view key) {
  // Only tombstone rows that currently exist and are not already deleted.
  Stmt stmt;
  Status s = stmt.Prepare(db_, "UPDATE objects SET deleted=1 WHERE key=? AND deleted=0;");
  if (!s.ok()) {
    return s;
  }
  sqlite3_stmt* q = stmt.get();
  BindText(q, 1, key);
  if (sqlite3_step(q) != SQLITE_DONE) {
    return Status::IoError(std::string("delete failed: ") + sqlite3_errmsg(db_));
  }
  if (sqlite3_changes(db_) == 0) {
    return Status::NotFound(std::string("no such key: ") + std::string(key));
  }
  return Status::Ok();
}

StatusOr<std::vector<ObjectMetadata>> SqliteMetadataStore::List(std::string_view prefix) {
  Stmt stmt;
  Status s = stmt.Prepare(db_, std::string("SELECT ") + kSelectColumns +
                                   " FROM objects WHERE deleted=0 AND key >= ? AND key < ?"
                                   " ORDER BY key;");
  if (!s.ok()) {
    return s;
  }
  // Range scan [prefix, prefix++) so the primary-key index is used.
  std::string lo(prefix);
  std::string hi(prefix);
  while (!hi.empty() && static_cast<unsigned char>(hi.back()) == 0xff) {
    hi.pop_back();
  }
  if (hi.empty()) {
    // Empty prefix (or all-0xff): scan everything.
    hi = std::string(1, '\xff');
    if (prefix.empty()) {
      lo.clear();
    }
  } else {
    hi.back() = static_cast<char>(static_cast<unsigned char>(hi.back()) + 1);
  }
  sqlite3_stmt* q = stmt.get();
  BindText(q, 1, lo);
  BindText(q, 2, hi);

  std::vector<ObjectMetadata> out;
  int rc;
  while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
    out.push_back(ReadRow(q));
  }
  if (rc != SQLITE_DONE) {
    return Status::IoError(std::string("list failed: ") + sqlite3_errmsg(db_));
  }
  return out;
}

} // namespace dos
