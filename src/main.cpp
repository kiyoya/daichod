#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "daichod_version.h"
#include "engine/session.h"
#include "engine/worker.h"
#include "journal/journal.h"
#include "journal/recovery.h"
#include "rpc/account_service.h"
#include "rpc/commodity_service.h"
#include "rpc/session_service.h"
#include "rpc/transaction_service.h"
#include "util/backup.h"

namespace daichod {
namespace {

struct Options {
  std::string book_uri;
  std::string socket_path;
  std::string journal_path;
  std::string backup_dir;
  int backup_keep_days = 14;
  bool read_only = false;
  std::size_t queue_depth = 256;

  // Local filesystem path for sqlite3:// books; empty otherwise.
  std::string book_path() const {
    static constexpr char kPrefix[] = "sqlite3://";
    return book_uri.rfind(kPrefix, 0) == 0
               ? book_uri.substr(sizeof(kPrefix) - 1)
               : std::string();
  }
};

// Startup failures must be machine-readable: one JSON object on stderr,
// nonzero exit. The supervisor (daicho-api) parses this.
[[noreturn]] void Fail(const std::string& reason, const std::string& detail) {
  std::fprintf(stderr, "{\"error\":\"%s\",\"detail\":\"%s\"}\n",
               reason.c_str(), detail.c_str());
  std::exit(1);
}

// Lowercase hex, for turning a raw digest into a filesystem-safe lock name.
std::string HexEncode(const std::string& data) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(data.size() * 2);
  for (const unsigned char byte : data) {
    hex.push_back(kHexDigits[byte >> 4]);
    hex.push_back(kHexDigits[byte & 0x0F]);
  }
  return hex;
}

// Strips `user[:pass]@` from a URI's authority component so lock identity
// depends only on which book is addressed, not which credentials connect.
std::string StripCredentials(const std::string& uri) {
  const auto scheme_end = uri.find("://");
  if (scheme_end == std::string::npos) return uri;
  const auto authority_start = scheme_end + 3;
  const auto at_pos = uri.find('@', authority_start);
  if (at_pos == std::string::npos) return uri;
  const auto slash_pos = uri.find('/', authority_start);
  // An '@' past the authority (e.g. in a path/query) isn't credentials.
  if (slash_pos != std::string::npos && at_pos > slash_pos) return uri;
  return uri.substr(0, authority_start) + uri.substr(at_pos + 1);
}

Options ParseOptions(int argc, char** argv) {
  static const option kLongOptions[] = {
      {"book-uri", required_argument, nullptr, 'b'},
      {"socket", required_argument, nullptr, 's'},
      {"journal", required_argument, nullptr, 'j'},
      {"backup-dir", required_argument, nullptr, 'B'},
      {"backup-keep-days", required_argument, nullptr, 'K'},
      {"read-only", no_argument, nullptr, 'r'},
      {"queue-depth", required_argument, nullptr, 'q'},
      {"version", no_argument, nullptr, 'v'},
      {nullptr, 0, nullptr, 0}};

  Options options;
  int opt;
  while ((opt = getopt_long(argc, argv, "", kLongOptions, nullptr)) != -1) {
    switch (opt) {
      case 'b':
        options.book_uri = optarg;
        break;
      case 's':
        options.socket_path = optarg;
        break;
      case 'j':
        options.journal_path = optarg;
        break;
      case 'B':
        options.backup_dir = optarg;
        break;
      case 'K':
        options.backup_keep_days = std::stoi(optarg);
        break;
      case 'r':
        options.read_only = true;
        break;
      case 'q':
        options.queue_depth = static_cast<std::size_t>(std::stoul(optarg));
        break;
      case 'v':
        std::printf("daichod %s (gnucash %s)\n", DAICHOD_VERSION,
                    DAICHOD_GNUCASH_PINNED_VERSION);
        std::exit(0);
      default:
        Fail("usage",
             "daichod --book-uri <sqlite3://...> --socket </abs/path.sock> "
             "[--read-only] [--queue-depth N]");
    }
  }
  if (options.book_uri.empty() || options.socket_path.empty()) {
    Fail("usage", "--book-uri and --socket are required");
  }
  if (options.socket_path.front() != '/') {
    Fail("usage", "--socket must be an absolute path");
  }
  if (options.journal_path.empty()) {
    // The design's default: a sidecar next to the book. Only file-backed
    // books have a "next to"; anything else must say where explicitly.
    if (!options.book_path().empty()) {
      options.journal_path = options.book_path() + ".daichod-journal";
    } else {
      Fail("usage", "--journal is required for non-sqlite3 books");
    }
  }
  if (!options.backup_dir.empty() && options.book_path().empty()) {
    Fail("usage",
         "--backup-dir requires a sqlite3 book; database backups are a "
         "deployment concern");
  }
  return options;
}

// flock(lock_path) + pidfile at pid_path. On contention, exits via Fail with
// fail_reason (the caller's promise about what this particular lock means).
void AcquireOneLock(const std::string& lock_path, const std::string& pid_path,
                    const char* fail_reason) {
  const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd < 0) Fail("lock_open_failed", lock_path);
  if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    Fail(fail_reason, "another daichod holds " + lock_path);
  }
  // lock_fd stays open (and locked) for the life of the process.

  FILE* pid_file = std::fopen(pid_path.c_str(), "w");
  if (pid_file == nullptr) Fail("pidfile_open_failed", pid_path);
  std::fprintf(pid_file, "%d\n", getpid());
  std::fclose(pid_file);
}

