#include "rpc/commodity_service.h"

#include <map>
#include <string>
#include <utility>

#include <Account.h>
#include <Query.h>
#include <Split.h>
#include <Transaction.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <gnc-pricedb.h>
#include <qofquery.h>

#include "engine/map.h"
#include "rpc/mutation.h"
#include "rpc/rpc_util.h"

namespace daichod {

namespace shim = daicho::shim::v1;

namespace {

void CommodityToProto(const gnc_commodity* commodity, shim::Commodity* out) {
  CommodityIdToProto(commodity, out->mutable_id());
  const char* fullname = gnc_commodity_get_fullname(commodity);
  out->set_fullname(fullname != nullptr ? fullname : "");
  out->set_fraction(gnc_commodity_get_fraction(commodity));
  out->set_is_currency(gnc_commodity_is_currency(commodity));
}

void PriceToProto(GNCPrice* price, shim::Price* out) {
  out->set_guid(GuidToString(gnc_price_get_guid(price)));
  CommodityIdToProto(gnc_price_get_commodity(price), out->mutable_commodity());
  CommodityIdToProto(gnc_price_get_currency(price), out->mutable_currency());
  const GDate date = time64_to_gdate(gnc_price_get_time64(price));
  if (g_date_valid(&date)) {
    DateToProto(date, out->mutable_date());
  }
  NumericToProto(gnc_price_get_value(price), out->mutable_value());
  const char* source = gnc_price_get_source_string(price);
  out->set_source(source != nullptr ? source : "");
}

}  // namespace

grpc::Status CommodityServiceImpl::ListCommodities(
    grpc::ServerContext*, const shim::Empty*, shim::CommodityList* response) {
  return RunRpc(worker_, [this, response] {
    gnc_commodity_table* table =
        gnc_commodity_table_get_table(session_->book());
    GList* namespaces = gnc_commodity_table_get_namespaces(table);
    for (GList* ns = namespaces; ns != nullptr; ns = ns->next) {
      GList* commodities = gnc_commodity_table_get_commodities(
          table, static_cast<const char*>(ns->data));
      for (GList* node = commodities; node != nullptr; node = node->next) {
        CommodityToProto(static_cast<gnc_commodity*>(node->data),
                         response->add_commodities());
      }
      g_list_free(commodities);
    }
    g_list_free(namespaces);
  });
}

grpc::Status CommodityServiceImpl::GetPrices(grpc::ServerContext*,
                                             const shim::GetPricesRequest* request,
                                             shim::PriceList* response) {
  return RunRpc(worker_, [this, request, response] {
    QofBook* book = session_->book();
    gnc_commodity* commodity =
        FindCommodity(book, request->commodity(), "commodity");
    gnc_commodity* currency =
        request->has_currency()
            ? FindCommodity(book, request->currency(), "currency")
            : nullptr;

    time64 from = 0;
    time64 to = 0;
    if (request->has_date_from()) {
      const GDate d = DateFromProto(request->date_from(), "date_from");
      from = gnc_dmy2time64(g_date_get_day(&d), g_date_get_month(&d),
                            g_date_get_year(&d));
    }
    if (request->has_date_to()) {
      const GDate d = DateFromProto(request->date_to(), "date_to");
      to = gnc_dmy2time64_end(g_date_get_day(&d), g_date_get_month(&d),
                              g_date_get_year(&d));
    }

    GNCPriceDB* db = gnc_pricedb_get_db(book);
    PriceList* prices = gnc_pricedb_get_prices(db, commodity, currency);
    for (GList* node = prices; node != nullptr; node = node->next) {
      auto* price = static_cast<GNCPrice*>(node->data);
      const time64 stamp = gnc_price_get_time64(price);
      if (request->has_date_from() && stamp < from) continue;
      if (request->has_date_to() && stamp > to) continue;
      PriceToProto(price, response->add_prices());
    }
    gnc_price_list_destroy(prices);
  });
}

grpc::Status CommodityServiceImpl::AddPrice(grpc::ServerContext*,
                                            const shim::AddPriceRequest* request,
                                            shim::Price* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.CommodityService/AddPrice", response,
      [this, request](shim::Price* out) {
        const shim::Price& spec = request->price();
        if (!spec.guid().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "guid must be empty on create; the engine assigns",
                          "price.guid");
        }
        QofBook* book = session_->book();
        gnc_commodity* commodity =
            FindCommodity(book, spec.commodity(), "price.commodity");
        gnc_commodity* currency =
            FindCommodity(book, spec.currency(), "price.currency");
        if (!gnc_commodity_is_currency(currency)) {
          throw ShimError(shim::CURRENCY_MISMATCH,
                          "price currency is not a currency commodity",
                          "price.currency");
        }
        if (!spec.has_date()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL, "date is required",
                          "price.date");
        }
        const GDate date = DateFromProto(spec.date(), "price.date");
        const gnc_numeric value =
            NumericFromProto(spec.value(), "price.value");

