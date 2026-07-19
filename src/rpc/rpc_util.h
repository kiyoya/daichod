#pragma once

#include <exception>
#include <utility>

#include <grpcpp/support/status.h>

#include "engine/worker.h"
#include "rpc/error.h"

namespace daichod {

// Runs fn on the engine thread and converts every exception into the typed
// gRPC status. All unary handlers funnel through here so the error contract
// is enforced in exactly one place.
template <typename Fn>
grpc::Status RunRpc(EngineWorker* worker, Fn&& fn) {
  try {
    worker->Run(std::forward<Fn>(fn));
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    return StatusFromException(e);
  }
}

}  // namespace daichod
