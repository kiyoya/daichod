#include "rpc/transaction_service.h"

#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Account.h>
#include <Query.h>
#include <Split.h>
#include <Transaction.h>
#include <glib.h>
#include <gnc-date.h>
#include <qofquery.h>

#include "engine/map.h"
#include "rpc/mutation.h"
#include "rpc/rpc_util.h"

namespace daichod {

namespace shim = daicho::shim::v1;

namespace {

// Everything a Post/Update needs, resolved and validated BEFORE any engine
// object is touched. Once a plan exists, applying it cannot fail, so an
// engine edit never has to roll back halfway — partial application is
// impossible by construction, not by cleanup.
struct SplitPlan {
  ::Split* existing = nullptr;  // update-in-place when set
  ::Account* account = nullptr;
  gnc_numeric value;
  gnc_numeric quantity;
  std::string memo;
  std::string action;
  char reconcile = NREC;
};

struct TransactionPlan {
  gnc_commodity* currency = nullptr;
  GDate post_date;
  time64 enter_time = 0;  // 0 = engine's now
  std::string num;
  std::string description;
  std::vector<SplitPlan> splits;
  std::vector<::Split*> doomed;  // update: engine splits absent from the spec
};

TransactionPlan PlanTransaction(QofBook* book, const shim::Transaction& spec,
                                ::Transaction* existing) {
  TransactionPlan plan;
  if (!spec.has_currency()) {
    throw ShimError(shim::INVALID_ARGUMENT_DETAIL, "currency is required",
                    "transaction.currency");
  }
  plan.currency =
      FindCommodity(book, spec.currency(), "transaction.currency");
  if (!gnc_commodity_is_currency(plan.currency)) {
    throw ShimError(shim::CURRENCY_MISMATCH,
                    "transaction currency is not a currency commodity",
                    "transaction.currency");
  }
  if (!spec.has_post_date()) {
    throw ShimError(shim::INVALID_ARGUMENT_DETAIL, "post_date is required",
                    "transaction.post_date");
  }
  plan.post_date = DateFromProto(spec.post_date(), "transaction.post_date");
  // 0 = now, resolved here rather than left to the engine's commit hook so
  // the pending response (serialized pre-commit) matches the committed
  // state byte-for-byte — crash reconciliation depends on that.
  plan.enter_time =
      spec.enter_time_utc() != 0 ? spec.enter_time_utc() : gnc_time(nullptr);
  plan.num = spec.num();
  plan.description = spec.description();

  if (spec.splits().empty()) {
    throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                    "at least one split is required", "transaction.splits");
  }

  // Update: match spec splits to engine splits by GUID; anything the spec
  // omits is destroyed (full replacement).
  std::unordered_map<std::string, ::Split*> engine_splits;
  if (existing != nullptr) {
    for (GList* node = xaccTransGetSplitList(existing); node != nullptr;
         node = node->next) {
      auto* split = static_cast<::Split*>(node->data);
      engine_splits[GuidToString(qof_instance_get_guid(split))] = split;
    }
  }
  std::unordered_set<std::string> claimed;

