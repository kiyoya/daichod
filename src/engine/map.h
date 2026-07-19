#pragma once

#include <string>

#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <gnc-commodity.h>
#include <gnc-numeric.h>
#include <guid.h>
#include <qofbook.h>

#include "shim.pb.h"

namespace daichod {

// proto <-> engine conversions and by-GUID lookups. Lookups throw the
// matching *_NOT_FOUND ShimError; conversions throw
// INVALID_ARGUMENT_DETAIL. `context` names the offending field or GUID for
// ErrorDetail.context.

// ----------------------------------------------------------------- guids
std::string GuidToString(const GncGUID* guid);
GncGUID StringToGuid(const std::string& text, const std::string& context);

// Strips any password from a backend URI (postgres://user:pw@host/db) so
// GetBookInfo never echoes credentials.
std::string RedactUri(const std::string& uri);

// -------------------------------------------------------------- numerics
gnc_numeric NumericFromProto(const daicho::shim::v1::Numeric& numeric,
                             const std::string& context);
void NumericToProto(gnc_numeric value, daicho::shim::v1::Numeric* out);

// ----------------------------------------------------------------- dates
GDate DateFromProto(const daicho::shim::v1::Date& date,
                    const std::string& context);
void DateToProto(const GDate& date, daicho::shim::v1::Date* out);

// ----------------------------------------------------------- commodities
gnc_commodity* FindCommodity(QofBook* book,
                             const daicho::shim::v1::CommodityId& id,
                             const std::string& context);
void CommodityIdToProto(const gnc_commodity* commodity,
                        daicho::shim::v1::CommodityId* out);

// -------------------------------------------------------------- accounts
::Account* FindAccount(QofBook* book, const std::string& guid,
                       const std::string& context);
GNCAccountType AccountTypeFromProto(daicho::shim::v1::AccountType type,
                                    const std::string& context);
daicho::shim::v1::AccountType AccountTypeToProto(GNCAccountType type);
void AccountToProto(::Account* account, daicho::shim::v1::Account* out);

// ---------------------------------------------------- transactions/splits
::Transaction* FindTransaction(QofBook* book, const std::string& guid,
                               const std::string& context);
char ReconcileStateFromProto(daicho::shim::v1::ReconcileState state,
                             const std::string& context);
daicho::shim::v1::ReconcileState ReconcileStateToProto(char flag);
void SplitToProto(::Split* split, daicho::shim::v1::Split* out);
void TransactionToProto(::Transaction* transaction,
                        daicho::shim::v1::Transaction* out);

}  // namespace daichod
