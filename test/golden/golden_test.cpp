// Golden-book round-trip: scripted mutations through the shim, then the
// same book opened by the pinned desktop gnucash-cli. The desktop tool is
// the arbiter — if it loads the book without integrity complaints and its
// report shows the amounts the shim posted, shim and desktop agree.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "daemon_fixture.h"

namespace daichod::testing {
namespace {

namespace shim = daicho::shim::v1;

// Local helpers: run a child with stderr captured to a file, and slurp one.
int RunCommandCapture(const std::vector<std::string>& argv,
                      const std::string& stderr_path) {
  const pid_t pid = fork();
  if (pid == 0) {
    FILE* err = std::freopen(stderr_path.c_str(), "w", stderr);
    (void)err;
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    execv(args[0], args.data());
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string ReadFile(const std::string& path) {
  std::ifstream stream(path);
  std::ostringstream out;
  out << stream.rdbuf();
  return out.str();
}

class GoldenTest : public DaemonFixture {
 protected:
  std::string CreateAccount(shim::AccountService::Stub* accounts,
                            const std::string& parent_guid,
                            const std::string& name, shim::AccountType type,
                            const std::string& mutation_id) {
    grpc::ClientContext context;
    shim::CreateAccountRequest request;
    request.mutable_meta()->set_mutation_id(mutation_id);
    shim::Account* spec = request.mutable_account();
    spec->set_parent_guid(parent_guid);
    spec->set_name(name);
    spec->set_type(type);
    spec->mutable_commodity()->set_space("CURRENCY");
    spec->mutable_commodity()->set_mnemonic("USD");
    shim::Account response;
    EXPECT_TRUE(accounts->CreateAccount(&context, request, &response).ok());
    return response.guid();
  }

  void PostSimple(shim::TransactionService::Stub* transactions,
                  const std::string& debit_guid,
                  const std::string& credit_guid, int64_t cents,
                  const std::string& description,
                  const std::string& mutation_id) {
    grpc::ClientContext context;
    shim::PostTransactionRequest request;
    request.mutable_meta()->set_mutation_id(mutation_id);
    shim::Transaction* txn = request.mutable_transaction();
    txn->mutable_currency()->set_space("CURRENCY");
    txn->mutable_currency()->set_mnemonic("USD");
    txn->mutable_post_date()->set_year(2026);
    txn->mutable_post_date()->set_month(7);
    txn->mutable_post_date()->set_day(10);
    txn->set_description(description);
    shim::Split* debit = txn->add_splits();
    debit->set_account_guid(debit_guid);
    debit->mutable_value()->set_num(cents);
    debit->mutable_value()->set_denom(100);
    debit->mutable_quantity()->set_num(cents);
    debit->mutable_quantity()->set_denom(100);
    shim::Split* credit = txn->add_splits();
    credit->set_account_guid(credit_guid);
    credit->mutable_value()->set_num(-cents);
    credit->mutable_value()->set_denom(100);
    credit->mutable_quantity()->set_num(-cents);
    credit->mutable_quantity()->set_denom(100);
    shim::Transaction response;
    EXPECT_TRUE(
        transactions->PostTransaction(&context, request, &response).ok());
  }
};

TEST_F(GoldenTest, DesktopAgreesWithShim) {
  auto channel = Connect();
  auto accounts = shim::AccountService::NewStub(channel);
  auto transactions = shim::TransactionService::NewStub(channel);
  auto session = shim::SessionService::NewStub(channel);

  grpc::ClientContext info_context;
  shim::Empty empty;
  shim::BookInfo info;
  ASSERT_TRUE(session->GetBookInfo(&info_context, empty, &info).ok());
  const std::string root = info.root_account_guid();

  const std::string checking =
      CreateAccount(accounts.get(), root, "Checking", shim::BANK,
                    "00000000-0000-4000-8000-00000000a001");
  const std::string salary =
      CreateAccount(accounts.get(), root, "Salary", shim::INCOME,
                    "00000000-0000-4000-8000-00000000a002");
  const std::string groceries =
      CreateAccount(accounts.get(), root, "Groceries", shim::EXPENSE,
                    "00000000-0000-4000-8000-00000000a003");

  PostSimple(transactions.get(), checking, salary, 250000, "July salary",
             "00000000-0000-4000-8000-00000000a004");
  PostSimple(transactions.get(), groceries, checking, 13456, "Market run",
             "00000000-0000-4000-8000-00000000a005");

  // Clean shutdown so the desktop tool sees a fully committed book.
  StopDaemon();

  // The pinned gnucash-cli opens the book and renders a Trial Balance.
  const std::string report_path = (dir_ / "trial-balance.html").string();
  const std::string stderr_path = (dir_ / "gnucash-cli.err").string();
  const int exit_code = RunCommandCapture(
      {"/opt/gnucash/bin/gnucash-cli", "--report", "run", "--name",
       "Trial Balance", "--output-file", report_path, book_uri_},
      stderr_path);
  ASSERT_EQ(exit_code, 0) << ReadFile(stderr_path);

  const std::string report = ReadFile(report_path);
  // The shim's amounts, as the desktop renders them.
  EXPECT_NE(report.find("2,500.00"), std::string::npos) << report_path;
  EXPECT_NE(report.find("134.56"), std::string::npos) << report_path;

  // Integrity complaints the desktop would log on load.
  const std::string errors = ReadFile(stderr_path);
  EXPECT_EQ(errors.find("Imbalance"), std::string::npos) << errors;
  EXPECT_EQ(errors.find("Orphan"), std::string::npos) << errors;

  // Round-trip back: the shim reopens the book the desktop just touched.
  StartDaemon(daichod_bin_);
  auto channel2 = Connect();
  auto balances = shim::BalanceService::NewStub(channel2);
  grpc::ClientContext balance_context;
  shim::TrialBalance trial;
  ASSERT_TRUE(balances->GetTrialBalance(&balance_context, empty, &trial).ok());
  for (const auto& entry : trial.entries()) {
    EXPECT_EQ(entry.sum().num(), 0) << entry.currency().mnemonic();
  }
}

}  // namespace
}  // namespace daichod::testing