  // The transaction must balance in its currency. The engine "enforces"
  // imbalance by materializing an Imbalance split at scrub time, which is
  // exactly the silent surprise the contract forbids — so the sum check
  // runs here, with engine arithmetic, and rejects instead.
  gnc_numeric sum = gnc_numeric_zero();
  for (int i = 0; i < spec.splits_size(); ++i) {
    const shim::Split& split_spec = spec.splits(i);
    const std::string field = "transaction.splits[" + std::to_string(i) + "]";
    SplitPlan split_plan;
    if (!split_spec.guid().empty()) {
      if (existing == nullptr) {
        throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                        "split guid must be empty on create; the engine "
                        "assigns",
                        field + ".guid");
      }
      const auto found = engine_splits.find(split_spec.guid());
      if (found == engine_splits.end()) {
        throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                        "split " + split_spec.guid() +
                            " does not belong to this transaction",
                        field + ".guid");
      }
      if (!claimed.insert(split_spec.guid()).second) {
        throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                        "split " + split_spec.guid() + " appears twice",
                        field + ".guid");
      }
      split_plan.existing = found->second;
    }
    split_plan.account =
        FindAccount(book, split_spec.account_guid(), field + ".account_guid");
    split_plan.value = NumericFromProto(split_spec.value(), field + ".value");
    split_plan.quantity =
        NumericFromProto(split_spec.quantity(), field + ".quantity");
    split_plan.memo = split_spec.memo();
    split_plan.action = split_spec.action();
    // Full replacement: UNSPECIFIED is the proto3 default, mapped to 'n'.
    split_plan.reconcile =
        split_spec.reconcile_state() == shim::RECONCILE_STATE_UNSPECIFIED
            ? NREC
            : ReconcileStateFromProto(split_spec.reconcile_state(),
                                      field + ".reconcile_state");
    sum = gnc_numeric_add(sum, split_plan.value, GNC_DENOM_AUTO,
                          GNC_HOW_DENOM_LCD);
    if (gnc_numeric_check(sum) != GNC_ERROR_OK) {
      throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                      "split values overflow engine arithmetic", field);
    }
    plan.splits.push_back(std::move(split_plan));
  }
  if (!gnc_numeric_zero_p(sum)) {
    // gnc_numeric_to_string g_strdup's its result; the caller owns it.
    gchar* sum_str = gnc_numeric_to_string(sum);
    const std::string message =
        "split values sum to " + std::string(sum_str) + ", expected zero";
    g_free(sum_str);
    throw ShimError(shim::UNBALANCED_TRANSACTION, message,
                    "transaction.splits");
  }

  for (const auto& [guid, split] : engine_splits) {
    if (claimed.find(guid) == claimed.end()) plan.doomed.push_back(split);
  }
  return plan;
}

// Applies a validated plan inside the caller's BeginEdit. Cannot fail.
void ApplyPlan(QofBook* book, ::Transaction* transaction,
               const TransactionPlan& plan) {
  xaccTransSetCurrency(transaction, plan.currency);
  xaccTransSetDatePostedGDate(transaction, plan.post_date);
  xaccTransSetDateEnteredSecs(transaction, plan.enter_time);
  xaccTransSetNum(transaction, plan.num.c_str());
  xaccTransSetDescription(transaction, plan.description.c_str());
  for (::Split* split : plan.doomed) xaccSplitDestroy(split);
  for (const SplitPlan& split_plan : plan.splits) {
    ::Split* split = split_plan.existing;
    if (split == nullptr) {
      split = xaccMallocSplit(book);
      xaccSplitSetParent(split, transaction);
    }
    xaccSplitSetAccount(split, split_plan.account);
    xaccSplitSetValue(split, split_plan.value);
    xaccSplitSetAmount(split, split_plan.quantity);
    xaccSplitSetMemo(split, split_plan.memo.c_str());
    xaccSplitSetAction(split, split_plan.action.c_str());
    xaccSplitSetReconcile(split, split_plan.reconcile);
  }
}

cleared_match_t ClearedMaskFromStates(
    const google::protobuf::RepeatedField<int>& states) {
  int mask = 0;
  for (const int state : states) {
    switch (static_cast<shim::ReconcileState>(state)) {
      case shim::NOT_RECONCILED: mask |= CLEARED_NO; break;
      case shim::CLEARED:        mask |= CLEARED_CLEARED; break;
      case shim::RECONCILED:     mask |= CLEARED_RECONCILED; break;
      case shim::FROZEN:         mask |= CLEARED_FROZEN; break;
      case shim::VOIDED:         mask |= CLEARED_VOIDED; break;
      default:
        throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                        "unspecified reconcile state", "states");
    }
  }
  return static_cast<cleared_match_t>(mask);
}

