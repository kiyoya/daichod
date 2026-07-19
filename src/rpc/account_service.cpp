#include "rpc/account_service.h"

#include <Account.h>
#include <gnc-date.h>
#include <glib.h>

#include "engine/map.h"
#include "rpc/mutation.h"
#include "rpc/rpc_util.h"

namespace daichod {

namespace shim = daicho::shim::v1;

namespace {

// Reparent guard: parent must not be the account itself or one of its
// descendants. Mechanical tree integrity, not policy — the engine would
// corrupt the tree silently otherwise.
void CheckNoCycle(::Account* account, ::Account* new_parent,
                  const std::string& context) {
  for (::Account* cursor = new_parent; cursor != nullptr;
       cursor = gnc_account_get_parent(cursor)) {
    if (cursor == account) {
      throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                      "parent_guid would create a cycle", context);
    }
  }
}

void CheckIsNotRoot(::Account* account, ::Account* root,
                    const std::string& context) {
  if (account == root) {
    throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                    "the root account cannot be modified", context);
  }
}

}  // namespace

grpc::Status AccountServiceImpl::ListAccounts(grpc::ServerContext*,
                                              const shim::Empty*,
                                              shim::AccountList* response) {
  return RunRpc(worker_, [this, response] {
    ::Account* root = session_->root_account();
    AccountToProto(root, response->add_accounts());
    GList* descendants = gnc_account_get_descendants_sorted(root);
    for (GList* node = descendants; node != nullptr; node = node->next) {
      AccountToProto(static_cast<::Account*>(node->data),
                     response->add_accounts());
    }
    g_list_free(descendants);
  });
}

grpc::Status AccountServiceImpl::GetAccount(grpc::ServerContext*,
                                            const shim::AccountRef* request,
                                            shim::Account* response) {
  return RunRpc(worker_, [this, request, response] {
    AccountToProto(FindAccount(session_->book(), request->guid(), "guid"),
                   response);
  });
}

grpc::Status AccountServiceImpl::CreateAccount(
    grpc::ServerContext*, const shim::CreateAccountRequest* request,
    shim::Account* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.AccountService/CreateAccount", response,
      [this, request](shim::Account* out,
                      const PendingRecorder& record_pending) {
        const shim::Account& spec = request->account();
        if (!spec.guid().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "guid must be empty on create; the engine assigns",
                          "account.guid");
        }
        if (spec.name().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL, "name is required",
                          "account.name");
        }
        if (spec.parent_guid().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "parent_guid is required", "account.parent_guid");
        }
        QofBook* book = session_->book();
        // Validate everything before the edit begins so the edit cannot fail.
        ::Account* parent =
            FindAccount(book, spec.parent_guid(), "account.parent_guid");
        const GNCAccountType type =
            AccountTypeFromProto(spec.type(), "account.type");
        gnc_commodity* commodity =
            FindCommodity(book, spec.commodity(), "account.commodity");

        ::Account* account = xaccMallocAccount(book);
        xaccAccountBeginEdit(account);
        xaccAccountSetName(account, spec.name().c_str());
        xaccAccountSetType(account, type);
        xaccAccountSetCommodity(account, commodity);
        xaccAccountSetCode(account, spec.code().c_str());
        xaccAccountSetDescription(account, spec.description().c_str());
        xaccAccountSetPlaceholder(account, spec.placeholder());
        xaccAccountSetHidden(account, spec.hidden());
        gnc_account_append_child(parent, account);
        AccountToProto(account, out);
        record_pending();
        xaccAccountCommitEdit(account);
      });
}

