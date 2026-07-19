#include <cstdio>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "daemon_fixture.h"

// Drives every crash point in the mutation protocol (see rpc/mutation.h and
// journal/recovery.h) against a real daemon process: kill it exactly there
// with DAICHOD_CRASH_AT, restart clean, and check that startup reconciliation
// converges on the one true outcome — and that re-issuing the same
// mutation_id afterwards is safe (never duplicates engine state).

namespace daichod::testing {
namespace {

namespace shim = daicho::shim::v1;

// Fixed, canonically-formatted mutation ids (8-4-4-4-12 hex; the journal
// only validates shape, not RFC 4122 version/variant bits). mutation_id is a
// single global key in the journal, so every id used within one test's
// daemon instance must be unique across all services.
std::string MutationId(int n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012x", n);
  return std::string(buf);
}

class CrashTest : public DaemonFixture {
 protected:
  void SetUp() override {
    DaemonFixture::SetUp();
    ConnectStubs();
    const std::string root_guid = RootGuid();
    asset_guid_ =
        CreateAccount(NextMutationId(), root_guid, "A", shim::ASSET).guid();
    expense_guid_ =
        CreateAccount(NextMutationId(), root_guid, "E", shim::EXPENSE).guid();
  }

  void ConnectStubs() {
    auto channel = Connect();
    account_stub_ = shim::AccountService::NewStub(channel);
    txn_stub_ = shim::TransactionService::NewStub(channel);
    session_stub_ = shim::SessionService::NewStub(channel);
    balance_stub_ = shim::BalanceService::NewStub(channel);
  }

  std::string NextMutationId() { return MutationId(next_mutation_id_++); }

  std::string RootGuid() {
    grpc::ClientContext context;
    shim::Empty request;
    shim::AccountList response;
    EXPECT_TRUE(
        account_stub_->ListAccounts(&context, request, &response).ok());
    for (const auto& account : response.accounts()) {
      if (account.type() == shim::ROOT) return account.guid();
    }
    ADD_FAILURE() << "no root account in ListAccounts response";
    return "";
  }

  shim::Account CreateAccount(const std::string& mutation_id,
                              const std::string& parent_guid,
                              const std::string& name,
                              shim::AccountType type) {
    shim::CreateAccountRequest request;
    request.mutable_meta()->set_mutation_id(mutation_id);
    shim::Account* spec = request.mutable_account();
    spec->set_parent_guid(parent_guid);
    spec->set_name(name);
    spec->set_type(type);
    spec->mutable_commodity()->set_space("CURRENCY");
    spec->mutable_commodity()->set_mnemonic("USD");
    grpc::ClientContext context;
    shim::Account response;
    EXPECT_TRUE(
        account_stub_->CreateAccount(&context, request, &response).ok());
    return response;
  }

  // A balanced two-split transaction on the fixture's A/E accounts, built
  // fresh each call so the same mutation_id can be re-sent byte-for-byte.
  shim::PostTransactionRequest BuildPostRequest(
      const std::string& mutation_id) {
    shim::PostTransactionRequest request;
    request.mutable_meta()->set_mutation_id(mutation_id);
    shim::Transaction* spec = request.mutable_transaction();
    spec->mutable_currency()->set_space("CURRENCY");
    spec->mutable_currency()->set_mnemonic("USD");
    shim::Date* date = spec->mutable_post_date();
    date->set_year(2026);
    date->set_month(7);
    date->set_day(1);
    spec->set_description("Crash test transaction");
    shim::Split* asset_split = spec->add_splits();
    asset_split->set_account_guid(asset_guid_);
    asset_split->mutable_value()->set_num(1000);
    asset_split->mutable_value()->set_denom(100);
    asset_split->mutable_quantity()->set_num(1000);
    asset_split->mutable_quantity()->set_denom(100);
    shim::Split* expense_split = spec->add_splits();
    expense_split->set_account_guid(expense_guid_);
    expense_split->mutable_value()->set_num(-1000);
    expense_split->mutable_value()->set_denom(100);
    expense_split->mutable_quantity()->set_num(-1000);
    expense_split->mutable_quantity()->set_denom(100);
    return request;
  }