constexpr int kDefaultPageSize = 100;
constexpr int kMaxPageSize = 500;

}  // namespace

grpc::Status TransactionServiceImpl::GetTransaction(
    grpc::ServerContext*, const shim::TxnRef* request,
    shim::Transaction* response) {
  return RunRpc(worker_, [this, request, response] {
    TransactionToProto(
        FindTransaction(session_->book(), request->guid(), "guid"), response);
  });
}

grpc::Status TransactionServiceImpl::PostTransaction(
    grpc::ServerContext*, const shim::PostTransactionRequest* request,
    shim::Transaction* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.TransactionService/PostTransaction", response,
      [this, request](shim::Transaction* out,
                      const PendingRecorder& record_pending) {
        const shim::Transaction& spec = request->transaction();
        if (!spec.guid().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "guid must be empty on create; the engine assigns",
                          "transaction.guid");
        }
        QofBook* book = session_->book();
        const TransactionPlan plan = PlanTransaction(book, spec, nullptr);
        ::Transaction* transaction = xaccMallocTransaction(book);
        xaccTransBeginEdit(transaction);
        ApplyPlan(book, transaction, plan);
        TransactionToProto(transaction, out);
        record_pending();
        xaccTransCommitEdit(transaction);
      });
}

grpc::Status TransactionServiceImpl::UpdateTransaction(
    grpc::ServerContext*, const shim::UpdateTransactionRequest* request,
    shim::Transaction* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.TransactionService/UpdateTransaction", response,
      [this, request](shim::Transaction* out,
                      const PendingRecorder& record_pending) {
        const shim::Transaction& spec = request->transaction();
        if (spec.guid().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "guid is required on update", "transaction.guid");
        }
        QofBook* book = session_->book();
        ::Transaction* transaction =
            FindTransaction(book, spec.guid(), "transaction.guid");
        const TransactionPlan plan = PlanTransaction(book, spec, transaction);
        xaccTransBeginEdit(transaction);
        ApplyPlan(book, transaction, plan);
        TransactionToProto(transaction, out);
        record_pending();
        xaccTransCommitEdit(transaction);
      });
}

grpc::Status TransactionServiceImpl::DeleteTransaction(
    grpc::ServerContext*, const shim::DeleteTransactionRequest* request,
    shim::Empty* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.TransactionService/DeleteTransaction", response,
      [this, request](shim::Empty*, const PendingRecorder& record_pending) {
        ::Transaction* transaction =
            FindTransaction(session_->book(), request->guid(), "guid");
        record_pending();
        xaccTransBeginEdit(transaction);
        xaccTransDestroy(transaction);
        xaccTransCommitEdit(transaction);
      });
}