// Two independent locks. The socket lock (<socket>.lock) only guards against
// a second instance reusing the same --socket path; two daichod processes on
// the same book with different --socket values sail past it and would both
// reach Session::Open with live engine locks. The book-identity lock is what
// actually makes "a second instance on the same book fails loudly" true
// regardless of --socket — for sqlite3 books, a path next to the book file;
// for postgres, a hashed-URI path next to the journal (journal_path is
// always set for postgres; ParseOptions enforces it), since there's no local
// book file to key off of. Credentials are stripped before hashing so lock
// identity doesn't depend on which account connects.
//
// This lock is also what justifies Session::Open's stale-lock inference: by
// the time Open() sees a still-locked engine, this flock has already proven
// no other daichod is alive on the book, so what remains to rule out is only
// whether some non-daichod writer (desktop GnuCash) holds it — see
// GncLockIsProvablyStale in engine/session.cpp.
void AcquireLocks(const Options& options) {
  AcquireOneLock(options.socket_path + ".lock", options.socket_path + ".pid",
                "already_running");

  std::string book_lock_path;
  if (!options.book_path().empty()) {
    book_lock_path = options.book_path() + ".daichod.lock";
  } else {
    const std::string digest =
        HexEncode(Sha256(StripCredentials(options.book_uri))).substr(0, 32);
    book_lock_path = (std::filesystem::path(options.journal_path)
                          .parent_path() /
                      (digest + ".daichod.lock"))
                         .string();
  }
  AcquireOneLock(book_lock_path, book_lock_path + ".pid", "book_already_open");

  // The socket lock above proves no live owner of this socket path; a
  // leftover socket file is stale.
  unlink(options.socket_path.c_str());
}

