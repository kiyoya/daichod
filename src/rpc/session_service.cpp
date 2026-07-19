#include "rpc/session_service.h"

#include <Account.h>
#include <qof.h>

#include "daichod_version.h"
#include "engine/map.h"
#include "rpc/rpc_util.h"

namespace daichod {

namespace shim = daicho::shim::v1;

// gnc-version.h is an internal (noinst) GnuCash header not installed to
// /opt/gnucash/include/gnucash (see engine/session.cpp for the same
// situation). gnc_version() is still exported by libgnc-core-utils, so
// declare it locally.
extern "C" {
const char* gnc_version(void);
}

void SessionServiceImpl::FillBookInfo(shim::BookInfo* info) {
  QofBook* book = session_->book();
  info->set_root_account_guid(
      GuidToString(qof_instance_get_guid(session_->root_account())));
  info->set_backend_uri(RedactUri(session_->config().book_uri));
  info->set_engine_version(gnc_version());
  info->set_shim_version(DAICHOD_VERSION);
  info->set_read_only(session_->config().read_only ||
                      qof_book_is_readonly(book));
  // default_currency is intentionally left unset: the engine has no
  // book-level currency; choosing one would be policy, which lives upstairs.
}

grpc::Status SessionServiceImpl::OpenBook(grpc::ServerContext*,
                                          const shim::Empty*,
                                          shim::BookInfo* response) {
  return RunRpc(worker_, [this, response] {
    session_->Open();
    FillBookInfo(response);
  });
}

grpc::Status SessionServiceImpl::CloseBook(grpc::ServerContext*,
                                           const shim::Empty*, shim::Empty*) {
  return RunRpc(worker_, [this] { session_->Close(); });
}

grpc::Status SessionServiceImpl::GetBookInfo(grpc::ServerContext*,
                                             const shim::Empty*,
                                             shim::BookInfo* response) {
  return RunRpc(worker_, [this, response] { FillBookInfo(response); });
}

grpc::Status SessionServiceImpl::Ping(grpc::ServerContext*, const shim::Empty*,
                                      shim::PingResponse* response) {
  // Deliberately not routed through the engine queue: Ping reports queue
  // depth and must answer even when the queue is saturated.
  response->set_queue_depth(static_cast<int64_t>(worker_->queue_depth()));
  response->set_uptime_seconds(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start_time_)
          .count());
  response->set_book_open(session_->is_open());
  return grpc::Status::OK;
}

grpc::Status SessionServiceImpl::ListIndeterminateMutations(
    grpc::ServerContext*, const shim::Empty*,
    shim::IndeterminateMutations* response) {
  return RunRpc(worker_, [this, response] {
    for (const Journal::IndeterminateEntry& entry :
         journal_->ListIndeterminate()) {
      shim::IndeterminateMutations::Entry* out = response->add_entries();
      out->set_mutation_id(entry.mutation_id);
      out->set_rpc_name(entry.rpc_name);
      out->set_payload_sha256(entry.payload_sha256);
      out->set_journaled_at_utc(entry.journaled_at_utc);
    }
  });
}

}  // namespace daichod
