#include "journal/journal.h"

#include <glib.h>
#include <sqlite3.h>

#include "rpc/error.h"

namespace daichod {

namespace shim = daicho::shim::v1;

namespace {

[[noreturn]] void ThrowSqlite(sqlite3* db, const std::string& context) {
  throw ShimError(shim::ERROR_CODE_ENGINE_ERROR,
                  std::string("journal: ") + sqlite3_errmsg(db), context);
}

void Exec(sqlite3* db, const char* sql) {
  char* error_message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error_message) != SQLITE_OK) {
    std::string message =
        error_message != nullptr ? error_message : "unknown sqlite error";
    sqlite3_free(error_message);
    throw ShimError(shim::ERROR_CODE_ENGINE_ERROR, "journal: " + message, sql);
  }
}

// RAII prepared statement; statements are short-lived, clarity over reuse.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      ThrowSqlite(db, sql);
    }
  }
  ~Stmt() { sqlite3_finalize(stmt_); }

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& BindText(int index, const std::string& value) {
    if (sqlite3_bind_text(stmt_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      ThrowSqlite(db_, "bind");
    }
    return *this;
  }
  Stmt& BindBlob(int index, const std::string& value) {
    if (sqlite3_bind_blob(stmt_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      ThrowSqlite(db_, "bind");
    }
    return *this;
  }
  Stmt& BindInt64(int index, int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
      ThrowSqlite(db_, "bind");
    }
    return *this;
  }

  // True while a row is available.
  bool Step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    ThrowSqlite(db_, "step");
  }

  std::string ColumnText(int index) {
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    return text != nullptr ? reinterpret_cast<const char*>(text) : "";
  }
  std::string ColumnBlob(int index) {
    const void* data = sqlite3_column_blob(stmt_, index);
    const int size = sqlite3_column_bytes(stmt_, index);
    return data != nullptr ? std::string(static_cast<const char*>(data),
                                         static_cast<size_t>(size))
                           : "";
  }
  int64_t ColumnInt64(int index) { return sqlite3_column_int64(stmt_, index); }

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace

std::string Sha256(const std::string& data) {
  guint8 digest[32];
  gsize digest_length = sizeof(digest);
  GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum,
                    reinterpret_cast<const guchar*>(data.data()),
                    static_cast<gssize>(data.size()));
  g_checksum_get_digest(checksum, digest, &digest_length);
  g_checksum_free(checksum);
  return std::string(reinterpret_cast<const char*>(digest), digest_length);
}

std::unique_ptr<Journal> Journal::Open(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    const std::string message =
        db != nullptr ? sqlite3_errmsg(db) : "cannot allocate sqlite handle";
    sqlite3_close(db);
    throw ShimError(shim::ERROR_CODE_ENGINE_ERROR, "journal: " + message, path);
  }
  auto journal = std::unique_ptr<Journal>(new Journal(db));
  // WAL keeps the fsync cost of the two-writes-per-mutation protocol sane;
  // synchronous=FULL is the "fsync" in the design's step 1 and step 4.
  Exec(db, "PRAGMA journal_mode=WAL");
  Exec(db, "PRAGMA synchronous=FULL");
  Exec(db, "PRAGMA foreign_keys=ON");
  Exec(db,
       "CREATE TABLE IF NOT EXISTS intents ("
       "  mutation_id TEXT PRIMARY KEY,"
       "  rpc_name TEXT NOT NULL,"
       "  payload BLOB NOT NULL,"
       "  payload_sha256 BLOB NOT NULL,"
       "  journaled_at_utc INTEGER NOT NULL,"
       "  pending_response BLOB)");
  Exec(db,
       "CREATE TABLE IF NOT EXISTS applied ("
       "  mutation_id TEXT PRIMARY KEY"
       "    REFERENCES intents(mutation_id),"
       "  outcome BLOB NOT NULL,"
       "  applied_at_utc INTEGER NOT NULL)");
  return journal;
}

Journal::~Journal() { sqlite3_close(db_); }

void Journal::RecordIntent(const std::string& mutation_id,
                           const std::string& rpc_name,
                           const std::string& payload, int64_t now_utc) {
  Stmt stmt(db_,
            "INSERT OR IGNORE INTO intents "
            "(mutation_id, rpc_name, payload, payload_sha256, "
            "journaled_at_utc) VALUES (?, ?, ?, ?, ?)");
  stmt.BindText(1, mutation_id)
      .BindText(2, rpc_name)
      .BindBlob(3, payload)
      .BindBlob(4, Sha256(payload))
      .BindInt64(5, now_utc);
  stmt.Step();
}

std::optional<internal::Outcome> Journal::GetOutcome(
    const std::string& mutation_id) {
  Stmt stmt(db_, "SELECT outcome FROM applied WHERE mutation_id = ?");
  stmt.BindText(1, mutation_id);
  if (!stmt.Step()) return std::nullopt;
  internal::Outcome outcome;
  if (!outcome.ParseFromString(stmt.ColumnBlob(0))) {
    throw ShimError(shim::ERROR_CODE_ENGINE_ERROR,
                    "journal: corrupt outcome record", mutation_id);
  }
  return outcome;
}

void Journal::RecordPending(const std::string& mutation_id,
                            const std::string& response_bytes) {
  Stmt stmt(db_,
            "UPDATE intents SET pending_response = ? WHERE mutation_id = ?");
  stmt.BindBlob(1, response_bytes).BindText(2, mutation_id);
  stmt.Step();
}

std::vector<Journal::PendingEntry> Journal::ListPendingUnresolved() {
  Stmt stmt(db_,
            "SELECT i.mutation_id, i.rpc_name, i.payload, i.pending_response "
            "FROM intents i LEFT JOIN applied a USING (mutation_id) "
            "WHERE a.mutation_id IS NULL AND i.pending_response IS NOT NULL "
            "ORDER BY i.journaled_at_utc");
  std::vector<PendingEntry> entries;
  while (stmt.Step()) {
    entries.push_back(PendingEntry{stmt.ColumnText(0), stmt.ColumnText(1),
                                   stmt.ColumnBlob(2), stmt.ColumnBlob(3)});
  }
  return entries;
}

void Journal::ClearPending(const std::string& mutation_id) {
  Stmt stmt(db_,
            "UPDATE intents SET pending_response = NULL "
            "WHERE mutation_id = ?");
  stmt.BindText(1, mutation_id);
  stmt.Step();
}

void Journal::RecordOutcome(const std::string& mutation_id,
                            const internal::Outcome& outcome,
                            int64_t now_utc) {
  Stmt stmt(db_,
            "INSERT INTO applied (mutation_id, outcome, applied_at_utc) "
            "VALUES (?, ?, ?)");
  stmt.BindText(1, mutation_id)
      .BindBlob(2, outcome.SerializeAsString())
      .BindInt64(3, now_utc);
  stmt.Step();
}

std::vector<Journal::IndeterminateEntry> Journal::ListIndeterminate() {
  Stmt stmt(db_,
            "SELECT i.mutation_id, i.rpc_name, i.payload_sha256, "
            "       i.journaled_at_utc "
            "FROM intents i LEFT JOIN applied a USING (mutation_id) "
            "WHERE a.mutation_id IS NULL ORDER BY i.journaled_at_utc");
  std::vector<IndeterminateEntry> entries;
  while (stmt.Step()) {
    entries.push_back(IndeterminateEntry{
        stmt.ColumnText(0), stmt.ColumnText(1), stmt.ColumnBlob(2),
        stmt.ColumnInt64(3)});
  }
  return entries;
}

}  // namespace daichod
