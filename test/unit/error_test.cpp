#include "rpc/error.h"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "google/rpc/status.pb.h"
#include "shim.pb.h"

namespace daichod {
namespace {

namespace shim = daicho::shim::v1;

TEST(ErrorTest, GrpcCodeForMappings) {
  EXPECT_EQ(GrpcCodeFor(shim::ACCOUNT_NOT_FOUND), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(GrpcCodeFor(shim::BOOK_LOCKED), grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(GrpcCodeFor(shim::UNBALANCED_TRANSACTION),
            grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(GrpcCodeFor(shim::INVALID_ARGUMENT_DETAIL),
            grpc::StatusCode::INVALID_ARGUMENT);
}

// Extracts the single packed ErrorDetail from a status's
// grpc-status-details-bin payload (a serialized google.rpc.Status).
shim::ErrorDetail UnpackDetail(const grpc::Status& status) {
  google::rpc::Status rpc_status;
  EXPECT_TRUE(rpc_status.ParseFromString(status.error_details()));
  EXPECT_EQ(rpc_status.details_size(), 1);
  shim::ErrorDetail detail;
  EXPECT_TRUE(rpc_status.details(0).UnpackTo(&detail));
  return detail;
}

TEST(ErrorTest, ToStatusRoundTrip) {
  const ShimError error(shim::ACCOUNT_NOT_FOUND, "no such account",
                        "guid=3d1c5e0cf3244e33aeb1dc327e16ca0f");
  const grpc::Status status = error.ToStatus();

  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(status.error_message(), "no such account");

  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::ACCOUNT_NOT_FOUND);
  EXPECT_EQ(detail.engine_message(), "no such account");
  EXPECT_EQ(detail.context(), "guid=3d1c5e0cf3244e33aeb1dc327e16ca0f");
}

TEST(ErrorTest, ToStatusRoundTripPreservesEmptyContext) {
  const ShimError error(shim::BOOK_LOCKED, "backend locked");
  const grpc::Status status = error.ToStatus();

  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);

  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::BOOK_LOCKED);
  EXPECT_EQ(detail.engine_message(), "backend locked");
  EXPECT_EQ(detail.context(), "");
}

TEST(ErrorTest, StatusFromExceptionRuntimeErrorIsInternalEngineError) {
  const std::runtime_error error("boom");
  const grpc::Status status = StatusFromException(error);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(status.error_message(), "boom");

  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::ENGINE_ERROR);
  EXPECT_EQ(detail.engine_message(), "boom");
}

TEST(ErrorTest, StatusFromExceptionShimErrorUsesMappedCode) {
  const ShimError error(shim::UNBALANCED_TRANSACTION, "splits do not sum to zero");
  const grpc::Status status = StatusFromException(error);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::UNBALANCED_TRANSACTION);
}

}  // namespace
}  // namespace daichod
