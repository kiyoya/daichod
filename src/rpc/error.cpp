#include "rpc/error.h"

#include "engine/worker.h"
#include "google/rpc/status.pb.h"

namespace daichod {

namespace shim = daicho::shim::v1;

namespace {

grpc::Status PackStatus(grpc::StatusCode grpc_code, shim::ErrorCode code,
                        const std::string& message,
                        const std::string& context) {
  shim::ErrorDetail detail;
  detail.set_code(code);
  detail.set_engine_message(message);
  detail.set_context(context);
  google::rpc::Status rpc_status;
  rpc_status.set_code(static_cast<int>(grpc_code));
  rpc_status.set_message(message);
  rpc_status.add_details()->PackFrom(detail);
  return grpc::Status(grpc_code, message, rpc_status.SerializeAsString());
}

}  // namespace

grpc::StatusCode GrpcCodeFor(shim::ErrorCode code) {
  switch (code) {
    case shim::ACCOUNT_NOT_FOUND:
    case shim::TXN_NOT_FOUND:
    case shim::COMMODITY_NOT_FOUND:
      return grpc::StatusCode::NOT_FOUND;
    case shim::UNBALANCED_TRANSACTION:
    case shim::CURRENCY_MISMATCH:
    case shim::ACCOUNT_NOT_EMPTY:
    case shim::READ_ONLY_BOOK:
    case shim::INDETERMINATE_MUTATION:
      return grpc::StatusCode::FAILED_PRECONDITION;
    case shim::BOOK_LOCKED:
    case shim::BOOK_NOT_OPEN:
      return grpc::StatusCode::UNAVAILABLE;
    case shim::INVALID_ARGUMENT_DETAIL:
      return grpc::StatusCode::INVALID_ARGUMENT;
    case shim::ENGINE_ERROR:
    default:
      return grpc::StatusCode::INTERNAL;
  }
}

shim::ErrorDetail ShimError::ToDetail() const {
  shim::ErrorDetail detail;
  detail.set_code(code_);
  detail.set_engine_message(engine_message_);
  detail.set_context(context_);
  return detail;
}

grpc::Status ShimError::ToStatus() const {
  return PackStatus(GrpcCodeFor(code_), code_, engine_message_, context_);
}

grpc::Status StatusFromException(const std::exception& e) {
  if (const auto* shim_error = dynamic_cast<const ShimError*>(&e)) {
    return shim_error->ToStatus();
  }
  // Transport-level backpressure, not book state: no contract error code.
  if (dynamic_cast<const QueueFullError*>(&e) != nullptr) {
    return PackStatus(grpc::StatusCode::RESOURCE_EXHAUSTED,
                      shim::ERROR_CODE_UNSPECIFIED, e.what(), "");
  }
  if (dynamic_cast<const WorkerStoppedError*>(&e) != nullptr) {
    return PackStatus(grpc::StatusCode::UNAVAILABLE,
                      shim::ERROR_CODE_UNSPECIFIED, e.what(), "");
  }
  // Non-ShimError exceptions are internal bugs; still emit a typed detail so
  // the client never has to parse free text.
  return ShimError(shim::ENGINE_ERROR, e.what()).ToStatus();
}

}  // namespace daichod
