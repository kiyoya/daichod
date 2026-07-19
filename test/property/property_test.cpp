// Property tests: randomly generated balanced transactions must be accepted
// by the engine, and the book's invariants must hold afterwards. Seeded
// PRNG rather than a property-testing framework — failures reproduce
// exactly from the seed printed in the test name, and the dev image needs
// no extra dependency.

#include <cinttypes>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "daemon_fixture.h"

namespace daichod::testing {
namespace {

namespace shim = daicho::shim::v1;

class PropertyTest : public DaemonFixture,
                     public ::testing::WithParamInterface<unsigned> {
 protected:
  std::string NextMutationId() {
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer),
                  "%08x-0000-4000-8000-%012x", GetParam(), ++counter_);
    return buffer;
  }
  unsigned counter_ = 0;
};

TEST_P(PropertyTest, RandomBalancedTransactionsHoldInvariants) {
  std::mt19937 rng(GetParam());
  auto channel = Connect();
  auto session = shim::SessionService::NewStub(channel);
  auto accounts = shim::AccountService::NewStub(channel);
  auto transactions = shim::TransactionService::NewStub(channel);
  auto balances = shim::BalanceService::NewStub(channel);

  shim::Empty empty;
  shim::BookInfo info;
  {
    grpc::ClientContext context;
    ASSERT_TRUE(session->GetBookInfo(&context, empty, &info).ok());
  }

  // A small random forest of accounts.
  const shim::AccountType kTypes[] = {shim::ASSET, shim::BANK, shim::EXPENSE,
                                      shim::INCOME, shim::LIABILITY,
                                      shim::EQUITY};
  std::vector<std::string> account_guids;
  const int account_count = 3 + static_cast<int>(rng() % 5);
  for (int i = 0; i < account_count; ++i) {
    grpc::ClientContext context;
    shim::CreateAccountRequest request;
    request.mutable_meta()->set_mutation_id(NextMutationId());
    shim::Account* spec = request.mutable_account();
    spec->set_parent_guid(info.root_account_guid());
    spec->set_name("Acct" + std::to_string(i));
    spec->set_type(kTypes[rng() % (sizeof(kTypes) / sizeof(kTypes[0]))]);
    spec->mutable_commodity()->set_space("CURRENCY");
    spec->mutable_commodity()->set_mnemonic("USD");
    shim::Account response;
    ASSERT_TRUE(accounts->CreateAccount(&context, request, &response).ok());
    account_guids.push_back(response.guid());
  }

  // Random balanced transactions: 2-5 splits, cents in [-10^7, 10^7],
  // dates across four decades, the last split absorbing the remainder.
  int total_splits = 0;
  const int txn_count = 20;
  for (int t = 0; t < txn_count; ++t) {
    grpc::ClientContext context;
    shim::PostTransactionRequest request;
    request.mutable_meta()->set_mutation_id(NextMutationId());
    shim::Transaction* txn = request.mutable_transaction();
    txn->mutable_currency()->set_space("CURRENCY");
    txn->mutable_currency()->set_mnemonic("USD");
    txn->mutable_post_date()->set_year(1990 + static_cast<int>(rng() % 40));
    txn->mutable_post_date()->set_month(1 + static_cast<int>(rng() % 12));
    txn->mutable_post_date()->set_day(1 + static_cast<int>(rng() % 28));
    txn->set_description("prop txn " + std::to_string(t) + " ünïcode ✓");

    const int split_count = 2 + static_cast<int>(rng() % 4);
    int64_t remainder = 0;
    for (int s = 0; s < split_count; ++s) {
      shim::Split* split = txn->add_splits();
      split->set_account_guid(account_guids[rng() % account_guids.size()]);
      int64_t cents;
      if (s + 1 == split_count) {
        cents = -remainder;
      } else {
        cents = static_cast<int64_t>(rng() % 20000001) - 10000000;
        remainder += cents;
      }
      split->mutable_value()->set_num(cents);
      split->mutable_value()->set_denom(100);
      split->mutable_quantity()->set_num(cents);
      split->mutable_quantity()->set_denom(100);
    }
    shim::Transaction response;
    const grpc::Status status =
        transactions->PostTransaction(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    total_splits += split_count;

    // Round-trip: what the engine stored is what was sent.
    grpc::ClientContext get_context;
    shim::TxnRef ref;
    ref.set_guid(response.guid());
    shim::Transaction fetched;
    ASSERT_TRUE(
        transactions->GetTransaction(&get_context, ref, &fetched).ok());
    ASSERT_EQ(fetched.splits_size(), split_count);
    int64_t fetched_sum = 0;
    for (const shim::Split& split : fetched.splits()) {
      ASSERT_EQ(split.value().denom(), 100);
      fetched_sum += split.value().num();
    }
    EXPECT_EQ(fetched_sum, 0);
    EXPECT_EQ(fetched.post_date().year(), txn->post_date().year());
    EXPECT_EQ(fetched.post_date().month(), txn->post_date().month());
    EXPECT_EQ(fetched.post_date().day(), txn->post_date().day());
  }

  // Invariants: trial balance is zero in every currency, and the split
  // universe matches what was posted.
  {
    grpc::ClientContext context;
    shim::TrialBalance trial;
    ASSERT_TRUE(balances->GetTrialBalance(&context, empty, &trial).ok());
    for (const auto& entry : trial.entries()) {
      EXPECT_EQ(entry.sum().num(), 0) << entry.currency().mnemonic();
    }
  }
  {
    int listed = 0;
    std::string token;
    do {
      grpc::ClientContext context;
      shim::QuerySplitsRequest request;
      request.set_page_size(500);
      request.set_page_token(token);
      shim::QuerySplitsResponse response;
      ASSERT_TRUE(
          transactions->QuerySplits(&context, request, &response).ok());
      listed += response.splits_size();
      token = response.next_page_token();
    } while (!token.empty());
    EXPECT_EQ(listed, total_splits);
  }
}

INSTANTIATE_TEST_SUITE_P(Seeds, PropertyTest,
                         ::testing::Values(1u, 42u, 20260719u));

}  // namespace
}  // namespace daichod::testing
