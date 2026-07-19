#pragma once

#include <ctime>
#include <string>
#include <utility>

#include <google/protobuf/message.h>
#include <grpcpp/support/status.h>

#include "engine/worker.h"
#include "journal/journal.h"
#include "rpc/error.h"
#include "util/crash_point.h"

namespace daichod {

// Rejects anything that is not a canonically formatted UUID
// (8-4-4-4-12 lowercase/uppercase hex). The journal keys on this string.
void ValidateMutationId(const std::string& mutation_id);

// The mutation protocol, in one place. Every mutating RPC runs through here:
//
//   1. journal the full request; fsync (Journal has synchronous=FULL)
//   2. a previously applied mutation_id returns its recorded outcome untouched
//   3. apply(response) on the engine thread — the callee wraps its work in
//      the engine's BeginEdit/CommitEdit so partial application is impossible
//   4. record outcome (success bytes or typed failure); fsync; respond
//
// Only deterministic results are recorded: a ShimError is an outcome, any
// other exception leaves the entry indeterminate for the recovery handshake.
template <typename Response, typename ApplyFn>
grpc::Status RunMutation(EngineWorker* worker, Journal* journal,
                         const daicho::shim::v1::MutationMeta& meta,
                         const google::protobuf::Message& request,
                         const std::string& rpc_name, Response* response,
                         ApplyFn&& apply) {
  try {
    worker->Run([&] {
      ValidateMutationId(meta.mutation_id());
      const int64_t now = static_cast<int64_t>(std::time(nullptr));

      journal->RecordIntent(meta.mutation_id(), rpc_name,
                            request.SerializeAsString(), now);
      CrashPointMaybe("after_intent");

      if (auto recorded = journal->GetOutcome(meta.mutation_id())) {
        if (!recorded->ok()) throw ShimError(recorded->error());
        if (!response->ParseFromString(recorded->response())) {
          throw ShimError(daicho::shim::v1::ENGINE_ERROR,
                          "journal: recorded outcome does not parse as " +
                              rpc_name + " response",
                          meta.mutation_id());
        }
        return;
      }

      internal::Outcome outcome;
      outcome.set_rpc_name(rpc_name);
      try {
        apply(response);
        CrashPointMaybe("after_apply");
        outcome.set_ok(true);
        outcome.set_response(response->SerializeAsString());
      } catch (const ShimError& shim_error) {
        outcome.set_ok(false);
        *outcome.mutable_error() = shim_error.ToDetail();
        journal->RecordOutcome(meta.mutation_id(), outcome, now);
        throw;
      }
      journal->RecordOutcome(meta.mutation_id(), outcome, now);
      CrashPointMaybe("after_outcome");
    });
    return grpc::Status::OK;
  } catch (const std::exception& e) {
    return StatusFromException(e);
  }
}

}  // namespace daichod
