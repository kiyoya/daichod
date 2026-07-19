#include "journal/recovery.h"

#include <ctime>
#include <optional>
#include <string>

#include <Account.h>
#include <Transaction.h>
#include <gnc-pricedb.h>
#include <qof.h>

#include "engine/map.h"
#include "rpc/error.h"
#include "shim.pb.h"

namespace daichod {

namespace shim = daicho::shim::v1;

namespace {

bool GuidExists(QofBook* book, const std::string& guid,
                QofIdTypeConst type) {
  GncGUID parsed;
  if (!string_to_guid(guid.c_str(), &parsed)) return false;
  if (type == GNC_ID_ACCOUNT) {
    return xaccAccountLookup(&parsed, book) != nullptr;
  }
  if (type == GNC_ID_TRANS) {
    return xaccTransLookup(&parsed, book) != nullptr;
  }
  return false;
}

// Decides whether the pending mutation's engine commit reached the book.
// nullopt means the record is malformed — treated as not applied, which is
// the safe direction (it stays indeterminate and is never silently
// resolved as done).
std::optional<bool> WasApplied(QofBook* book,
                               const Journal::PendingEntry& entry) {
  const std::string& rpc = entry.rpc_name;
  const auto ends_with = [&rpc](const char* suffix) {
    const std::string s(suffix);
    return rpc.size() >= s.size() &&
           rpc.compare(rpc.size() - s.size(), s.size(), s) == 0;
  };

  if (ends_with("/CreateAccount")) {
    shim::Account response;
    if (!response.ParseFromString(entry.pending_response)) return std::nullopt;
    return GuidExists(book, response.guid(), GNC_ID_ACCOUNT);
  }
  if (ends_with("/UpdateAccount")) {
    shim::Account response;
    if (!response.ParseFromString(entry.pending_response)) return std::nullopt;
    GncGUID parsed;
    if (!string_to_guid(response.guid().c_str(), &parsed)) {
      return std::nullopt;
    }
    ::Account* account = xaccAccountLookup(&parsed, book);
    if (account == nullptr) return std::nullopt;
    shim::Account current;
    AccountToProto(account, &current);
    return current.SerializeAsString() == entry.pending_response;
  }
  if (ends_with("/DeleteAccount")) {
    shim::DeleteAccountRequest request;
    if (!request.ParseFromString(entry.request_payload)) return std::nullopt;
    return !GuidExists(book, request.guid(), GNC_ID_ACCOUNT);
  }
  if (ends_with("/SetReconcileInfo")) {
    shim::ReconcileInfo response;
    if (!response.ParseFromString(entry.pending_response)) return std::nullopt;
    GncGUID parsed;
    if (!string_to_guid(response.account_guid().c_str(), &parsed)) {
      return std::nullopt;
    }
    ::Account* account = xaccAccountLookup(&parsed, book);
    if (account == nullptr) return std::nullopt;
    shim::ReconcileInfo current;
    ReconcileInfoToProto(account, &current);
    return current.SerializeAsString() == entry.pending_response;
  }
  if (ends_with("/PostTransaction")) {
    shim::Transaction response;
    if (!response.ParseFromString(entry.pending_response)) return std::nullopt;
    return GuidExists(book, response.guid(), GNC_ID_TRANS);
  }
  if (ends_with("/UpdateTransaction")) {
    shim::Transaction response;
    if (!response.ParseFromString(entry.pending_response)) return std::nullopt;
    GncGUID parsed;
    if (!string_to_guid(response.guid().c_str(), &parsed)) {
      return std::nullopt;
    }
    ::Transaction* transaction = xaccTransLookup(&parsed, book);
    if (transaction == nullptr) return std::nullopt;
    shim::Transaction current;
    TransactionToProto(transaction, &current);
    return current.SerializeAsString() == entry.pending_response;
  }
  if (ends_with("/DeleteTransaction")) {
    shim::DeleteTransactionRequest request;
    if (!request.ParseFromString(entry.request_payload)) return std::nullopt;
    return !GuidExists(book, request.guid(), GNC_ID_TRANS);
  }
  if (ends_with("/AddPrice")) {
    shim::Price response;
    if (!response.ParseFromString(entry.pending_response)) return std::nullopt;
    GncGUID parsed;
    if (!string_to_guid(response.guid().c_str(), &parsed)) {
      return std::nullopt;
    }
    GNCPrice* price = gnc_price_lookup(&parsed, book);
    if (price == nullptr) return false;
    gnc_price_unref(price);
    return true;
  }
  return std::nullopt;  // unknown RPC: conservative, stays indeterminate
}

}  // namespace

std::size_t ReconcilePendingMutations(Journal* journal, QofBook* book) {
  std::size_t applied_count = 0;
  for (const Journal::PendingEntry& entry : journal->ListPendingUnresolved()) {
    const std::optional<bool> applied = WasApplied(book, entry);
    if (applied.has_value() && *applied) {
      internal::Outcome outcome;
      outcome.set_rpc_name(entry.rpc_name);
      outcome.set_ok(true);
      outcome.set_response(entry.pending_response);
      journal->RecordOutcome(entry.mutation_id, outcome,
                             static_cast<int64_t>(std::time(nullptr)));
      ++applied_count;
    } else {
      journal->ClearPending(entry.mutation_id);
    }
  }
  return applied_count;
}

}  // namespace daichod
