#include "engine/session.h"

#include <utility>

#include <Account.h>
#include <gnc-engine.h>
#include <qof.h>

#include "rpc/error.h"

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
      return ShimError(shim::BOOK_LOCKED, message, uri);
    case ERR_BACKEND_READONLY:
    case ERR_BACKEND_PERM:
      return ShimError(shim::READ_ONLY_BOOK, message, uri);
    default:
      return ShimError(shim::ENGINE_ERROR, message, uri);
  }
}

void ThrowOnSessionError(QofSession* session, const std::string& uri) {
  const QofBackendError error = qof_session_get_error(session);
  if (error == ERR_BACKEND_NO_ERR) return;
  throw BackendError(error, qof_session_get_error_message(session), uri);
}

}  // namespace

void Session::GlobalInit() {
  gnc_environment_setup();
  gnc_filepath_init();
  qof_log_init();
  qof_log_set_level("", QOF_LOG_WARNING);
  gnc_engine_init(0, nullptr);
}

void Session::GlobalShutdown() { gnc_engine_shutdown(); }

Session::~Session() {
  if (session_ != nullptr) Close();
}

void Session::Open() {
  if (session_ != nullptr) return;  // administrative reopen is idempotent

  if (!HasSupportedScheme(config_.book_uri)) {
    throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                    "unsupported book URI scheme; only sqlite3:// and "
                    "postgres:// books are crash-safe",
                    config_.book_uri);
  }

  QofSession* session = qof_session_new(qof_book_new());
  qof_session_begin(session, config_.book_uri.c_str(),
                    config_.read_only ? SESSION_READ_ONLY
                                      : SESSION_NORMAL_OPEN);
  QofBackendError error = qof_session_get_error(session);
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
  open_.store(false, std::memory_order_relaxed);
  if (!config_.read_only) {
    qof_session_save(session_, nullptr);
    ThrowOnSessionError(session_, config_.book_uri);
  }
  qof_session_end(session_);
  qof_session_destroy(session_);
  session_ = nullptr;
}

QofBook* Session::book() const {
  if (session_ == nullptr) {
    throw ShimError(shim::BOOK_NOT_OPEN, "no book is open");
  }
  return qof_session_get_book(session_);
}

::Account* Session::root_account() const {
  return gnc_book_get_root_account(book());
}

}  // namespace daichod
