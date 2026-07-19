#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "daichod_version.h"
#include "engine/session.h"
#include "engine/worker.h"
#include "journal/journal.h"
#include "rpc/account_service.h"
#include "rpc/commodity_service.h"
#include "rpc/session_service.h"
#include "rpc/transaction_service.h"

namespace daichod {
namespace {

struct Options {
  std::string book_uri;
  std::string socket_path;
  std::string journal_path;
  bool read_only = false;
  std::size_t queue_depth = 256;
};

// Startup failures must be machine-readable: one JSON object on stderr,
// nonzero exit. The supervisor (daicho-api) parses this.
[[noreturn]] void Fail(const std::string& reason, const std::string& detail) {
  std::fprintf(stderr, "{\"error\":\"%s\",\"detail\":\"%s\"}\n",
               reason.c_str(), detail.c_str());
  std::exit(1);
}

Options ParseOptions(int argc, char** argv) {
  static const option kLongOptions[] = {
      {"book-uri", required_argument, nullptr, 'b'},
      {"socket", required_argument, nullptr, 's'},
      {"journal", required_argument, nullptr, 'j'},
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
    static constexpr char kSqlitePrefix[] = "sqlite3://";
    if (options.book_uri.rfind(kSqlitePrefix, 0) == 0) {
      options.journal_path =
          options.book_uri.substr(sizeof(kSqlitePrefix) - 1) +
          ".daichod-journal";
    } else {
      Fail("usage", "--journal is required for non-sqlite3 books");
    }
  }
  return options;
}

// flock on <socket>.lock plus a pidfile. Together with the engine's own book
// lock this makes a second instance on the same book fail loudly at startup.
void AcquireLocks(const std::string& socket_path) {
  const std::string lock_path = socket_path + ".lock";
  const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd < 0) Fail("lock_open_failed", lock_path);
  if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    Fail("already_running", "another daichod holds " + lock_path);
  }
  // lock_fd stays open (and locked) for the life of the process.

  const std::string pid_path = socket_path + ".pid";
  FILE* pid_file = std::fopen(pid_path.c_str(), "w");
  if (pid_file == nullptr) Fail("pidfile_open_failed", pid_path);
  std::fprintf(pid_file, "%d\n", getpid());
  std::fclose(pid_file);

  // The flock proves no live owner; a leftover socket file is stale.
  unlink(socket_path.c_str());
}

int Run(const Options& options) {
  AcquireLocks(options.socket_path);

  EngineWorker worker(options.queue_depth);
  Session session(
      Session::Config{options.book_uri, options.read_only});

  // Startup order per DESIGN.md: locks (above) → sidecar journal → book.
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
  // Signals are blocked on every thread and handled synchronously here.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  std::thread signal_thread([&signals, &server] {
    int received = 0;
    sigwait(&signals, &received);
    server->Shutdown();
  });

  std::fprintf(stderr, "daichod %s serving %s on %s\n", DAICHOD_VERSION,
               options.book_uri.c_str(), options.socket_path.c_str());
  server->Wait();

  // In-flight handlers have finished; run the close on the engine thread,
  // then drain whatever is still queued.
  try {
    worker.Run([&session] { session.Close(); });
  } catch (const std::exception& e) {
    std::fprintf(stderr, "{\"error\":\"close_failed\",\"detail\":\"%s\"}\n",
                 e.what());
  }
  worker.Drain();
  unlink(options.socket_path.c_str());

  // Wake and join the signal thread if shutdown came from elsewhere.
  kill(getpid(), SIGTERM);
  signal_thread.join();
  return 0;
}

}  // namespace
}  // namespace daichod

int main(int argc, char** argv) {
  return daichod::Run(daichod::ParseOptions(argc, argv));
}