grpc::Status TransactionServiceImpl::QuerySplits(
    grpc::ServerContext*, const shim::QuerySplitsRequest* request,
    shim::QuerySplitsResponse* response) {
  return RunRpc(worker_, [this, request, response] {
    QofBook* book = session_->book();

    // Validate and resolve every input before the query object exists, so
    // no throw can leak it.
    GList* accounts = nullptr;
    for (const std::string& guid : request->account_guids()) {
      accounts =
          g_list_prepend(accounts, FindAccount(book, guid, "account_guids"));
    }
    time64 start = 0;
    time64 end = 0;
    if (request->has_date_from()) {
      const GDate d = DateFromProto(request->date_from(), "date_from");
      start = gnc_dmy2time64(g_date_get_day(&d), g_date_get_month(&d),
                             g_date_get_year(&d));
    }
    if (request->has_date_to()) {
      const GDate d = DateFromProto(request->date_to(), "date_to");
      end = gnc_dmy2time64_end(g_date_get_day(&d), g_date_get_month(&d),
                               g_date_get_year(&d));
    }
    const cleared_match_t cleared_mask =
        ClearedMaskFromStates(request->states());

    // Value bounds are filtered post-run: the engine's numeric predicates
    // compare against sign-classified magnitudes, not plain ranges, and
    // reimplementing the contract's inclusive range on top of them would be
    // interpretation. gnc_numeric_compare is still engine arithmetic.
    const bool has_min = request->has_value_min();
    const bool has_max = request->has_value_max();
    gnc_numeric value_min = gnc_numeric_zero();
    gnc_numeric value_max = gnc_numeric_zero();
    if (has_min) {
      value_min = NumericFromProto(request->value_min(), "value_min");
    }
    if (has_max) {
      value_max = NumericFromProto(request->value_max(), "value_max");
    }

    int page_size = request->page_size();
    if (page_size <= 0) page_size = kDefaultPageSize;
    if (page_size > kMaxPageSize) page_size = kMaxPageSize;

    QofQuery* query = qof_query_create_for(GNC_ID_SPLIT);
    qof_query_set_book(query, book);
    if (accounts != nullptr) {
      xaccQueryAddAccountMatch(query, accounts, QOF_GUID_MATCH_ANY,
                               QOF_QUERY_AND);
      g_list_free(accounts);
    }
    if (request->has_date_from() || request->has_date_to()) {
      xaccQueryAddDateMatchTT(query, request->has_date_from(), start,
                              request->has_date_to(), end, QOF_QUERY_AND);
    }
    if (!request->description_contains().empty()) {
      xaccQueryAddDescriptionMatch(query,
                                   request->description_contains().c_str(),
                                   FALSE, FALSE, QOF_COMPARE_CONTAINS,
                                   QOF_QUERY_AND);
    }
    if (!request->memo_contains().empty()) {
      xaccQueryAddMemoMatch(query, request->memo_contains().c_str(), FALSE,
                            FALSE, QOF_COMPARE_CONTAINS, QOF_QUERY_AND);
    }
    if (!request->states().empty()) {
      xaccQueryAddClearedMatch(query, cleared_mask, QOF_QUERY_AND);
    }

    GList* results = qof_query_run(query);

    // Cursor: the GUID of the last split of the previous page. The engine
    // thread serializes queries with mutations, so within one call the list
    // is consistent; a token whose split no longer matches simply resumes
    // from nowhere and reports the mismatch.
    bool skipping = !request->page_token().empty();
    bool token_seen = false;
    int emitted = 0;
    for (GList* node = results; node != nullptr; node = node->next) {
      auto* split = static_cast<::Split*>(node->data);
      const std::string guid = GuidToString(qof_instance_get_guid(split));
      if (skipping) {
        if (guid == request->page_token()) {
          skipping = false;
          token_seen = true;
        }
        continue;
      }
      if (has_min &&
          gnc_numeric_compare(xaccSplitGetValue(split), value_min) < 0) {
        continue;
      }
      if (has_max &&
          gnc_numeric_compare(xaccSplitGetValue(split), value_max) > 0) {
        continue;
      }
      if (emitted == page_size) {
        // One past the page: the previous split starts the next page.
        response->set_next_page_token(
            response->splits(emitted - 1).split().guid());
        break;
      }
      shim::SplitWithContext* out = response->add_splits();
      SplitToProto(split, out->mutable_split());
      ::Transaction* transaction = xaccSplitGetParent(split);
      out->set_transaction_guid(
          GuidToString(qof_instance_get_guid(transaction)));
      const GDate post_date = xaccTransGetDatePostedGDate(transaction);
      if (g_date_valid(&post_date)) {
        DateToProto(post_date, out->mutable_post_date());
      }
      const char* description = xaccTransGetDescription(transaction);
      out->set_description(description != nullptr ? description : "");
      const gnc_commodity* currency = xaccTransGetCurrency(transaction);
      if (currency != nullptr) {
        CommodityIdToProto(currency, out->mutable_txn_currency());
      }
      ++emitted;
    }
    qof_query_destroy(query);

    if (skipping && !token_seen) {
      throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                      "page_token does not match any current result",
                      "page_token");
    }
  });
}

}  // namespace daichod
