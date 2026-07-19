#pragma once

#include "engine/session.h"
#include "engine/worker.h"
#include "journal/journal.h"
#include "shim.grpc.pb.h"

namespace daichod {

class AccountServiceImpl final
    : public daicho::shim::v1::AccountService::Service {
 public:
  AccountServiceImpl(EngineWorker* worker, Session* session, Journal* journal)
      : worker_(worker), session_(session), journal_(journal) {}

  grpc::Status ListAccounts(grpc::ServerContext* context,
                            const daicho::shim::v1::Empty* request,
                            daicho::shim::v1::AccountList* response) override;
  grpc::Status GetAccount(grpc::ServerContext* context,
                          const daicho::shim::v1::AccountRef* request,
                          daicho::shim::v1::Account* response) override;
  grpc::Status CreateAccount(grpc::ServerContext* context,
                             const daicho::shim::v1::CreateAccountRequest* request,
                             daicho::shim::v1::Account* response) override;
  grpc::Status UpdateAccount(grpc::ServerContext* context,
                             const daicho::shim::v1::UpdateAccountRequest* request,
                             daicho::shim::v1::Account* response) override;
  grpc::Status DeleteAccount(grpc::ServerContext* context,
                             const daicho::shim::v1::DeleteAccountRequest* request,
                             daicho::shim::v1::Empty* response) override;
  grpc::Status GetReconcileInfo(grpc::ServerContext* context,
                                const daicho::shim::v1::AccountRef* request,
                                daicho::shim::v1::ReconcileInfo* response) override;
  grpc::Status SetReconcileInfo(
      grpc::ServerContext* context,
      const daicho::shim::v1::SetReconcileInfoRequest* request,
      daicho::shim::v1::ReconcileInfo* response) override;

 private:
  EngineWorker* worker_;
  Session* session_;
  Journal* journal_;
};

}  // namespace daichod
