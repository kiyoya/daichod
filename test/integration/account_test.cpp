#include <cstdio>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "daemon_fixture.h"
#include "google/rpc/status.pb.h"

namespace daichod::testing {
namespace {

namespace shim = daicho::shim::v1;

// Fixed, canonically-formatted mutation ids (8-4-4-4-12 hex; the journal
// only validates shape, not RFC 4122 version/variant bits).
std::string MutationId(int n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012x", n);
  return std::string(buf);
}

// Syntactically valid but non-existent 32-hex-char GUID.
constexpr char kNonexistentGuid[] = "deadbeefdeadbeefdeadbeefdeadbeef";

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

class AccountServiceTest : public DaemonFixture {
 protected:
  void SetUp() override {
    DaemonFixture::SetUp();
    stub_ = shim::AccountService::NewStub(Connect());
  }

  std::string RootGuid() {
    grpc::ClientContext context;
    shim::Empty request;
    shim::AccountList response;
    EXPECT_TRUE(stub_->ListAccounts(&context, request, &response).ok());
    for (const auto& account : response.accounts()) {
      if (account.type() == shim::ACCOUNT_TYPE_ROOT) return account.guid();
    }
    ADD_FAILURE() << "no root account in ListAccounts response";
    return "";
  }

  shim::Account CreateAccount(const std::string& mutation_id,
                              const std::string& parent_guid,
                              const std::string& name,
                              shim::AccountType type = shim::ACCOUNT_TYPE_ASSET) {
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
    EXPECT_TRUE(stub_->CreateAccount(&context, request, &response).ok());
    return response;
  }

  shim::Account GetAccount(const std::string& guid) {
    grpc::ClientContext context;
    shim::AccountRef request;
    request.set_guid(guid);
    shim::Account response;
    EXPECT_TRUE(stub_->GetAccount(&context, request, &response).ok());
    return response;
  }

  int ListAccountsCount() {
    grpc::ClientContext context;
    shim::Empty request;
    shim::AccountList response;
    EXPECT_TRUE(stub_->ListAccounts(&context, request, &response).ok());
    return response.accounts_size();
  }

