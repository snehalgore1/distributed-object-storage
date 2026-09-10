#ifndef DOS_STORAGE_SQLITE_METADATA_STORE_H_
#define DOS_STORAGE_SQLITE_METADATA_STORE_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/metadata_store.h"

struct sqlite3;

namespace dos {

// SQLite-backed MetadataStore (spec: "SQLite first"). Owns the connection and
// the `objects` table described in spec Section 15.1.
class SqliteMetadataStore : public MetadataStore {
public:
  // Opens (creating if needed) the database at `db_path` and ensures the schema
  // exists. Returns kIoError on failure.
  static StatusOr<std::unique_ptr<SqliteMetadataStore>> Open(const std::filesystem::path& db_path);

  ~SqliteMetadataStore() override;

  SqliteMetadataStore(const SqliteMetadataStore&) = delete;
  SqliteMetadataStore& operator=(const SqliteMetadataStore&) = delete;

  Status Put(const ObjectMetadata& meta) override;
  StatusOr<ObjectMetadata> Get(std::string_view key) override;
  StatusOr<uint64_t> CurrentVersion(std::string_view key) override;
  Status Delete(std::string_view key) override;
  StatusOr<std::vector<ObjectMetadata>> List(std::string_view prefix) override;

private:
  explicit SqliteMetadataStore(sqlite3* db) : db_(db) {}

  sqlite3* db_;
};

} // namespace dos

#endif // DOS_STORAGE_SQLITE_METADATA_STORE_H_
