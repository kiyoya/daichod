#pragma once

#include <chrono>

#include "engine/session.h"
#include "engine/worker.h"
#include "journal/journal.h"
#include "shim.grpc.pb.h"

namespace daichod {

class SessionServiceImpl final
    : public daicho::shim::v1::SessionService::Service {
 public:
  SessionServiceImpl(EngineWorker* worker, Session* session, Journal* journal)
      : worker_(worker),
        session_(session),
        journal_(journal),
        start_time_(std::chrono::steady_clock::now()) {}

  grpc::Status OpenBook(grpc::ServerContext* context,
                        const daicho::shim::v1::Empty* request,
                        daicho::shim::v1::BookInfo* response) override;
  grpc::Status CloseBook(grpc::ServerContext* context,
                         const daicho::shim::v1::Empty* request,
                         daicho::shim::v1::Empty* response) override;
  grpc::Status GetBookInfo(grpc::ServerContext* context,
                           const daicho::shim::v1::Empty* request,
                           daicho::shim::v1::BookInfo* response) override;
  grpc::Status Ping(grpc::ServerContext* context,
                    const daicho::shim::v1::Empty* request,
                    daicho::shim::v1::PingResponse* response) override;
  grpc::Status ListIndeterminateMutations(
      grpc::ServerContext* context, const daicho::shim::v1::Empty* request,
      daicho::shim::v1::IndeterminateMutations* response) override;

 private:
  void FillBookInfo(daicho::shim::v1::BookInfo* info);  // engine thread only

  EngineWorker* worker_;
  Session* session_;
  Journal* journal_;
  std::chrono::steady_clock::time_point start_time_;
};

}  // namespace daichod