  std::unique_ptr<shim::AccountService::Stub> stub_;
};

TEST_F(AccountServiceTest, ListAccountsOnFreshBookHasOnlyRoot) {
  grpc::ClientContext context;
  shim::Empty request;
  shim::AccountList response;
  ASSERT_TRUE(stub_->ListAccounts(&context, request, &response).ok());
  ASSERT_EQ(response.accounts_size(), 1);
  const shim::Account& root = response.accounts(0);
  EXPECT_EQ(root.type(), shim::ACCOUNT_TYPE_ROOT);
  EXPECT_TRUE(root.parent_guid().empty());
  EXPECT_EQ(root.guid().size(), 32u);
}

TEST_F(AccountServiceTest, CreateAccountAddsChildOfRoot) {
  const std::string root_guid = RootGuid();
  const shim::Account created =
      CreateAccount(MutationId(1), root_guid, "Checking");
  EXPECT_EQ(created.parent_guid(), root_guid);
  EXPECT_EQ(created.name(), "Checking");
  EXPECT_EQ(created.type(), shim::ACCOUNT_TYPE_ASSET);
  EXPECT_EQ(created.commodity().space(), "CURRENCY");
  EXPECT_EQ(created.commodity().mnemonic(), "USD");
  EXPECT_EQ(created.guid().size(), 32u);
  EXPECT_EQ(ListAccountsCount(), 2);
}

TEST_F(AccountServiceTest, CreateAccountIsIdempotentOnMutationId) {
  const std::string root_guid = RootGuid();
  const shim::Account first =
      CreateAccount(MutationId(1), root_guid, "Checking");
  ASSERT_EQ(ListAccountsCount(), 2);

  // Same mutation_id, identical request: replayed outcome, no new account.
  const shim::Account replay =
      CreateAccount(MutationId(1), root_guid, "Checking");
  EXPECT_EQ(replay.guid(), first.guid());
  EXPECT_EQ(ListAccountsCount(), 2);

  // New mutation_id, same name: the engine allows duplicate names, so this
  // is a genuinely new account.
  const shim::Account second =
      CreateAccount(MutationId(2), root_guid, "Checking");
  EXPECT_NE(second.guid(), first.guid());
  EXPECT_EQ(ListAccountsCount(), 3);
}

TEST_F(AccountServiceTest, CreateAccountRejectsNonexistentParent) {
  shim::CreateAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(1));
  shim::Account* spec = request.mutable_account();
  spec->set_parent_guid(kNonexistentGuid);
  spec->set_name("Checking");
  spec->set_type(shim::ACCOUNT_TYPE_ASSET);
  spec->mutable_commodity()->set_space("CURRENCY");
  spec->mutable_commodity()->set_mnemonic("USD");
  grpc::ClientContext context;
  shim::Account response;
  const grpc::Status status =
      stub_->CreateAccount(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(AccountServiceTest, CreateAccountRejectsEmptyName) {
  const std::string root_guid = RootGuid();
  shim::CreateAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(1));
  shim::Account* spec = request.mutable_account();
  spec->set_parent_guid(root_guid);
  spec->set_name("");
  spec->set_type(shim::ACCOUNT_TYPE_ASSET);
  spec->mutable_commodity()->set_space("CURRENCY");
  spec->mutable_commodity()->set_mnemonic("USD");
  grpc::ClientContext context;
  shim::Account response;
  const grpc::Status status =
      stub_->CreateAccount(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(AccountServiceTest, CreateAccountRejectsGuidSet) {
  const std::string root_guid = RootGuid();
  shim::CreateAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(1));
  shim::Account* spec = request.mutable_account();
  spec->set_guid(kNonexistentGuid);
  spec->set_parent_guid(root_guid);
  spec->set_name("Checking");
  spec->set_type(shim::ACCOUNT_TYPE_ASSET);
  spec->mutable_commodity()->set_space("CURRENCY");
  spec->mutable_commodity()->set_mnemonic("USD");
  grpc::ClientContext context;
  shim::Account response;
  const grpc::Status status =
      stub_->CreateAccount(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(AccountServiceTest, UpdateAccountRenamesAndPersistsFields) {
  const std::string root_guid = RootGuid();
  const shim::Account created =
      CreateAccount(MutationId(1), root_guid, "Checking");

  shim::UpdateAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(2));
  shim::Account* spec = request.mutable_account();
  spec->set_guid(created.guid());
  spec->set_parent_guid(root_guid);
  spec->set_name("Checking2");
  spec->set_type(shim::ACCOUNT_TYPE_ASSET);
  spec->mutable_commodity()->set_space("CURRENCY");
  spec->mutable_commodity()->set_mnemonic("USD");
  spec->set_code("1000");
  spec->set_description("Primary checking account");
  spec->set_placeholder(true);

  grpc::ClientContext context;
  shim::Account response;
  ASSERT_TRUE(stub_->UpdateAccount(&context, request, &response).ok());
  EXPECT_EQ(response.name(), "Checking2");
  EXPECT_EQ(response.code(), "1000");
  EXPECT_EQ(response.description(), "Primary checking account");
  EXPECT_TRUE(response.placeholder());

  const shim::Account reread = GetAccount(created.guid());
  EXPECT_EQ(reread.name(), "Checking2");
  EXPECT_EQ(reread.code(), "1000");
  EXPECT_EQ(reread.description(), "Primary checking account");
  EXPECT_TRUE(reread.placeholder());
}

TEST_F(AccountServiceTest, UpdateAccountReparentsAndRejectsCycle) {
  const std::string root_guid = RootGuid();
  const shim::Account a = CreateAccount(MutationId(1), root_guid, "A");
  const shim::Account b = CreateAccount(MutationId(2), root_guid, "B");

  // Reparent A under B: root -> B -> A.
  {
    shim::UpdateAccountRequest request;
    request.mutable_meta()->set_mutation_id(MutationId(3));
    shim::Account* spec = request.mutable_account();
    spec->set_guid(a.guid());
    spec->set_parent_guid(b.guid());
    spec->set_name("A");
    spec->set_type(shim::ACCOUNT_TYPE_ASSET);
    spec->mutable_commodity()->set_space("CURRENCY");
    spec->mutable_commodity()->set_mnemonic("USD");
    grpc::ClientContext context;
    shim::Account response;
    ASSERT_TRUE(stub_->UpdateAccount(&context, request, &response).ok());
    EXPECT_EQ(response.parent_guid(), b.guid());
  }
  EXPECT_EQ(GetAccount(a.guid()).parent_guid(), b.guid());

  // B's parent is root, A's parent is B. Reparenting B under A would make A
  // both an ancestor and (transitively, after the move) a descendant of B:
  // a cycle the shim must reject before touching the tree.
  {
    shim::UpdateAccountRequest request;
    request.mutable_meta()->set_mutation_id(MutationId(4));
    shim::Account* spec = request.mutable_account();
    spec->set_guid(b.guid());
    spec->set_parent_guid(a.guid());
    spec->set_name("B");
    spec->set_type(shim::ACCOUNT_TYPE_ASSET);
    spec->mutable_commodity()->set_space("CURRENCY");
    spec->mutable_commodity()->set_mnemonic("USD");
    grpc::ClientContext context;
    shim::Account response;
    const grpc::Status status =
        stub_->UpdateAccount(&context, request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
}

TEST_F(AccountServiceTest, UpdateAccountRejectsRoot) {
  const std::string root_guid = RootGuid();
  shim::UpdateAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(1));
  shim::Account* spec = request.mutable_account();
  spec->set_guid(root_guid);
  spec->set_name("NewRootName");
  grpc::ClientContext context;
  shim::Account response;
  const grpc::Status status =
      stub_->UpdateAccount(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(AccountServiceTest, DeleteAccountRemovesLeaf) {
  const std::string root_guid = RootGuid();
  const shim::Account leaf = CreateAccount(MutationId(1), root_guid, "Leaf");
  ASSERT_EQ(ListAccountsCount(), 2);

  shim::DeleteAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(2));
  request.set_guid(leaf.guid());
  grpc::ClientContext context;
  shim::Empty response;
  ASSERT_TRUE(stub_->DeleteAccount(&context, request, &response).ok());
  EXPECT_EQ(ListAccountsCount(), 1);
}

TEST_F(AccountServiceTest, DeleteAccountRejectsNonEmptyParent) {
  const std::string root_guid = RootGuid();
  const shim::Account parent =
      CreateAccount(MutationId(1), root_guid, "Parent");
  CreateAccount(MutationId(2), parent.guid(), "Child");

  shim::DeleteAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(3));
  request.set_guid(parent.guid());
  grpc::ClientContext context;
  shim::Empty response;
  const grpc::Status status =
      stub_->DeleteAccount(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  const shim::ErrorDetail detail = UnpackDetail(status);
  EXPECT_EQ(detail.code(), shim::ERROR_CODE_ACCOUNT_NOT_EMPTY);
}

TEST_F(AccountServiceTest, DeleteAccountRejectsNonexistent) {
  shim::DeleteAccountRequest request;
  request.mutable_meta()->set_mutation_id(MutationId(1));
  request.set_guid(kNonexistentGuid);
  grpc::ClientContext context;
  shim::Empty response;
  const grpc::Status status =
      stub_->DeleteAccount(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(AccountServiceTest, ReconcileInfoRoundTripsCalendarDate) {
  const std::string root_guid = RootGuid();
  const shim::Account account =
      CreateAccount(MutationId(1), root_guid, "Checking");

  {
    grpc::ClientContext context;
    shim::AccountRef request;
    request.set_guid(account.guid());
    shim::ReconcileInfo response;
    ASSERT_TRUE(stub_->GetReconcileInfo(&context, request, &response).ok());
    EXPECT_FALSE(response.has_last_date());
  }

  {
    shim::SetReconcileInfoRequest request;
    request.mutable_meta()->set_mutation_id(MutationId(2));
    request.set_account_guid(account.guid());
    shim::Date* date = request.mutable_last_date();
    date->set_year(2026);
    date->set_month(7);
    date->set_day(15);
    grpc::ClientContext context;
    shim::ReconcileInfo response;
    ASSERT_TRUE(stub_->SetReconcileInfo(&context, request, &response).ok());
    ASSERT_TRUE(response.has_last_date());
    EXPECT_EQ(response.last_date().year(), 2026);
    EXPECT_EQ(response.last_date().month(), 7);
    EXPECT_EQ(response.last_date().day(), 15);
  }

  {
    // Re-fetch independently: the timezone-bug canary. If the stored
    // timestamp were not timezone-neutral, converting it back to a calendar
    // date could land on the 14th or 16th depending on the daemon's TZ.
    grpc::ClientContext context;
    shim::AccountRef request;
    request.set_guid(account.guid());
    shim::ReconcileInfo response;
    ASSERT_TRUE(stub_->GetReconcileInfo(&context, request, &response).ok());
    ASSERT_TRUE(response.has_last_date());
    EXPECT_EQ(response.last_date().year(), 2026);
    EXPECT_EQ(response.last_date().month(), 7);
    EXPECT_EQ(response.last_date().day(), 15);
    EXPECT_EQ(response.reconciled_balance().num(), 0);
  }
}

}  // namespace
}  // namespace daichod::testing
