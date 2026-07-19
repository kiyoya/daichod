#pragma once

#include "engine/session.h"
#include "engine/worker.h"
#include "journal/journal.h"
#include "shim.grpc.pb.h"

namespace daichod {

class TransactionServiceImpl final
    : public daicho::shim::v1::TransactionService::Service {
 public:
  TransactionServiceImpl(EngineWorker* worker, Session* session,
                         Journal* journal)
      : worker_(worker), session_(session), journal_(journal) {}

  grpc::Status GetTransaction(grpc::ServerContext* context,
                              const daicho::shim::v1::TxnRef* request,
                              daicho::shim::v1::Transaction* response) override;
  grpc::Status PostTransaction(
      grpc::ServerContext* context,
      const daicho::shim::v1::PostTransactionRequest* request,
      daicho::shim::v1::Transaction* response) override;
  grpc::Status UpdateTransaction(
      grpc::ServerContext* context,
      const daicho::shim::v1::UpdateTransactionRequest* request,
      daicho::shim::v1::Transaction* response) override;
  grpc::Status DeleteTransaction(
      grpc::ServerContext* context,
      const daicho::shim::v1::DeleteTransactionRequest* request,
      daicho::shim::v1::Empty* response) override;
  grpc::Status QuerySplits(
      grpc::ServerContext* context,
      const daicho::shim::v1::QuerySplitsRequest* request,
      daicho::shim::v1::QuerySplitsResponse* response) override;

 private:
  EngineWorker* worker_;
  Session* session_;
  Journal* journal_;
};

}  // namespace daichod
