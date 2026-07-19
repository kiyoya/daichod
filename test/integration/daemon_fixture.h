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

  void StartDaemon(const std::string& binary,
                   const std::vector<std::string>& extra_env = {}) {
    daemon_pid_ = fork();
    ASSERT_GE(daemon_pid_, 0);
    if (daemon_pid_ == 0) {
      for (const std::string& assignment : extra_env) {
        const auto eq = assignment.find('=');
        setenv(assignment.substr(0, eq).c_str(),
               assignment.substr(eq + 1).c_str(), 1);
      }
      execl(binary.c_str(), binary.c_str(), "--book-uri", book_uri_.c_str(),
            "--socket", socket_path_.c_str(), static_cast<char*>(nullptr));
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
