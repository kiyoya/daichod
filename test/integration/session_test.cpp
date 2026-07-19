#include <gtest/gtest.h>

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
  EXPECT_FALSE(info.shim_version().empty());
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

TEST_F(DaemonFixture, SecondInstanceFailsLoudly) {
  Connect();
  // Same socket, same book: the flock must reject it, quickly and nonzero.
  const int exit_code =
      RunCommand({daichod_bin_, "--book-uri", book_uri_, "--socket",
                  socket_path_.string()});
  EXPECT_NE(exit_code, 0);
}

}  // namespace
}  // namespace daichod::testing
