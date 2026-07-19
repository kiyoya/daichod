#pragma once

#include "engine/session.h"
#include "engine/worker.h"
#include "journal/journal.h"
#include "shim.grpc.pb.h"

namespace daichod {

class CommodityServiceImpl final
    : public daicho::shim::v1::CommodityService::Service {
 public:
  CommodityServiceImpl(EngineWorker* worker, Session* session,
                       Journal* journal)
      : worker_(worker), session_(session), journal_(journal) {}

  grpc::Status ListCommodities(
      grpc::ServerContext* context, const daicho::shim::v1::Empty* request,
      daicho::shim::v1::CommodityList* response) override;
  grpc::Status GetPrices(grpc::ServerContext* context,
                         const daicho::shim::v1::GetPricesRequest* request,
                         daicho::shim::v1::PriceList* response) override;
  grpc::Status AddPrice(grpc::ServerContext* context,
                        const daicho::shim::v1::AddPriceRequest* request,
                        daicho::shim::v1::Price* response) override;

 private:
  EngineWorker* worker_;
  Session* session_;
  Journal* journal_;
};

class BalanceServiceImpl final
    : public daicho::shim::v1::BalanceService::Service {
 public:
  BalanceServiceImpl(EngineWorker* worker, Session* session)
      : worker_(worker), session_(session) {}

  grpc::Status GetBalance(grpc::ServerContext* context,
                          const daicho::shim::v1::GetBalanceRequest* request,
                          daicho::shim::v1::Balance* response) override;
  grpc::Status GetTrialBalance(grpc::ServerContext* context,
                               const daicho::shim::v1::Empty* request,
                               daicho::shim::v1::TrialBalance* response) override;

 private:
  EngineWorker* worker_;
  Session* session_;
};

}  // namespace daichod
