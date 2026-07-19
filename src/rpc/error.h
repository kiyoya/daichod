#pragma once

#include <exception>
#include <string>
#include <utility>

#include <grpcpp/support/status.h>

#include "shim.pb.h"

namespace daichod {

// Canonical gRPC status code for each contract error code. One mapping,
// used both when an error is first raised and when a recorded failure is
// replayed for a duplicate mutation_id, so both responses are identical.
grpc::StatusCode GrpcCodeFor(daicho::shim::v1::ErrorCode code);

// The one error currency inside the daemon. Thrown anywhere on the engine
// thread or in RPC decoding, caught once at the gRPC boundary, and rendered
// as a status whose grpc-status-details-bin carries a google.rpc.Status
// wrapping daicho.shim.v1.ErrorDetail. The engine's message is always
// preserved verbatim: the shim classifies, it never interprets.
class ShimError : public std::exception {
 public:
  ShimError(daicho::shim::v1::ErrorCode code, std::string engine_message,
            std::string context = "")
      : code_(code),
        engine_message_(std::move(engine_message)),
        context_(std::move(context)) {}

  explicit ShimError(const daicho::shim::v1::ErrorDetail& detail)
      : ShimError(detail.code(), detail.engine_message(), detail.context()) {}

  const char* what() const noexcept override { return engine_message_.c_str(); }

  daicho::shim::v1::ErrorCode code() const { return code_; }
  const std::string& engine_message() const { return engine_message_; }
  const std::string& context() const { return context_; }

  daicho::shim::v1::ErrorDetail ToDetail() const;
  grpc::Status ToStatus() const;

 private:
  daicho::shim::v1::ErrorCode code_;
  std::string engine_message_;
  std::string context_;
};

// Renders any exception (ShimError or otherwise) as the gRPC status to send.
grpc::Status StatusFromException(const std::exception& e);

}  // namespace daichod