grpc::Status AccountServiceImpl::UpdateAccount(
    grpc::ServerContext*, const shim::UpdateAccountRequest* request,
    shim::Account* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.AccountService/UpdateAccount", response,
      [this, request](shim::Account* out,
                      const PendingRecorder& record_pending) {
        const shim::Account& spec = request->account();
        if (spec.guid().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "guid is required on update", "account.guid");
        }
        if (spec.name().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL, "name is required",
                          "account.name");
        }
        QofBook* book = session_->book();
        ::Account* account = FindAccount(book, spec.guid(), "account.guid");
        CheckIsNotRoot(account, session_->root_account(), "account.guid");

        const GNCAccountType new_type =
            AccountTypeFromProto(spec.type(), "account.type");
        gnc_commodity* commodity =
            FindCommodity(book, spec.commodity(), "account.commodity");
        if (spec.parent_guid().empty()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "parent_guid is required (full replacement)",
                          "account.parent_guid");
        }
        ::Account* new_parent =
            FindAccount(book, spec.parent_guid(), "account.parent_guid");
        CheckNoCycle(account, new_parent, "account.parent_guid");

        // Full replacement inside one engine edit.
        xaccAccountBeginEdit(account);
        xaccAccountSetName(account, spec.name().c_str());
        xaccAccountSetType(account, new_type);
        xaccAccountSetCommodity(account, commodity);
        xaccAccountSetCode(account, spec.code().c_str());
        xaccAccountSetDescription(account, spec.description().c_str());
        xaccAccountSetPlaceholder(account, spec.placeholder());
        xaccAccountSetHidden(account, spec.hidden());
        if (new_parent != gnc_account_get_parent(account)) {
          gnc_account_append_child(new_parent, account);
        }
        AccountToProto(account, out);
        record_pending();
        xaccAccountCommitEdit(account);
      });
}

grpc::Status AccountServiceImpl::DeleteAccount(
    grpc::ServerContext*, const shim::DeleteAccountRequest* request,
    shim::Empty* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.AccountService/DeleteAccount", response,
      [this, request](shim::Empty*, const PendingRecorder& record_pending) {
        ::Account* account =
            FindAccount(session_->book(), request->guid(), "guid");
        CheckIsNotRoot(account, session_->root_account(), "guid");
        if (xaccAccountGetSplitList(account) != nullptr ||
            gnc_account_n_children(account) > 0) {
          throw ShimError(shim::ACCOUNT_NOT_EMPTY,
                          "account has splits or children", request->guid());
        }
        record_pending();
        xaccAccountBeginEdit(account);
        xaccAccountDestroy(account);  // commits the edit itself
      });
}

grpc::Status AccountServiceImpl::GetReconcileInfo(
    grpc::ServerContext*, const shim::AccountRef* request,
    shim::ReconcileInfo* response) {
  return RunRpc(worker_, [this, request, response] {
    ReconcileInfoToProto(FindAccount(session_->book(), request->guid(), "guid"),
                      response);
  });
}

grpc::Status AccountServiceImpl::SetReconcileInfo(
    grpc::ServerContext*, const shim::SetReconcileInfoRequest* request,
    shim::ReconcileInfo* response) {
  return RunMutation(
      worker_, journal_, request->meta(), *request,
      "daicho.shim.v1.AccountService/SetReconcileInfo", response,
      [this, request](shim::ReconcileInfo* out,
                      const PendingRecorder& record_pending) {
        ::Account* account = FindAccount(session_->book(),
                                         request->account_guid(),
                                         "account_guid");
        if (!request->has_last_date()) {
          throw ShimError(shim::INVALID_ARGUMENT_DETAIL,
                          "last_date is required", "last_date");
        }
        GDate date = DateFromProto(request->last_date(), "last_date");
        // The engine's own date-only convention: a neutral time of day that
        // maps back to the same calendar date in any timezone.
        const time64 stamp = gnc_dmy2time64_neutral(
            g_date_get_day(&date), g_date_get_month(&date),
            g_date_get_year(&date));
        xaccAccountBeginEdit(account);
        xaccAccountSetReconcileLastDate(account, stamp);
        ReconcileInfoToProto(account, out);
        record_pending();
        xaccAccountCommitEdit(account);
      });
}

}  // namespace daichod
