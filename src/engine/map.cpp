#include "engine/map.h"

#include <gnc-date.h>

#include "rpc/error.h"

namespace daichod {

namespace shim = daicho::shim::v1;

std::string GuidToString(const GncGUID* guid) {
  char buffer[GUID_ENCODING_LENGTH + 1];
  guid_to_string_buff(guid, buffer);
  return std::string(buffer);
}

GncGUID StringToGuid(const std::string& text, const std::string& context) {
  GncGUID guid;
  if (!string_to_guid(text.c_str(), &guid)) {
    throw ShimError(shim::ERROR_CODE_INVALID_ARGUMENT, "malformed GUID: " + text,
                    context);
  }
  return guid;
}

std::string RedactUri(const std::string& uri) {
  // scheme://user:password@rest -> scheme://user:***@rest
  const std::string::size_type scheme_end = uri.find("://");
  if (scheme_end == std::string::npos) return uri;
  const std::string::size_type at = uri.find('@', scheme_end + 3);
  if (at == std::string::npos) return uri;
  const std::string::size_type colon = uri.find(':', scheme_end + 3);
  if (colon == std::string::npos || colon > at) return uri;
  return uri.substr(0, colon + 1) + "***" + uri.substr(at);
}

gnc_numeric NumericFromProto(const shim::Numeric& numeric,
                             const std::string& context) {
  if (numeric.denom() <= 0) {
    throw ShimError(shim::ERROR_CODE_INVALID_ARGUMENT,
                    "Numeric.denom must be > 0", context);
  }
  return gnc_numeric_create(numeric.num(), numeric.denom());
}

void NumericToProto(gnc_numeric value, shim::Numeric* out) {
  out->set_num(value.num);
  out->set_denom(value.denom);
}

GDate DateFromProto(const shim::Date& date, const std::string& context) {
  if (!g_date_valid_dmy(static_cast<GDateDay>(date.day()),
                        static_cast<GDateMonth>(date.month()),
                        static_cast<GDateYear>(date.year()))) {
    throw ShimError(shim::ERROR_CODE_INVALID_ARGUMENT,
                    "invalid calendar date " + std::to_string(date.year()) +
                        "-" + std::to_string(date.month()) + "-" +
                        std::to_string(date.day()),
                    context);
  }
  GDate out;
  g_date_clear(&out, 1);
  g_date_set_dmy(&out, static_cast<GDateDay>(date.day()),
                 static_cast<GDateMonth>(date.month()),
                 static_cast<GDateYear>(date.year()));
  return out;
}

void DateToProto(const GDate& date, shim::Date* out) {
  out->set_year(g_date_get_year(&date));
  out->set_month(g_date_get_month(&date));
  out->set_day(g_date_get_day(&date));
}

gnc_commodity* FindCommodity(QofBook* book, const shim::CommodityId& id,
                             const std::string& context) {
  gnc_commodity_table* table = gnc_commodity_table_get_table(book);
  gnc_commodity* commodity = gnc_commodity_table_lookup(
      table, id.space().c_str(), id.mnemonic().c_str());
  if (commodity == nullptr) {
    throw ShimError(shim::ERROR_CODE_COMMODITY_NOT_FOUND,
                    "no commodity {" + id.space() + ", " + id.mnemonic() + "}",
                    context);
  }
  return commodity;
}

void CommodityIdToProto(const gnc_commodity* commodity,
                        shim::CommodityId* out) {
  out->set_space(gnc_commodity_get_namespace(commodity));
  out->set_mnemonic(gnc_commodity_get_mnemonic(commodity));
}

::Account* FindAccount(QofBook* book, const std::string& guid,
                       const std::string& context) {
  const GncGUID parsed = StringToGuid(guid, context);
  ::Account* account = xaccAccountLookup(&parsed, book);
  if (account == nullptr) {
    throw ShimError(shim::ERROR_CODE_ACCOUNT_NOT_FOUND, "no account " + guid, context);
  }
  return account;
}

GNCAccountType AccountTypeFromProto(shim::AccountType type,
                                    const std::string& context) {
  switch (type) {
    case shim::ACCOUNT_TYPE_ASSET:      return ACCT_TYPE_ASSET;
    case shim::ACCOUNT_TYPE_BANK:       return ACCT_TYPE_BANK;
    case shim::ACCOUNT_TYPE_CASH:       return ACCT_TYPE_CASH;
    case shim::ACCOUNT_TYPE_CREDIT:     return ACCT_TYPE_CREDIT;
    case shim::ACCOUNT_TYPE_LIABILITY:  return ACCT_TYPE_LIABILITY;
    case shim::ACCOUNT_TYPE_EQUITY:     return ACCT_TYPE_EQUITY;
    case shim::ACCOUNT_TYPE_INCOME:     return ACCT_TYPE_INCOME;
    case shim::ACCOUNT_TYPE_EXPENSE:    return ACCT_TYPE_EXPENSE;
    case shim::ACCOUNT_TYPE_STOCK:      return ACCT_TYPE_STOCK;
    case shim::ACCOUNT_TYPE_MUTUAL:     return ACCT_TYPE_MUTUAL;
    case shim::ACCOUNT_TYPE_RECEIVABLE: return ACCT_TYPE_RECEIVABLE;
    case shim::ACCOUNT_TYPE_PAYABLE:    return ACCT_TYPE_PAYABLE;
    case shim::ACCOUNT_TYPE_TRADING:    return ACCT_TYPE_TRADING;
    case shim::ACCOUNT_TYPE_ROOT:
      // The root account exists exactly once and is never created or
      // retyped through the contract.
      throw ShimError(shim::ERROR_CODE_INVALID_ARGUMENT,
                      "ROOT is not a settable account type", context);
    default:
      throw ShimError(shim::ERROR_CODE_INVALID_ARGUMENT,
                      "unspecified account type", context);
  }
}

shim::AccountType AccountTypeToProto(GNCAccountType type) {
  switch (type) {
    case ACCT_TYPE_ASSET:      return shim::ACCOUNT_TYPE_ASSET;
    case ACCT_TYPE_BANK:       return shim::ACCOUNT_TYPE_BANK;
    case ACCT_TYPE_CASH:       return shim::ACCOUNT_TYPE_CASH;
    case ACCT_TYPE_CREDIT:     return shim::ACCOUNT_TYPE_CREDIT;
    case ACCT_TYPE_LIABILITY:  return shim::ACCOUNT_TYPE_LIABILITY;
    case ACCT_TYPE_EQUITY:     return shim::ACCOUNT_TYPE_EQUITY;
    case ACCT_TYPE_INCOME:     return shim::ACCOUNT_TYPE_INCOME;
    case ACCT_TYPE_EXPENSE:    return shim::ACCOUNT_TYPE_EXPENSE;
    case ACCT_TYPE_STOCK:      return shim::ACCOUNT_TYPE_STOCK;
    case ACCT_TYPE_MUTUAL:     return shim::ACCOUNT_TYPE_MUTUAL;
    case ACCT_TYPE_RECEIVABLE: return shim::ACCOUNT_TYPE_RECEIVABLE;
    case ACCT_TYPE_PAYABLE:    return shim::ACCOUNT_TYPE_PAYABLE;
    case ACCT_TYPE_TRADING:    return shim::ACCOUNT_TYPE_TRADING;
    case ACCT_TYPE_ROOT:       return shim::ACCOUNT_TYPE_ROOT;
    default:
      // Obsolete engine types (CURRENCY, CHECKING, ...) that the contract
      // deliberately does not model.
      return shim::ACCOUNT_TYPE_UNSPECIFIED;
  }
}

void ReconcileInfoToProto(::Account* account, shim::ReconcileInfo* out) {
  out->set_account_guid(GuidToString(qof_instance_get_guid(account)));
  time64 last_date = 0;
  if (xaccAccountGetReconcileLastDate(account, &last_date)) {
    const GDate date = time64_to_gdate(last_date);
    DateToProto(date, out->mutable_last_date());
  }
  NumericToProto(xaccAccountGetReconciledBalance(account),
                 out->mutable_reconciled_balance());
}

::Transaction* FindTransaction(QofBook* book, const std::string& guid,
                               const std::string& context) {
  const GncGUID parsed = StringToGuid(guid, context);
  ::Transaction* transaction = xaccTransLookup(&parsed, book);
  if (transaction == nullptr) {
    throw ShimError(shim::ERROR_CODE_TXN_NOT_FOUND, "no transaction " + guid, context);
  }
  return transaction;
}

char ReconcileStateFromProto(shim::ReconcileState state,
                             const std::string& context) {
  switch (state) {
    case shim::RECONCILE_STATE_NOT_RECONCILED: return NREC;
    case shim::RECONCILE_STATE_CLEARED:        return CREC;
    case shim::RECONCILE_STATE_RECONCILED:     return YREC;
    case shim::RECONCILE_STATE_FROZEN:         return FREC;
    case shim::RECONCILE_STATE_VOIDED:         return VREC;
    default:
      throw ShimError(shim::ERROR_CODE_INVALID_ARGUMENT,
                      "unspecified reconcile state", context);
  }
}

shim::ReconcileState ReconcileStateToProto(char flag) {
  switch (flag) {
    case NREC: return shim::RECONCILE_STATE_NOT_RECONCILED;
    case CREC: return shim::RECONCILE_STATE_CLEARED;
    case YREC: return shim::RECONCILE_STATE_RECONCILED;
    case FREC: return shim::RECONCILE_STATE_FROZEN;
    case VREC: return shim::RECONCILE_STATE_VOIDED;
    default:   return shim::RECONCILE_STATE_UNSPECIFIED;
  }
}

void SplitToProto(::Split* split, shim::Split* out) {
  const auto safe = [](const char* s) { return s != nullptr ? s : ""; };
  out->set_guid(GuidToString(qof_instance_get_guid(split)));
  out->set_account_guid(
      GuidToString(qof_instance_get_guid(xaccSplitGetAccount(split))));
  NumericToProto(xaccSplitGetValue(split), out->mutable_value());
  NumericToProto(xaccSplitGetAmount(split), out->mutable_quantity());
  out->set_memo(safe(xaccSplitGetMemo(split)));
  out->set_action(safe(xaccSplitGetAction(split)));
  out->set_reconcile_state(ReconcileStateToProto(xaccSplitGetReconcile(split)));
}

void TransactionToProto(::Transaction* transaction, shim::Transaction* out) {
  const auto safe = [](const char* s) { return s != nullptr ? s : ""; };
  out->set_guid(GuidToString(qof_instance_get_guid(transaction)));
  const gnc_commodity* currency = xaccTransGetCurrency(transaction);
  if (currency != nullptr) {
    CommodityIdToProto(currency, out->mutable_currency());
  }
  const GDate post_date = xaccTransGetDatePostedGDate(transaction);
  if (g_date_valid(&post_date)) {
    DateToProto(post_date, out->mutable_post_date());
  }
  out->set_enter_time_utc(xaccTransGetDateEntered(transaction));
  out->set_num(safe(xaccTransGetNum(transaction)));
  out->set_description(safe(xaccTransGetDescription(transaction)));
  for (GList* node = xaccTransGetSplitList(transaction); node != nullptr;
       node = node->next) {
    auto* split = static_cast<::Split*>(node->data);
    // A split queued for destruction (xaccSplitDestroy) is only actually
    // unlinked from this list by the transaction's outermost CommitEdit
    // (see trans_cleanup_commit in the engine); within a still-open edit —
    // exactly where the mutation protocol serializes the response before
    // committing — it lingers here. Skip it so the pre-commit response
    // matches the post-commit book exactly, which crash reconciliation
    // depends on.
    if (qof_instance_get_destroying(QOF_INSTANCE(split))) continue;
    SplitToProto(split, out->add_splits());
  }
}

void AccountToProto(::Account* account, shim::Account* out) {
  // Engine string getters may return null for never-set fields.
  const auto safe = [](const char* s) { return s != nullptr ? s : ""; };
  out->set_guid(GuidToString(qof_instance_get_guid(account)));
  const ::Account* parent = gnc_account_get_parent(account);
  if (parent != nullptr) {
    out->set_parent_guid(GuidToString(qof_instance_get_guid(parent)));
  }
  out->set_name(safe(xaccAccountGetName(account)));
  out->set_type(AccountTypeToProto(xaccAccountGetType(account)));
  const gnc_commodity* commodity = xaccAccountGetCommodity(account);
  if (commodity != nullptr) {
    CommodityIdToProto(commodity, out->mutable_commodity());
  }
  out->set_code(safe(xaccAccountGetCode(account)));
  out->set_description(safe(xaccAccountGetDescription(account)));
  out->set_placeholder(xaccAccountGetPlaceholder(account));
  out->set_hidden(xaccAccountGetHidden(account));
}

}  // namespace daichod
