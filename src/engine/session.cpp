#include "engine/session.h"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <utility>

#include <Account.h>
#include <Query.h>
#include <Split.h>
#include <Transaction.h>
#include <glib.h>
#include <gnc-engine.h>
#include <qof.h>
#include <sqlite3.h>

#include "engine/map.h"
#include "rpc/error.h"
#include "util/fail_point.h"

namespace daichod {

// gnc-environment.h and gnc-filepath-utils.h are internal (noinst) GnuCash
// headers: upstream's own CMakeLists.txt marks them noinst and does not
// install them, so they are absent from /opt/gnucash/include/gnucash. The
// symbols themselves are exported by libgnc-core-utils (already linked in
// via the gnucash_engine target), so declare the two entry points used here
// locally, matching their real (vendored source) signatures.
extern "C" {
void gnc_environment_setup(void);
char* gnc_filepath_init(void);
}

namespace shim = daicho::shim::v1;

namespace {

bool HasSupportedScheme(const std::string& uri) {
  // Only SQL-backed books persist engine commits incrementally; XML books
  // save whole-file and are refused rather than pretending to be crash-safe.
  return uri.rfind("sqlite3://", 0) == 0 || uri.rfind("postgres://", 0) == 0;
}

// Local filesystem path for sqlite3:// books; empty otherwise. Mirrors
// main.cpp's Options::book_path() idiom.
std::string Sqlite3Path(const std::string& uri) {
  static constexpr char kPrefix[] = "sqlite3://";
  return uri.rfind(kPrefix, 0) == 0 ? uri.substr(sizeof(kPrefix) - 1)
                                    : std::string();
}

// Classifies a backend error; the engine's own message text (when the
// session provides one) rides along verbatim.
ShimError BackendError(QofBackendError error, const char* engine_message,
                       const std::string& uri) {
  const std::string message =
      engine_message != nullptr && engine_message[0] != '\0'
          ? engine_message
          : "backend error " + std::to_string(static_cast<int>(error));
  switch (error) {
    case ERR_BACKEND_LOCKED:
      return ShimError(shim::ERROR_CODE_BOOK_LOCKED, message, uri);
    case ERR_BACKEND_READONLY:
    case ERR_BACKEND_PERM:
      return ShimError(shim::ERROR_CODE_READ_ONLY_BOOK, message, uri);
    default:
      return ShimError(shim::ERROR_CODE_ENGINE_ERROR, message, uri);
  }
}

void ThrowOnSessionError(QofSession* session, const std::string& uri) {
  const QofBackendError error = qof_session_get_error(session);
  if (error == ERR_BACKEND_NO_ERR) return;
  throw BackendError(error, qof_session_get_error_message(session), uri);
}

}  // namespace

bool GncLockIsProvablyStale(const std::string& book_uri) {
  const std::string path = Sqlite3Path(book_uri);
  if (path.empty()) return false;  // postgres: no local gnclock to inspect

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }
  sqlite3_busy_timeout(db, 200);

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT Hostname, PID FROM gnclock", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return false;  // e.g. no gnclock table: ambiguous, refuse to break
  }

  // Same accessor GnuCash's own SQL backend uses to write the row, so a
  // match here means the same host wrote it (not merely a lookalike from
  // gethostname(2), which can disagree with GLib's cached/normalized name).
  const char* this_host = g_get_host_name();
  bool provably_stale = true;
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const unsigned char* row_host = sqlite3_column_text(stmt, 0);
    const auto row_pid = static_cast<pid_t>(sqlite3_column_int64(stmt, 1));
    const bool same_host =
        row_host != nullptr &&
        std::strcmp(reinterpret_cast<const char*>(row_host), this_host) == 0;
    const bool dead = kill(row_pid, 0) != 0 && errno == ESRCH;
    if (!(same_host && dead)) {
      provably_stale = false;
      break;
    }
  }
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) provably_stale = false;  // I/O

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return provably_stale;
}

void Session::GlobalInit() {
  gnc_environment_setup();
  gnc_filepath_init();
  qof_log_init();
  qof_log_set_level("", QOF_LOG_WARNING);
  gnc_engine_init(0, nullptr);
}

void Session::GlobalShutdown() { gnc_engine_shutdown(); }

Session::~Session() {
  // Destructor is implicitly noexcept; a real close belongs on the engine
  // thread via an explicit Close() call, so a save failure here is a
  // last-resort backstop, not the primary error-reporting path — log and
  // swallow rather than std::terminate the process.
  if (session_ == nullptr) return;
  try {
    Close();
  } catch (const std::exception& e) {
    std::fprintf(stderr,
                 "daichod: session close failed during destruction: %s\n",
                 e.what());
  } catch (...) {
    std::fprintf(stderr,
                 "daichod: session close failed during destruction: "
                 "unknown error\n");
  }
}

