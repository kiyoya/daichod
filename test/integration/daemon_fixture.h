#pragma once

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "shim.grpc.pb.h"

namespace daichod::testing {

// Spawns a daichod process against a fresh sqlite3 book in a temp dir and
// tears it down with SIGTERM. Binary locations come from the environment
// (set by CTest): DAICHOD_BIN, DAICHOD_MKBOOK.
class DaemonFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* daichod_bin = std::getenv("DAICHOD_BIN");
    const char* mkbook_bin = std::getenv("DAICHOD_MKBOOK");
    ASSERT_NE(daichod_bin, nullptr) << "DAICHOD_BIN not set";
    ASSERT_NE(mkbook_bin, nullptr) << "DAICHOD_MKBOOK not set";

    dir_ = std::filesystem::path(::testing::TempDir()) /
           ("daichod-it-" + std::to_string(getpid()) + "-" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir_);
    book_path_ = dir_ / "book.gnucash";
    socket_path_ = dir_ / "daichod.sock";
    book_uri_ = "sqlite3://" + book_path_.string();

    ASSERT_EQ(RunCommand({mkbook_bin, book_uri_}), 0) << "mkbook failed";
    StartDaemon(daichod_bin);
  }

  void TearDown() override {
    StopDaemon();
    std::filesystem::remove_all(dir_);
  }

  // extra_args is appended verbatim after the fixed --book-uri/--socket
  // pair (e.g. {"--backup-dir", dir} for lifecycle tests).
  void StartDaemon(const std::string& binary,
                   const std::vector<std::string>& extra_env = {},
                   const std::vector<std::string>& extra_args = {}) {
    daemon_pid_ = fork();
    ASSERT_GE(daemon_pid_, 0);
    if (daemon_pid_ == 0) {
      for (const std::string& assignment : extra_env) {
        const auto eq = assignment.find('=');
        setenv(assignment.substr(0, eq).c_str(),
               assignment.substr(eq + 1).c_str(), 1);
      }
      std::vector<std::string> arg_storage = {
          binary, "--book-uri", book_uri_, "--socket", socket_path_.string()};
      arg_storage.insert(arg_storage.end(), extra_args.begin(),
                         extra_args.end());
      std::vector<char*> args;
      args.reserve(arg_storage.size() + 1);
      for (std::string& arg : arg_storage) args.push_back(arg.data());
      args.push_back(nullptr);
      execv(binary.c_str(), args.data());
      _exit(127);
    }
    daichod_bin_ = binary;
  }

  void StopDaemon() {
    if (daemon_pid_ <= 0) return;
    kill(daemon_pid_, SIGTERM);
    int status = 0;
    waitpid(daemon_pid_, &status, 0);
    daemon_pid_ = -1;
  }

  // Simulates a crash: no SIGTERM handling, no lock/session teardown, so
  // whatever the engine's gnclock row and daichod's own locks looked like at
  // the moment of the kill is exactly what a restarted daemon has to cope
  // with.
  void KillDaemonHard() {
    if (daemon_pid_ <= 0) return;
    kill(daemon_pid_, SIGKILL);
    int status = 0;
    waitpid(daemon_pid_, &status, 0);
    daemon_pid_ = -1;
  }

  // For crash tests: the daemon has already died on its own (a crash-point
  // abort() observed as an RPC failure) and just needs reaping, not another
  // signal. Blocks until it has exited; returns its wait status.
  int WaitForExit() {
    if (daemon_pid_ <= 0) return -1;
    int status = 0;
    waitpid(daemon_pid_, &status, 0);
    daemon_pid_ = -1;
    return status;
  }

  // Waits for the daemon's Ping to answer; fails the test on timeout.
  std::shared_ptr<grpc::Channel> Connect() {
    auto channel = grpc::CreateChannel("unix:" + socket_path_.string(),
                                       grpc::InsecureChannelCredentials());
    auto stub = daicho::shim::v1::SessionService::NewStub(channel);
    for (int attempt = 0; attempt < 100; ++attempt) {
      grpc::ClientContext context;
      daicho::shim::v1::Empty request;
      daicho::shim::v1::PingResponse response;
      if (stub->Ping(&context, request, &response).ok()) return channel;
      usleep(100 * 1000);
    }
    ADD_FAILURE() << "daemon never answered Ping on " << socket_path_;
    return channel;
  }

  static int RunCommand(const std::vector<std::string>& argv) {
    const pid_t pid = fork();
    if (pid == 0) {
      std::vector<char*> args;
      args.reserve(argv.size() + 1);
      for (const std::string& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
      }
      args.push_back(nullptr);
      execv(args[0], args.data());
      _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

  std::filesystem::path dir_;
  std::filesystem::path book_path_;
  std::filesystem::path socket_path_;
  std::string book_uri_;
  std::string daichod_bin_;
  pid_t daemon_pid_ = -1;
};

}  // namespace daichod::testing