        GNCPrice* price = gnc_price_create(book);
        gnc_price_begin_edit(price);
        gnc_price_set_commodity(price, commodity);
        gnc_price_set_currency(price, currency);
        gnc_price_set_time64(price,
                             gnc_dmy2time64_neutral(g_date_get_day(&date),
                                                    g_date_get_month(&date),
                                                    g_date_get_year(&date)));
        gnc_price_set_value(price, value);
        gnc_price_set_source_string(
            price, spec.source().empty() ? "shim:api" : spec.source().c_str());
        gnc_price_commit_edit(price);
        gnc_pricedb_add_price(gnc_pricedb_get_db(book), price);
        PriceToProto(price, out);
        gnc_price_unref(price);
      });
}

grpc::Status BalanceServiceImpl::GetBalance(grpc::ServerContext*,
                                            const shim::GetBalanceRequest* request,
                                            shim::Balance* response) {
  return RunRpc(worker_, [this, request, response] {
    QofBook* book = session_->book();
    ::Account* account =
        FindAccount(book, request->account_guid(), "account_guid");

    GDate as_of;
    if (request->has_as_of()) {
      as_of = DateFromProto(request->as_of(), "as_of");
    } else {
      // "Latest" means all recorded activity, future-dated included.
      g_date_clear(&as_of, 1);
      g_date_set_dmy(&as_of, 31, G_DATE_DECEMBER, 9999);
    }
    const time64 stamp =
        gnc_dmy2time64_end(g_date_get_day(&as_of), g_date_get_month(&as_of),
                           g_date_get_year(&as_of));

    gnc_commodity* target =
        request->has_convert_to()
            ? FindCommodity(book, request->convert_to(), "convert_to")
            : xaccAccountGetCommodity(account);
    if (target == nullptr) {
      throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                      "account has no commodity and convert_to is unset",
                      request->account_guid());
    }

    const gnc_numeric balance = xaccAccountGetBalanceAsOfDateInCurrency(
        account, stamp, target, request->include_children());
    NumericToProto(balance, response->mutable_amount());
    CommodityIdToProto(target, response->mutable_commodity());
    if (request->has_as_of()) {
      *response->mutable_as_of() = request->as_of();
    } else {
      DateToProto(as_of, response->mutable_as_of());
    }
  });
}

grpc::Status BalanceServiceImpl::GetTrialBalance(grpc::ServerContext*,
                                                 const shim::Empty*,
                                                 shim::TrialBalance* response) {
  return RunRpc(worker_, [this, response] {
    // Per-currency sum of split values over every transaction in the book.
    // Zero everywhere is the definition of a healthy double-entry book;
    // this is the health-check primitive, so it recomputes from scratch.
    QofQuery* query = qof_query_create_for(GNC_ID_SPLIT);
    qof_query_set_book(query, session_->book());
    std::map<std::pair<std::string, std::string>,
             std::pair<const gnc_commodity*, gnc_numeric>>
        sums;
    for (GList* node = qof_query_run(query); node != nullptr;
         node = node->next) {
      auto* split = static_cast<::Split*>(node->data);
      const gnc_commodity* currency =
          xaccTransGetCurrency(xaccSplitGetParent(split));
      if (currency == nullptr) continue;
      const auto key =
          std::make_pair(std::string(gnc_commodity_get_namespace(currency)),
                         std::string(gnc_commodity_get_mnemonic(currency)));
      auto [it, inserted] = sums.try_emplace(
          key, std::make_pair(currency, gnc_numeric_zero()));
      it->second.second =
          gnc_numeric_add(it->second.second, xaccSplitGetValue(split),
                          GNC_DENOM_AUTO, GNC_HOW_DENOM_LCD);
    }
    qof_query_destroy(query);
    for (const auto& [key, entry] : sums) {
      shim::TrialBalance::Entry* out = response->add_entries();
      CommodityIdToProto(entry.first, out->mutable_currency());
      NumericToProto(entry.second, out->mutable_sum());
    }
  });
}

}  // namespace daichod