  // Posts a transaction through the currently-connected (non-crashing)
  // daemon; asserts success.
  shim::Transaction PostTxnNormally(const std::string& mutation_id) {
    const shim::PostTransactionRequest request = BuildPostRequest(mutation_id);
    grpc::ClientContext context;
    shim::Transaction response;
    EXPECT_TRUE(txn_stub_->PostTransaction(&context, request, &response).ok())
        << "expected normal PostTransaction to succeed";
    return response;
  }

  int QuerySplitCount() {
    grpc::ClientContext context;
    shim::QuerySplitsRequest request;
    shim::QuerySplitsResponse response;
    EXPECT_TRUE(txn_stub_->QuerySplits(&context, request, &response).ok());
    return response.splits_size();
  }

  // The guid of the transaction owning the book's only splits; only valid
  // when exactly one transaction exists.
  std::string SoleTransactionGuid() {
    grpc::ClientContext context;
    shim::QuerySplitsRequest request;
    shim::QuerySplitsResponse response;
    EXPECT_TRUE(txn_stub_->QuerySplits(&context, request, &response).ok());
    if (response.splits_size() == 0) {
      ADD_FAILURE() << "no splits to read a transaction guid from";
      return "";
    }
    return response.splits(0).transaction_guid();
  }

  bool IsIndeterminate(const std::string& mutation_id) {
    grpc::ClientContext context;
    shim::Empty request;
    shim::IndeterminateMutations response;
    EXPECT_TRUE(
        session_stub_->ListIndeterminateMutations(&context, request, &response)
            .ok());
    for (const auto& entry : response.entries()) {
      if (entry.mutation_id() == mutation_id) return true;
    }
    return false;
  }

  void ExpectTrialBalanceZero() {
    grpc::ClientContext context;
    shim::Empty request;
    shim::TrialBalance response;
    ASSERT_TRUE(
        balance_stub_->GetTrialBalance(&context, request, &response).ok());
    for (const auto& entry : response.entries()) {
      EXPECT_EQ(entry.sum().num(), 0)
          << "nonzero trial balance for " << entry.currency().mnemonic();
    }
  }

  // Restarts the daemon with DAICHOD_CRASH_AT=crash_point, then restarts it
  // clean once the crashed process has actually exited. Common preamble for
  // every per-crash-point test below.
  void RestartWithCrashPoint(const std::string& crash_point) {
    StopDaemon();
    StartDaemon(daichod_bin_, {"DAICHOD_CRASH_AT=" + crash_point});
    ConnectStubs();
  }
  void RestartClean() {
    WaitForExit();
    StartDaemon(daichod_bin_);
    ConnectStubs();
  }

  // Crash points before the engine commit lands (after_intent, after_pending
  // per rpc/mutation.h): the mutation is reported indeterminate and nothing
  // was created; re-issuing the identical request performs the real work
  // exactly once.
  void ExpectCrashNeverAppliedThenReissueApplies(
      const std::string& crash_point) {
    const std::string mutation_id = NextMutationId();
    const shim::PostTransactionRequest request = BuildPostRequest(mutation_id);

    RestartWithCrashPoint(crash_point);
    {
      grpc::ClientContext context;
      shim::Transaction response;
      const grpc::Status status =
          txn_stub_->PostTransaction(&context, request, &response);
      EXPECT_FALSE(status.ok())
          << "expected the daemon to crash mid-RPC at " << crash_point;
    }
    RestartClean();

    EXPECT_TRUE(IsIndeterminate(mutation_id));
    EXPECT_EQ(QuerySplitCount(), 0);

    grpc::ClientContext context;
    shim::Transaction response;
    ASSERT_TRUE(txn_stub_->PostTransaction(&context, request, &response).ok());
    EXPECT_EQ(QuerySplitCount(), 2);
    EXPECT_FALSE(IsIndeterminate(mutation_id));
    ExpectTrialBalanceZero();
  }