int Run(const Options& options) {
  // SIGTERM/SIGINT must be blocked before any other thread in this process
  // exists: POSIX delivers a process-directed signal to any thread that
  // hasn't blocked it, and every thread inherits its mask at creation time.
  // Blocking here, ahead of EngineWorker and the gRPC server threads, is
  // what guarantees the drain path below runs instead of default
  // termination. A signal arriving before signal_thread's sigwait() just
  // stays pending.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  AcquireLocks(options);

  // Startup order per DESIGN.md: locks → daily snapshot (files quiescent
  // before anything opens them) → sidecar journal → book → integrity
  // checks → pending-mutation reconciliation → report indeterminates →
  // serve.
  if (!options.backup_dir.empty()) {
    try {
      if (SnapshotBeforeOpen(options.book_path(), options.journal_path,
                             options.backup_dir, options.backup_keep_days)) {
        std::fprintf(stderr, "daichod: snapshotted book and journal to %s\n",
                     options.backup_dir.c_str());
      }
    } catch (const std::exception& e) {
      Fail("backup_failed", e.what());
    }
  }

  EngineWorker worker(options.queue_depth);
  Session session(
      Session::Config{options.book_uri, options.read_only});

  std::unique_ptr<Journal> journal;
  try {
    worker.Run([&journal, &options] {
      journal = Journal::Open(options.journal_path);
    });
  } catch (const std::exception& e) {
    Fail("journal_open_failed", e.what());
  }

  try {
    worker.Run([&session] {
      Session::GlobalInit();
      session.Open();
    });
  } catch (const std::exception& e) {
    Fail("book_open_failed", e.what());
  }

  try {
    worker.Run([&session] { session.RunStartupChecks(); });
  } catch (const std::exception& e) {
    Fail("startup_check_failed", e.what());
  }

  try {
    const std::size_t reconciled = worker.Run([&journal, &session] {
      return ReconcilePendingMutations(journal.get(), session.book());
    });
    if (reconciled > 0) {
      std::fprintf(stderr,
                   "daichod: %zu pending mutation(s) reconciled as applied\n",
                   reconciled);
    }
  } catch (const std::exception& e) {
    Fail("reconciliation_failed", e.what());
  }

  const std::size_t indeterminate_count =
      worker.Run([&journal] { return journal->ListIndeterminate().size(); });
  if (indeterminate_count > 0) {
    std::fprintf(stderr,
                 "daichod: %zu indeterminate mutation(s) await resolution\n",
                 indeterminate_count);
  }

  SessionServiceImpl session_service(&worker, &session, journal.get());
  AccountServiceImpl account_service(&worker, &session, journal.get());
  TransactionServiceImpl transaction_service(&worker, &session,
                                             journal.get());
  CommodityServiceImpl commodity_service(&worker, &session, journal.get());
  BalanceServiceImpl balance_service(&worker, &session);

  grpc::ServerBuilder builder;
  builder.AddListeningPort("unix:" + options.socket_path,
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&session_service);
  builder.RegisterService(&account_service);
  builder.RegisterService(&transaction_service);
  builder.RegisterService(&commodity_service);
  builder.RegisterService(&balance_service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (server == nullptr) Fail("listen_failed", options.socket_path);

  // SIGTERM/SIGINT: drain the queue, close the book cleanly, release locks.
  // Mask was set at the top of Run(), before any thread existed; handled
  // synchronously here.
  std::thread signal_thread([&signals, &server] {
    int received = 0;
    sigwait(&signals, &received);
    server->Shutdown();
  });

  std::fprintf(stderr, "daichod %s serving %s on %s\n", DAICHOD_VERSION,
               options.book_uri.c_str(), options.socket_path.c_str());
  server->Wait();

  // In-flight handlers have finished; run the close on the engine thread,
  // then drain whatever is still queued. A failed final save must not exit
  // 0 — the supervisor treats that as a clean shutdown.
  int exit_code = 0;
  try {
    worker.Run([&session] { session.Close(); });
  } catch (const std::exception& e) {
    std::fprintf(stderr, "{\"error\":\"close_failed\",\"detail\":\"%s\"}\n",
                 e.what());
    exit_code = 1;
  }
  worker.Drain();
  unlink(options.socket_path.c_str());

  // Wake and join the signal thread if shutdown came from elsewhere.
  kill(getpid(), SIGTERM);
  signal_thread.join();
  return exit_code;
}

}  // namespace
}  // namespace daichod

int main(int argc, char** argv) {
  return daichod::Run(daichod::ParseOptions(argc, argv));
}