void Session::Open() {
  if (session_ != nullptr) return;  // administrative reopen is idempotent

  if (!HasSupportedScheme(config_.book_uri)) {
    throw ShimError(shim::ERROR_CODE_INVALID_ARGUMENT,
                    "unsupported book URI scheme; only sqlite3:// and "
                    "postgres:// books are crash-safe",
                    config_.book_uri);
  }

  QofSession* session = qof_session_new(qof_book_new());
  qof_session_begin(session, config_.book_uri.c_str(),
                    config_.read_only ? SESSION_READ_ONLY
                                      : SESSION_NORMAL_OPEN);
  QofBackendError error = qof_session_get_error(session);

  // The engine's own book-level lock is a row inside the book itself,
  // released only by a clean session end; a crash-point abort() never gets
  // there, so it outlives the process. daichod's book-identity flock
  // (AcquireLocks in main.cpp, taken before Open() is ever reached, keyed to
  // the book rather than --socket) is the authoritative check for a second
  // live daichod instance, so it rules out that possibility entirely — a
  // lock reported here is either this book's own stale lock from a prior
  // crash (break it, exactly as DESIGN.md promises: "a crash is a restart
  // ... never a recovery procedure") or a live writer that isn't daichod,
  // most likely desktop GnuCash open on the same file (refuse; breaking a
  // live editor's lock would give the book two writers). For sqlite3 books,
  // GncLockIsProvablyStale distinguishes the two by reading the lock row's
  // Hostname/PID; only a positive confirmation of death breaks it. postgres
  // books keep the unconditional break — checking a postgres gnclock row
  // would need libpq, which isn't linked here; a documented scope cut, not
  // an oversight. Retrying on the same session object is safe: a failed
  // begin() resets its internal URI/backend state.
  if (error == ERR_BACKEND_LOCKED && !config_.read_only) {
    const bool is_sqlite3 = config_.book_uri.rfind("sqlite3://", 0) == 0;
    if (!is_sqlite3 || GncLockIsProvablyStale(config_.book_uri)) {
      qof_session_begin(session, config_.book_uri.c_str(), SESSION_BREAK_LOCK);
      error = qof_session_get_error(session);
    }
  }

  if (error != ERR_BACKEND_NO_ERR) {
    ShimError shim_error =
        BackendError(error, qof_session_get_error_message(session),
                     config_.book_uri);
    qof_session_destroy(session);
    throw shim_error;
  }

  qof_session_load(session, nullptr);
  error = qof_session_get_error(session);
  if (error != ERR_BACKEND_NO_ERR) {
    ShimError shim_error =
        BackendError(error, qof_session_get_error_message(session),
                     config_.book_uri);
    qof_session_end(session);
    qof_session_destroy(session);
    throw shim_error;
  }

  session_ = session;
  open_.store(true, std::memory_order_relaxed);
}

void Session::Close() {
  if (session_ == nullptr) return;
  QofSession* session = session_;
  open_.store(false, std::memory_order_relaxed);

  // Teardown (end/destroy/session_ = nullptr) must complete before a save
  // error is allowed to propagate. Otherwise a failed save would leave
  // is_open() false with session_ still non-null: Open()'s reopen guard
  // treats non-null session_ as "already open" and no-ops forever, so the
  // session wedges with no RPC able to recover it, and a second failure on
  // the destructor's re-close would escape past this point too.
  std::exception_ptr save_error;
  if (!config_.read_only) {
    try {
      FailPointMaybe("session_close_save");
      qof_session_save(session, nullptr);
      ThrowOnSessionError(session, config_.book_uri);
    } catch (...) {
      save_error = std::current_exception();
    }
  }

  qof_session_end(session);
  qof_session_destroy(session);
  session_ = nullptr;

  if (save_error) std::rethrow_exception(save_error);
}

QofBook* Session::book() const {
  if (session_ == nullptr) {
    throw ShimError(shim::ERROR_CODE_BOOK_NOT_OPEN, "no book is open");
  }
  return qof_session_get_book(session_);
}

::Account* Session::root_account() const {
  return gnc_book_get_root_account(book());
}
void Session::RunStartupChecks() const {
  QofBook* current_book = book();
  QofQuery* query = qof_query_create_for(GNC_ID_SPLIT);
  qof_query_set_book(query, current_book);

  std::string imbalanced_txn;
  std::map<std::string, gnc_numeric> sums;
  for (GList* node = qof_query_run(query); node != nullptr;
       node = node->next) {
    auto* split = static_cast<::Split*>(node->data);
    ::Transaction* transaction = xaccSplitGetParent(split);
    if (imbalanced_txn.empty() &&
        !gnc_numeric_zero_p(xaccTransGetImbalanceValue(transaction))) {
      imbalanced_txn = GuidToString(qof_instance_get_guid(transaction));
    }
    const gnc_commodity* currency = xaccTransGetCurrency(transaction);
    if (currency == nullptr) continue;
    const std::string key =
        std::string(gnc_commodity_get_namespace(currency)) + ":" +
        gnc_commodity_get_mnemonic(currency);
    const auto [it, inserted] = sums.try_emplace(key, gnc_numeric_zero());
    it->second = gnc_numeric_add(it->second, xaccSplitGetValue(split),
                                 GNC_DENOM_AUTO, GNC_HOW_DENOM_LCD);
  }
  qof_query_destroy(query);

  if (!imbalanced_txn.empty()) {
    throw ShimError(shim::ERROR_CODE_ENGINE_ERROR,
                    "startup check failed: imbalanced transaction",
                    imbalanced_txn);
  }
  for (const auto& [currency, sum] : sums) {
    if (!gnc_numeric_zero_p(sum)) {
      throw ShimError(shim::ERROR_CODE_ENGINE_ERROR,
                      "startup check failed: trial balance is nonzero",
                      currency);
    }
  }
}


}  // namespace daichod