  // Crash points at or after the engine commit (after_apply, after_outcome):
  // reconciliation resolves the entry as applied (or it was already
  // resolved); re-issuing must return the already-committed transaction
  // rather than create a second one — the no-duplication property.
  void ExpectCrashAlreadyAppliedThenReissueIsNoDuplication(
      const std::string& crash_point) {
    const std::string mutation_id = NextMutationId();
    const shim::PostTransactionRequest request = BuildPostRequest(mutation_id);

    RestartWithCrashPoint(crash_point);
    {
      grpc::ClientContext context;
      shim::Transaction response;
      const grpc::Status status =
          txn_stub_->PostTransaction(&context, request, &response);
      EXPECT_FALSE(status.ok())
          << "expected the daemon to crash mid-RPC at " << crash_point;
    }
    RestartClean();

    EXPECT_FALSE(IsIndeterminate(mutation_id));
    ASSERT_EQ(QuerySplitCount(), 2);
    const std::string committed_guid = SoleTransactionGuid();

    grpc::ClientContext context;
    shim::Transaction response;
    ASSERT_TRUE(txn_stub_->PostTransaction(&context, request, &response).ok());
    EXPECT_EQ(response.guid(), committed_guid);
    EXPECT_EQ(QuerySplitCount(), 2);
  }

  std::unique_ptr<shim::AccountService::Stub> account_stub_;
  std::unique_ptr<shim::TransactionService::Stub> txn_stub_;
  std::unique_ptr<shim::SessionService::Stub> session_stub_;
  std::unique_ptr<shim::BalanceService::Stub> balance_stub_;
  std::string asset_guid_;
  std::string expense_guid_;
  int next_mutation_id_ = 1;
};

TEST_F(CrashTest, CrashAfterIntentLeavesIndeterminateAndReissueSucceeds) {
  ExpectCrashNeverAppliedThenReissueApplies("after_intent");
}

TEST_F(CrashTest, CrashAfterPendingLeavesIndeterminateAndReissueSucceeds) {
  ExpectCrashNeverAppliedThenReissueApplies("after_pending");
}

TEST_F(CrashTest, CrashAfterApplyReconciledNoDuplicationOnReissue) {
  ExpectCrashAlreadyAppliedThenReissueIsNoDuplication("after_apply");
}

TEST_F(CrashTest, CrashAfterOutcomeNoDuplicationOnReissue) {
  ExpectCrashAlreadyAppliedThenReissueIsNoDuplication("after_outcome");
}

TEST_F(CrashTest, CrashAfterApplyOnDeleteReconciledNoDuplicationOnReissue) {
  const shim::Transaction posted = PostTxnNormally(NextMutationId());
  ASSERT_EQ(QuerySplitCount(), 2);

  const std::string delete_mutation_id = NextMutationId();
  shim::DeleteTransactionRequest delete_request;
  delete_request.mutable_meta()->set_mutation_id(delete_mutation_id);
  delete_request.set_guid(posted.guid());

  RestartWithCrashPoint("after_apply");
  {
    grpc::ClientContext context;
    shim::Empty response;
    const grpc::Status status =
        txn_stub_->DeleteTransaction(&context, delete_request, &response);
    EXPECT_FALSE(status.ok())
        << "expected the daemon to crash mid-RPC during DeleteTransaction";
  }
  RestartClean();

  // The commit landed before the crash (after_apply); reconciliation
  // resolved the outcome, so the mutation is not indeterminate.
  EXPECT_FALSE(IsIndeterminate(delete_mutation_id));
  EXPECT_EQ(QuerySplitCount(), 0);

  grpc::ClientContext context;
  shim::Empty response;
  ASSERT_TRUE(
      txn_stub_->DeleteTransaction(&context, delete_request, &response).ok());
  EXPECT_EQ(QuerySplitCount(), 0);
}

}  // namespace
}  // namespace daichod::testing
