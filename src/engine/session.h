#pragma once

#include <atomic>
#include <string>

extern "C" {
typedef struct QofSessionImpl QofSession;
typedef struct QofBook QofBook;
typedef struct account_s Account;
}

namespace daichod {

// Wraps the engine's QofSession for exactly one book. Everything here must
// run on the engine thread; the class does not enforce that itself — the
// worker's serialization is the concurrency model.
class Session {
 public:
  struct Config {
    std::string book_uri;
    bool read_only = false;
  };

  // One-time process-wide engine initialization (QOF, object registration,
  // backend modules). Must precede the first Open in this process.
  static void GlobalInit();
  static void GlobalShutdown();

  explicit Session(Config config) : config_(std::move(config)) {}
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Opens and loads the configured book. Refuses XML-backed books: only
  // sqlite3:// and postgres:// URIs persist engine commits incrementally,
  // which the mutation protocol's crash story depends on. Idempotent —
  // opening an already-open session is a no-op (OpenBook is administrative
  // reopen). Throws ShimError on any failure.
  void Open();

  // Saves pending state, releases the engine lock, destroys the session.
  // Idempotent. Teardown (releasing the lock, destroying the session) always
  // completes even when the save fails; the save error is rethrown only
  // after, so a failed Close() still leaves the session closed and safe to
  // reopen with Open() rather than wedged.
  void Close();

  // Safe to call from any thread (Ping must answer even when the engine
  // queue is saturated); everything else here is engine-thread only.
  bool is_open() const { return open_.load(std::memory_order_relaxed); }

  // Throw ShimError{BOOK_NOT_OPEN} when the session is closed.
  QofBook* book() const;
  ::Account* root_account() const;

  // Startup integrity gate: every transaction balances in its currency and
  // the per-currency trial balance is zero. Detection only — the shim never
  // repairs a book; a failed check throws and the daemon exits nonzero.
  void RunStartupChecks() const;

  const Config& config() const { return config_; }

 private:
  Config config_;
  QofSession* session_ = nullptr;
  std::atomic<bool> open_{false};
};

}  // namespace daichod
