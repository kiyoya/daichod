#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "daemon_fixture.h"
#include "google/rpc/status.pb.h"

namespace daichod::testing {
namespace {

namespace shim = daicho::shim::v1;

// Fixed, canonically-formatted mutation ids (8-4-4-4-12 hex; the journal
// only validates shape, not RFC 4122 version/variant bits). mutation_id is a
// single global key in the journal (see Journal::GetOutcome), so every id
// used within one test's daemon instance must be unique across *all*
// services, not just the one being exercised.
std::string MutationId(int n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012x", n);
  return std::string(buf);
}

// Syntactically valid but non-existent 32-hex-char GUID.
constexpr char kNonexistentGuid[] = "deadbeefdeadbeefdeadbeefdeadbeef";

bool IsHexGuid(const std::string& s) {
  if (s.size() != 32) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

// Extracts the single packed ErrorDetail from a status's
// grpc-status-details-bin payload (a serialized google.rpc.Status).
shim::ErrorDetail UnpackDetail(const grpc::Status& status) {
  google::rpc::Status rpc_status;
  EXPECT_TRUE(rpc_status.ParseFromString(status.error_details()));
  EXPECT_EQ(rpc_status.details_size(), 1);
  shim::ErrorDetail detail;
  EXPECT_TRUE(rpc_status.details(0).UnpackTo(&detail));
  return detail;
}

shim::Numeric MakeNumeric(int64_t num, int64_t denom = 100) {
  shim::Numeric out;
  out.set_num(num);
  out.set_denom(denom);
  return out;
}

shim::Date MakeDate(int year, int month, int day) {
  shim::Date out;
  out.set_year(year);
  out.set_month(month);
  out.set_day(day);
  return out;
}

// Cross-multiplies rather than assuming denominators survive engine
// arithmetic unchanged.
void ExpectNumericEq(const shim::Numeric& expected, const shim::Numeric& actual) {
  ASSERT_NE(expected.denom(), 0);
  ASSERT_NE(actual.denom(), 0);
  EXPECT_EQ(expected.num() * actual.denom(), actual.num() * expected.denom())
      << "expected " << expected.num() << "/" << expected.denom() << " got "
      << actual.num() << "/" << actual.denom();
}

class TransactionServiceTest : public DaemonFixture {
 protected:
  void SetUp() override {
    DaemonFixture::SetUp();
    auto channel = Connect();
    account_stub_ = shim::AccountService::NewStub(channel);
    txn_stub_ = shim::TransactionService::NewStub(channel);
    balance_stub_ = shim::BalanceService::NewStub(channel);
    commodity_stub_ = shim::CommodityService::NewStub(channel);

    root_guid_ = RootGuid();
    asset_guid_ =
        CreateAccount(NextMutationId(), root_guid_, "Asset", shim::ACCOUNT_TYPE_ASSET)
            .guid();
    expense_guid_ =
        CreateAccount(NextMutationId(), root_guid_, "Expense", shim::ACCOUNT_TYPE_EXPENSE)
            .guid();
  }

  std::string NextMutationId() { return MutationId(next_mutation_id_++); }

  std::string RootGuid() {
    grpc::ClientContext context;
    shim::Empty request;
    shim::AccountList response;
    EXPECT_TRUE(account_stub_->ListAccounts(&context, request, &response).ok());
    for (const auto& account : response.accounts()) {
      if (account.type() == shim::ACCOUNT_TYPE_ROOT) return account.guid();
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
    EXPECT_TRUE(account_stub_->CreateAccount(&context, request, &response).ok());
    return response;
  }

  // One split to plan: value == quantity, since every test account here is
  // denominated in USD (matching the transaction currency).
  struct SplitSpec {
    std::string guid;  // empty: new split, engine assigns
    std::string account_guid;
    int64_t num;
    int64_t denom = 100;
  };

  shim::PostTransactionRequest BuildPostRequest(
      const std::string& mutation_id, const std::string& description,
      const shim::Date& post_date, const std::vector<SplitSpec>& splits,
      const std::string& currency_space = "CURRENCY",
      const std::string& currency_mnemonic = "USD") {
    shim::PostTransactionRequest request;
    request.mutable_meta()->set_mutation_id(mutation_id);
    shim::Transaction* spec = request.mutable_transaction();
    spec->mutable_currency()->set_space(currency_space);
    spec->mutable_currency()->set_mnemonic(currency_mnemonic);
    *spec->mutable_post_date() = post_date;
    spec->set_description(description);
    for (const auto& s : splits) {
      shim::Split* sp = spec->add_splits();
      if (!s.guid.empty()) sp->set_guid(s.guid);
      sp->set_account_guid(s.account_guid);
      sp->mutable_value()->set_num(s.num);
      sp->mutable_value()->set_denom(s.denom);
      sp->mutable_quantity()->set_num(s.num);
      sp->mutable_quantity()->set_denom(s.denom);
    }
    return request;
  }

  // Happy-path post: asserts success.
  shim::Transaction PostTxn(const std::string& mutation_id,
                            const std::string& description,
                            const shim::Date& post_date,
                            const std::vector<SplitSpec>& splits) {
    const shim::PostTransactionRequest request =
        BuildPostRequest(mutation_id, description, post_date, splits);
    grpc::ClientContext context;
    shim::Transaction response;
    const grpc::Status status =
        txn_stub_->PostTransaction(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response;
  }

  shim::Transaction GetTransaction(const std::string& guid) {
    grpc::ClientContext context;
    shim::TxnRef request;
    request.set_guid(guid);
    shim::Transaction response;
    EXPECT_TRUE(txn_stub_->GetTransaction(&context, request, &response).ok());
    return response;
  }

  grpc::Status GetTransactionStatus(const std::string& guid,
                                    shim::Transaction* response) {
    grpc::ClientContext context;
    shim::TxnRef request;
    request.set_guid(guid);
    return txn_stub_->GetTransaction(&context, request, response);
  }

  shim::QuerySplitsResponse QuerySplits(const shim::QuerySplitsRequest& request) {
    grpc::ClientContext context;
    shim::QuerySplitsResponse response;
    EXPECT_TRUE(txn_stub_->QuerySplits(&context, request, &response).ok());
    return response;
  }

  shim::Balance GetBalance(const shim::GetBalanceRequest& request) {
    grpc::ClientContext context;
    shim::Balance response;
    EXPECT_TRUE(balance_stub_->GetBalance(&context, request, &response).ok());
    return response;
  }

  shim::TrialBalance GetTrialBalance() {
    grpc::ClientContext context;
    shim::Empty request;
    shim::TrialBalance response;
    EXPECT_TRUE(
        balance_stub_->GetTrialBalance(&context, request, &response).ok());
    return response;
  }

  shim::CommodityList ListCommodities() {
    grpc::ClientContext context;
    shim::Empty request;
    shim::CommodityList response;
    EXPECT_TRUE(
        commodity_stub_->ListCommodities(&context, request, &response).ok());
    return response;
  }

  shim::Price AddPrice(const std::string& mutation_id,
                       const shim::CommodityId& commodity,
                       const shim::CommodityId& currency,
                       const shim::Date& date, const shim::Numeric& value) {
    shim::AddPriceRequest request;
    request.mutable_meta()->set_mutation_id(mutation_id);
    shim::Price* spec = request.mutable_price();
    *spec->mutable_commodity() = commodity;
    *spec->mutable_currency() = currency;
    *spec->mutable_date() = date;
    *spec->mutable_value() = value;
    grpc::ClientContext context;
    shim::Price response;
    EXPECT_TRUE(commodity_stub_->AddPrice(&context, request, &response).ok());
    return response;
  }

  shim::PriceList GetPrices(const shim::GetPricesRequest& request) {
    grpc::ClientContext context;
    shim::PriceList response;
    EXPECT_TRUE(commodity_stub_->GetPrices(&context, request, &response).ok());
    return response;
  }

  std::unique_ptr<shim::AccountService::Stub> account_stub_;
  std::unique_ptr<shim::TransactionService::Stub> txn_stub_;
  std::unique_ptr<shim::BalanceService::Stub> balance_stub_;
  std::unique_ptr<shim::CommodityService::Stub> commodity_stub_;
  std::string root_guid_;
  std::string asset_guid_;
  std::string expense_guid_;
  int next_mutation_id_ = 1;
};

// ------------------------------------------------------------- PostTransaction

TEST_F(TransactionServiceTest, PostBalancedTransactionAssignsGuidsAndRoundTrips) {
  const shim::Transaction posted =
      PostTxn(NextMutationId(), "Groceries", MakeDate(2026, 7, 1),
             {{"", asset_guid_, 1000, 100}, {"", expense_guid_, -1000, 100}});
  EXPECT_TRUE(IsHexGuid(posted.guid()));
  ASSERT_EQ(posted.splits_size(), 2);
  for (const auto& split : posted.splits()) EXPECT_TRUE(IsHexGuid(split.guid()));
  EXPECT_EQ(posted.description(), "Groceries");
  EXPECT_EQ(posted.currency().space(), "CURRENCY");
  EXPECT_EQ(posted.currency().mnemonic(), "USD");
  ASSERT_TRUE(posted.has_post_date());
  EXPECT_EQ(posted.post_date().year(), 2026);
  EXPECT_EQ(posted.post_date().month(), 7);
  EXPECT_EQ(posted.post_date().day(), 1);

  // Re-fetch independently: the timezone-bug canary. If the stored posted
  // date were not timezone-neutral, converting it back could land on a
  // different calendar day depending on the daemon's TZ.
  const shim::Transaction reread = GetTransaction(posted.guid());
  EXPECT_EQ(reread.description(), "Groceries");
  ASSERT_EQ(reread.splits_size(), 2);
  ASSERT_TRUE(reread.has_post_date());
  EXPECT_EQ(reread.post_date().year(), 2026);
  EXPECT_EQ(reread.post_date().month(), 7);
  EXPECT_EQ(reread.post_date().day(), 1);
}

TEST_F(TransactionServiceTest, PostTransactionIsIdempotentOnMutationId) {
  const std::string mutation_id = NextMutationId();
  const shim::Transaction first =
      PostTxn(mutation_id, "Rent", MakeDate(2026, 7, 1),
             {{"", asset_guid_, 1000, 100}, {"", expense_guid_, -1000, 100}});
  const shim::Transaction replay =
      PostTxn(mutation_id, "Rent", MakeDate(2026, 7, 1),
             {{"", asset_guid_, 1000, 100}, {"", expense_guid_, -1000, 100}});
  EXPECT_EQ(replay.guid(), first.guid());

  const shim::TrialBalance trial = GetTrialBalance();
  bool found_usd = false;
  for (const auto& entry : trial.entries()) {
    if (entry.currency().space() == "CURRENCY" &&
        entry.currency().mnemonic() == "USD") {
      found_usd = true;
      EXPECT_EQ(entry.sum().num(), 0);
    }
  }
  EXPECT_TRUE(found_usd);

  // Replay must not have created a second transaction: still exactly one
  // transaction's worth of splits in the book.
  const shim::QuerySplitsResponse splits = QuerySplits(shim::QuerySplitsRequest());
  EXPECT_EQ(splits.splits_size(), 2);
}

TEST_F(TransactionServiceTest, PostUnbalancedTransactionRejectedFailedPrecondition) {
  const shim::PostTransactionRequest request = BuildPostRequest(
      NextMutationId(), "Bad", MakeDate(2026, 7, 1),
      {{"", asset_guid_, 1000, 100}, {"", expense_guid_, -999, 100}});
  grpc::ClientContext context;
  shim::Transaction response;
  const grpc::Status status =
      txn_stub_->PostTransaction(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::ERROR_CODE_UNBALANCED_TRANSACTION);
}

TEST_F(TransactionServiceTest, PostTransactionRejectsUnknownAccount) {
  const shim::PostTransactionRequest request = BuildPostRequest(
      NextMutationId(), "Bad", MakeDate(2026, 7, 1),
      {{"", kNonexistentGuid, 1000, 100}, {"", expense_guid_, -1000, 100}});
  grpc::ClientContext context;
  shim::Transaction response;
  const grpc::Status status =
      txn_stub_->PostTransaction(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(TransactionServiceTest, PostTransactionRejectsGuidSet) {
  shim::PostTransactionRequest request;
  request.mutable_meta()->set_mutation_id(NextMutationId());
  request.mutable_transaction()->set_guid(kNonexistentGuid);
  grpc::ClientContext context;
  shim::Transaction response;
  const grpc::Status status =
      txn_stub_->PostTransaction(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(TransactionServiceTest, PostTransactionRejectsNoSplits) {
  const shim::PostTransactionRequest request =
      BuildPostRequest(NextMutationId(), "Bad", MakeDate(2026, 7, 1), {});
  grpc::ClientContext context;
  shim::Transaction response;
  const grpc::Status status =
      txn_stub_->PostTransaction(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(TransactionServiceTest, PostTransactionRejectsUnknownCurrency) {
  shim::PostTransactionRequest request;
  request.mutable_meta()->set_mutation_id(NextMutationId());
  shim::CommodityId* currency =
      request.mutable_transaction()->mutable_currency();
  currency->set_space("BOGUS");
  currency->set_mnemonic("ZZZ");
  grpc::ClientContext context;
  shim::Transaction response;
  const grpc::Status status =
      txn_stub_->PostTransaction(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::ERROR_CODE_COMMODITY_NOT_FOUND);
}

// ----------------------------------------------------------- UpdateTransaction

TEST_F(TransactionServiceTest, UpdateTransactionChangesDescriptionAndAmountsKeepingGuids) {
  const shim::Transaction original =
      PostTxn(NextMutationId(), "Original", MakeDate(2026, 7, 1),
             {{"", asset_guid_, 1000, 100}, {"", expense_guid_, -1000, 100}});
  ASSERT_EQ(original.splits_size(), 2);

  shim::UpdateTransactionRequest request;
  request.mutable_meta()->set_mutation_id(NextMutationId());
  shim::Transaction* spec = request.mutable_transaction();
  spec->set_guid(original.guid());
  spec->mutable_currency()->set_space("CURRENCY");
  spec->mutable_currency()->set_mnemonic("USD");
  *spec->mutable_post_date() = MakeDate(2026, 7, 1);
  spec->set_description("Updated");
  for (const shim::Split& orig_split : original.splits()) {
    shim::Split* sp = spec->add_splits();
    sp->set_guid(orig_split.guid());
    sp->set_account_guid(orig_split.account_guid());
    const int64_t doubled = orig_split.value().num() * 2;  // still balanced
    sp->mutable_value()->set_num(doubled);
    sp->mutable_value()->set_denom(orig_split.value().denom());
    sp->mutable_quantity()->set_num(doubled);
    sp->mutable_quantity()->set_denom(orig_split.value().denom());
  }

  grpc::ClientContext context;
  shim::Transaction response;
  ASSERT_TRUE(txn_stub_->UpdateTransaction(&context, request, &response).ok());
  EXPECT_EQ(response.description(), "Updated");
  ASSERT_EQ(response.splits_size(), 2);

  std::set<std::string> original_guids;
  for (const auto& s : original.splits()) original_guids.insert(s.guid());
  std::set<std::string> updated_guids;
  for (const auto& s : response.splits()) updated_guids.insert(s.guid());
  EXPECT_EQ(original_guids, updated_guids);

  const shim::Transaction reread = GetTransaction(original.guid());
  EXPECT_EQ(reread.description(), "Updated");
  for (const auto& s : reread.splits()) {
    if (s.value().num() > 0) {
      EXPECT_EQ(s.value().num(), 2000);
    } else {
      EXPECT_EQ(s.value().num(), -2000);
    }
  }
}

TEST_F(TransactionServiceTest, UpdateTransactionReplacesOneSplitAndDropsAnother) {
  const std::string expense2_guid =
      CreateAccount(NextMutationId(), root_guid_, "Expense2", shim::ACCOUNT_TYPE_EXPENSE)
          .guid();
  const shim::Transaction original =
      PostTxn(NextMutationId(), "Original2", MakeDate(2026, 7, 2),
             {{"", asset_guid_, 500, 100}, {"", expense_guid_, -500, 100}});
  ASSERT_EQ(original.splits_size(), 2);
  std::string asset_split_guid, expense_split_guid;
  for (const auto& s : original.splits()) {
    if (s.account_guid() == asset_guid_) asset_split_guid = s.guid();
    if (s.account_guid() == expense_guid_) expense_split_guid = s.guid();
  }
  ASSERT_FALSE(asset_split_guid.empty());
  ASSERT_FALSE(expense_split_guid.empty());

  shim::UpdateTransactionRequest request;
  request.mutable_meta()->set_mutation_id(NextMutationId());
  shim::Transaction* spec = request.mutable_transaction();
  spec->set_guid(original.guid());
  spec->mutable_currency()->set_space("CURRENCY");
  spec->mutable_currency()->set_mnemonic("USD");
  *spec->mutable_post_date() = MakeDate(2026, 7, 2);
  spec->set_description("Original2");
  // Keep the asset split unchanged.
  shim::Split* kept = spec->add_splits();
  kept->set_guid(asset_split_guid);
  kept->set_account_guid(asset_guid_);
  kept->mutable_value()->set_num(500);
  kept->mutable_value()->set_denom(100);
  kept->mutable_quantity()->set_num(500);
  kept->mutable_quantity()->set_denom(100);
  // Replace the expense split with a brand-new one on a different account:
  // an empty guid means "new", and omitting expense_split_guid destroys it.
  shim::Split* replacement = spec->add_splits();
  replacement->set_account_guid(expense2_guid);
  replacement->mutable_value()->set_num(-500);
  replacement->mutable_value()->set_denom(100);
  replacement->mutable_quantity()->set_num(-500);
  replacement->mutable_quantity()->set_denom(100);

  grpc::ClientContext context;
  shim::Transaction response;
  ASSERT_TRUE(txn_stub_->UpdateTransaction(&context, request, &response).ok());
  ASSERT_EQ(response.splits_size(), 2);
  bool found_kept = false;
  bool found_new = false;
  std::string new_guid;
  for (const auto& s : response.splits()) {
    if (s.guid() == asset_split_guid) found_kept = true;
    if (s.guid() != asset_split_guid && s.guid() != expense_split_guid) {
      found_new = true;
      new_guid = s.guid();
      EXPECT_EQ(s.account_guid(), expense2_guid);
    }
  }
  EXPECT_TRUE(found_kept);
  EXPECT_TRUE(found_new);
  EXPECT_TRUE(IsHexGuid(new_guid));

  const shim::Transaction reread = GetTransaction(original.guid());
  ASSERT_EQ(reread.splits_size(), 2);
  for (const auto& s : reread.splits()) {
    EXPECT_NE(s.guid(), expense_split_guid);  // destroyed by the update
  }
}

TEST_F(TransactionServiceTest, UpdateTransactionUnbalancedLeavesOriginalUnchanged) {
  const shim::Transaction original =
      PostTxn(NextMutationId(), "Stays", MakeDate(2026, 7, 3),
             {{"", asset_guid_, 300, 100}, {"", expense_guid_, -300, 100}});

  shim::UpdateTransactionRequest request;
  request.mutable_meta()->set_mutation_id(NextMutationId());
  shim::Transaction* spec = request.mutable_transaction();
  spec->set_guid(original.guid());
  spec->mutable_currency()->set_space("CURRENCY");
  spec->mutable_currency()->set_mnemonic("USD");
  *spec->mutable_post_date() = MakeDate(2026, 7, 3);
  spec->set_description("ShouldNotApply");
  for (const shim::Split& orig_split : original.splits()) {
    shim::Split* sp = spec->add_splits();
    sp->set_guid(orig_split.guid());
    sp->set_account_guid(orig_split.account_guid());
    // Perturb only the negative leg by 1 cent so the sum no longer balances.
    const int64_t bump = orig_split.value().num() < 0 ? 1 : 0;
    sp->mutable_value()->set_num(orig_split.value().num() + bump);
    sp->mutable_value()->set_denom(orig_split.value().denom());
    sp->mutable_quantity()->set_num(orig_split.value().num() + bump);
    sp->mutable_quantity()->set_denom(orig_split.value().denom());
  }

  grpc::ClientContext context;
  shim::Transaction response;
  const grpc::Status status =
      txn_stub_->UpdateTransaction(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::ERROR_CODE_UNBALANCED_TRANSACTION);

  const shim::Transaction reread = GetTransaction(original.guid());
  EXPECT_EQ(reread.description(), "Stays");
  ASSERT_EQ(reread.splits_size(), 2);
  for (const auto& s : reread.splits()) {
    if (s.value().num() > 0) {
      EXPECT_EQ(s.value().num(), 300);
    } else {
      EXPECT_EQ(s.value().num(), -300);
    }
  }
}

// ----------------------------------------------------------- DeleteTransaction

TEST_F(TransactionServiceTest, DeleteTransactionRemovesAndIsIdempotent) {
  const shim::Transaction txn =
      PostTxn(NextMutationId(), "ToDelete", MakeDate(2026, 7, 4),
             {{"", asset_guid_, 200, 100}, {"", expense_guid_, -200, 100}});
  const std::string delete_mutation_id = NextMutationId();
  {
    shim::DeleteTransactionRequest request;
    request.mutable_meta()->set_mutation_id(delete_mutation_id);
    request.set_guid(txn.guid());
    grpc::ClientContext context;
    shim::Empty response;
    ASSERT_TRUE(txn_stub_->DeleteTransaction(&context, request, &response).ok());
  }
  {
    shim::Transaction response;
    const grpc::Status status = GetTransactionStatus(txn.guid(), &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
  }
  {
    // Idempotent replay of the same mutation_id: recorded outcome, still OK.
    shim::DeleteTransactionRequest request;
    request.mutable_meta()->set_mutation_id(delete_mutation_id);
    request.set_guid(txn.guid());
    grpc::ClientContext context;
    shim::Empty response;
    EXPECT_TRUE(txn_stub_->DeleteTransaction(&context, request, &response).ok());
  }
}

// ------------------------------------------------------------------ QuerySplits

TEST_F(TransactionServiceTest, QuerySplitsFiltersByAccountDateDescriptionAndPaginates) {
  const std::string expense2_guid =
      CreateAccount(NextMutationId(), root_guid_, "Expense2", shim::ACCOUNT_TYPE_EXPENSE)
          .guid();

  PostTxn(NextMutationId(), "Groceries weekly", MakeDate(2026, 7, 1),
         {{"", asset_guid_, 100, 100}, {"", expense_guid_, -100, 100}});
  PostTxn(NextMutationId(), "Rent payment", MakeDate(2026, 7, 3),
         {{"", asset_guid_, 200, 100}, {"", expense_guid_, -200, 100}});
  PostTxn(NextMutationId(), "Utilities bill", MakeDate(2026, 7, 5),
         {{"", asset_guid_, 300, 100}, {"", expense2_guid, -300, 100}});
  PostTxn(NextMutationId(), "Groceries again", MakeDate(2026, 7, 10),
         {{"", asset_guid_, 400, 100}, {"", expense2_guid, -400, 100}});
  PostTxn(NextMutationId(), "Misc groceries run", MakeDate(2026, 7, 15),
         {{"", asset_guid_, 500, 100}, {"", expense_guid_, -500, 100}});

  // Filter by account: only expense_guid_'s own splits (from txns 1, 2, 5).
  // Fewer matches than the default page size, so this also exercises the
  // empty-next_page_token "no more results" terminal case.
  {
    shim::QuerySplitsRequest request;
    request.add_account_guids(expense_guid_);
    const shim::QuerySplitsResponse response = QuerySplits(request);
    EXPECT_EQ(response.splits_size(), 3);
    EXPECT_TRUE(response.next_page_token().empty());
    for (const auto& s : response.splits()) {
      EXPECT_EQ(s.split().account_guid(), expense_guid_);
    }
  }

  // Inclusive date range 07-03..07-10 covers txns 2, 3, 4 => 6 splits.
  {
    shim::QuerySplitsRequest request;
    *request.mutable_date_from() = MakeDate(2026, 7, 3);
    *request.mutable_date_to() = MakeDate(2026, 7, 10);
    const shim::QuerySplitsResponse response = QuerySplits(request);
    EXPECT_EQ(response.splits_size(), 6);
    for (const auto& s : response.splits()) {
      EXPECT_GE(s.post_date().day(), 3);
      EXPECT_LE(s.post_date().day(), 10);
    }
  }

  // description_contains is case-insensitive: matches txns 1, 4, 5 => 6 splits.
  {
    shim::QuerySplitsRequest request;
    request.set_description_contains("groceries");
    const shim::QuerySplitsResponse response = QuerySplits(request);
    EXPECT_EQ(response.splits_size(), 6);
    for (const auto& s : response.splits()) {
      std::string lower = s.description();
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      EXPECT_NE(lower.find("groceries"), std::string::npos);
    }
  }

  // Pagination over all 10 splits, page_size 3: first page full with a
  // token, second page disjoint from the first.
  std::set<std::string> page1_guids;
  {
    shim::QuerySplitsRequest request;
    request.set_page_size(3);
    const shim::QuerySplitsResponse page1 = QuerySplits(request);
    EXPECT_EQ(page1.splits_size(), 3);
    EXPECT_FALSE(page1.next_page_token().empty());
    for (const auto& s : page1.splits()) page1_guids.insert(s.split().guid());

    shim::QuerySplitsRequest request2;
    request2.set_page_size(3);
    request2.set_page_token(page1.next_page_token());
    const shim::QuerySplitsResponse page2 = QuerySplits(request2);
    EXPECT_EQ(page2.splits_size(), 3);
    for (const auto& s : page2.splits()) {
      EXPECT_EQ(page1_guids.count(s.split().guid()), 0u);
    }
  }

  // A page_token that matches no split in the current result set is rejected.
  {
    shim::QuerySplitsRequest request;
    request.set_page_token(kNonexistentGuid);
    grpc::ClientContext context;
    shim::QuerySplitsResponse response;
    const grpc::Status status =
        txn_stub_->QuerySplits(&context, request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }

  // Every split defaults to NOT_RECONCILED: the filter matches all 10.
  {
    shim::QuerySplitsRequest request;
    request.add_states(shim::RECONCILE_STATE_NOT_RECONCILED);
    const shim::QuerySplitsResponse response = QuerySplits(request);
    EXPECT_EQ(response.splits_size(), 10);
  }
}

// ---------------------------------------------------------------- BalanceService

TEST_F(TransactionServiceTest, GetBalanceSumsPostedTransactionsAndRespectsAsOf) {
  PostTxn(NextMutationId(), "T1", MakeDate(2026, 7, 1),
         {{"", asset_guid_, 1000, 100}, {"", expense_guid_, -1000, 100}});
  PostTxn(NextMutationId(), "T2", MakeDate(2026, 7, 5),
         {{"", asset_guid_, 500, 100}, {"", expense_guid_, -500, 100}});

  {
    shim::GetBalanceRequest request;
    request.set_account_guid(asset_guid_);
    const shim::Balance balance = GetBalance(request);
    ExpectNumericEq(MakeNumeric(1500, 100), balance.amount());
    EXPECT_EQ(balance.commodity().mnemonic(), "USD");
  }
  {
    // Before either transaction posted: zero.
    shim::GetBalanceRequest request;
    request.set_account_guid(asset_guid_);
    *request.mutable_as_of() = MakeDate(2026, 6, 1);
    const shim::Balance balance = GetBalance(request);
    EXPECT_EQ(balance.amount().num(), 0);
  }
}

TEST_F(TransactionServiceTest, GetBalanceIncludesChildrenWhenRequested) {
  const std::string parent_guid =
      CreateAccount(NextMutationId(), root_guid_, "Parent", shim::ACCOUNT_TYPE_ASSET)
          .guid();
  const std::string child_guid =
      CreateAccount(NextMutationId(), parent_guid, "Child", shim::ACCOUNT_TYPE_ASSET)
          .guid();
  PostTxn(NextMutationId(), "IntoChild", MakeDate(2026, 7, 1),
         {{"", child_guid, 700, 100}, {"", expense_guid_, -700, 100}});

  shim::GetBalanceRequest without_children_request;
  without_children_request.set_account_guid(parent_guid);
  const shim::Balance without_children = GetBalance(without_children_request);
  EXPECT_EQ(without_children.amount().num(), 0);

  shim::GetBalanceRequest with_children_request;
  with_children_request.set_account_guid(parent_guid);
  with_children_request.set_include_children(true);
  const shim::Balance with_children = GetBalance(with_children_request);
  ExpectNumericEq(MakeNumeric(700, 100), with_children.amount());
}

TEST_F(TransactionServiceTest, GetTrialBalanceIsZeroForHealthyBook) {
  PostTxn(NextMutationId(), "T1", MakeDate(2026, 7, 1),
         {{"", asset_guid_, 250, 100}, {"", expense_guid_, -250, 100}});
  const shim::TrialBalance trial = GetTrialBalance();
  bool found_usd = false;
  for (const auto& entry : trial.entries()) {
    if (entry.currency().space() == "CURRENCY" &&
        entry.currency().mnemonic() == "USD") {
      found_usd = true;
      EXPECT_EQ(entry.sum().num(), 0);
    }
  }
  EXPECT_TRUE(found_usd);
}

// -------------------------------------------------------------- CommodityService

TEST_F(TransactionServiceTest, ListCommoditiesContainsUsd) {
  const shim::CommodityList list = ListCommodities();
  bool found = false;
  for (const auto& commodity : list.commodities()) {
    if (commodity.id().space() == "CURRENCY" &&
        commodity.id().mnemonic() == "USD") {
      found = true;
      EXPECT_TRUE(commodity.is_currency());
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(TransactionServiceTest, AddPriceAssignsGuidAndRoundTripsThroughGetPrices) {
  shim::CommodityId eur;
  eur.set_space("CURRENCY");
  eur.set_mnemonic("EUR");
  shim::CommodityId usd;
  usd.set_space("CURRENCY");
  usd.set_mnemonic("USD");
  const shim::Numeric value = MakeNumeric(108, 100);
  const shim::Date date = MakeDate(2026, 7, 1);

  const std::string mutation_id = NextMutationId();
  const shim::Price added = AddPrice(mutation_id, eur, usd, date, value);
  EXPECT_TRUE(IsHexGuid(added.guid()));

  shim::GetPricesRequest request;
  *request.mutable_commodity() = eur;
  *request.mutable_currency() = usd;
  const shim::PriceList prices = GetPrices(request);
  ASSERT_EQ(prices.prices_size(), 1);
  const shim::Price& price = prices.prices(0);
  EXPECT_EQ(price.guid(), added.guid());
  EXPECT_EQ(price.date().year(), 2026);
  EXPECT_EQ(price.date().month(), 7);
  EXPECT_EQ(price.date().day(), 1);
  ExpectNumericEq(value, price.value());

  // Idempotent replay: same guid, no second price recorded.
  const shim::Price replay = AddPrice(mutation_id, eur, usd, date, value);
  EXPECT_EQ(replay.guid(), added.guid());
  const shim::PriceList prices_after = GetPrices(request);
  EXPECT_EQ(prices_after.prices_size(), 1);
}

}  // namespace
}  // namespace daichod::testing
