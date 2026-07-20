#include <gtest/gtest.h>

#include <regex>

#include "daemon_fixture.h"

namespace daichod::testing {
namespace {

namespace shim = daicho::shim::v1;

TEST_F(DaemonFixture, PingReportsOpenBook) {
  auto stub = shim::SessionService::NewStub(Connect());
  grpc::ClientContext context;
  shim::Empty request;
  shim::PingResponse response;
  ASSERT_TRUE(stub->Ping(&context, request, &response).ok());
  EXPECT_TRUE(response.book_open());
  EXPECT_GE(response.queue_depth(), 0);
}

TEST_F(DaemonFixture, GetBookInfoReportsVersionsAndRoot) {
  auto stub = shim::SessionService::NewStub(Connect());
  grpc::ClientContext context;
  shim::Empty request;
  shim::BookInfo info;
  ASSERT_TRUE(stub->GetBookInfo(&context, request, &info).ok());
  EXPECT_EQ(info.root_account_guid().size(), 32u);
  EXPECT_FALSE(info.engine_version().empty());
  // The release scheme from CONTRACT.md: Major.YYYYMMDD.Iteration. The
  // Rust client refuses on a mismatched major, so shape is contractual.
  EXPECT_TRUE(std::regex_match(info.shim_version(),
                               std::regex(R"(\d+\.\d{8}\.\d+)")))
      << "shim_version: " << info.shim_version();
  EXPECT_FALSE(info.read_only());
  // Credentials never echo; this book has none, so the URI passes through.
  EXPECT_EQ(info.backend_uri(), book_uri_);
}

TEST_F(DaemonFixture, CloseAndReopenBook) {
  auto stub = shim::SessionService::NewStub(Connect());
  {
    grpc::ClientContext context;
    shim::Empty request, response;
    ASSERT_TRUE(stub->CloseBook(&context, request, &response).ok());
  }
  {
    grpc::ClientContext context;
    shim::Empty request;
    shim::BookInfo info;
    const grpc::Status status = stub->GetBookInfo(&context, request, &info);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
  }
  {
    grpc::ClientContext context;
    shim::Empty request;
    shim::BookInfo info;
    ASSERT_TRUE(stub->OpenBook(&context, request, &info).ok());
    EXPECT_EQ(info.root_account_guid().size(), 32u);
  }
}

TEST_F(DaemonFixture, CloseFailureAllowsReopen) {
  // A failed final save must not wedge the session: Close() completes
  // teardown before rethrowing, so session_ is null and Open() (via
  // OpenBook) is free to run again rather than silently no-op.
  StopDaemon();
  StartDaemon(daichod_bin_, {"DAICHOD_FAIL_AT=session_close_save"});
  auto stub = shim::SessionService::NewStub(Connect());

  {
    grpc::ClientContext context;
    shim::Empty request, response;
    EXPECT_FALSE(stub->CloseBook(&context, request, &response).ok());
  }
  {
    grpc::ClientContext context;
    shim::Empty request;
    shim::BookInfo info;
    ASSERT_TRUE(stub->OpenBook(&context, request, &info).ok());
  }
  {
    grpc::ClientContext context;
    shim::Empty request;
    shim::PingResponse response;
    ASSERT_TRUE(stub->Ping(&context, request, &response).ok());
    EXPECT_TRUE(response.book_open());
  }
}

TEST_F(DaemonFixture, SigtermExitsNonzeroWithoutAbortWhenSaveFails) {
  // The destructor's re-close on shutdown must never escape past Close():
  // a second save failure there logs and swallows rather than terminating,
  // and the daemon still exits nonzero so the supervisor sees the failure.
  StopDaemon();
  StartDaemon(daichod_bin_, {"DAICHOD_FAIL_AT=session_close_save"});
  Connect();

  kill(daemon_pid_, SIGTERM);
  const int status = WaitForExit();
  ASSERT_TRUE(WIFEXITED(status)) << "daemon did not exit cleanly (signaled: "
                                 << WIFSIGNALED(status) << ")";
  EXPECT_NE(WEXITSTATUS(status), 0);
}

TEST_F(DaemonFixture, SecondInstanceFailsLoudly) {
  Connect();
  // Same socket, same book: the flock must reject it, quickly and nonzero.
  const int exit_code =
      RunCommand({daichod_bin_, "--book-uri", book_uri_, "--socket",
                  socket_path_.string()});
  EXPECT_NE(exit_code, 0);
}

TEST_F(DaemonFixture, SecondInstanceDifferentSocketSameBookFailsLoudly) {
  Connect();
  // Different socket, same book: the socket-path flock alone would not
  // collide, but the book-identity lock must reject it regardless.
  const std::filesystem::path other_socket = dir_ / "other.sock";
  const int exit_code =
      RunCommand({daichod_bin_, "--book-uri", book_uri_, "--socket",
                  other_socket.string()});
  EXPECT_NE(exit_code, 0);
}

TEST_F(DaemonFixture, StaleGncLockFromDeadPidIsBroken) {
  // Establish that the book is open under the first daemon, then take it
  // out hard enough that the engine's gnclock row is never released — the
  // same crash story a real restart-on-failure recovers from.
  Connect();
  KillDaemonHard();
  StartDaemon(daichod_bin_);

  auto stub = shim::SessionService::NewStub(Connect());
  grpc::ClientContext context;
  shim::Empty request;
  shim::PingResponse response;
  ASSERT_TRUE(stub->Ping(&context, request, &response).ok());
  EXPECT_TRUE(response.book_open());
}

}  // namespace
}  // namespace daichod::testing
