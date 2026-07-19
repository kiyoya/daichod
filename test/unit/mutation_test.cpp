#include "rpc/mutation.h"

#include <unistd.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/worker.h"
#include "journal/journal.h"
#include "rpc/error.h"
#include "shim.pb.h"

namespace daichod {
namespace {

namespace shim = daicho::shim::v1;

// A real EngineWorker plus a real Journal in a fresh temp dir per test —
// RunMutation's protocol (journal + apply + journal) is exercised end to
// end, only the engine work itself (apply) is faked.
class MutationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::path(::testing::TempDir()) /
           ("daichod-mutation-test-" + std::to_string(getpid()) + "-" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir_);
    worker_ = std::make_unique<EngineWorker>(8);
    journal_ = Journal::Open((dir_ / "journal.db").string());
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
  std::unique_ptr<EngineWorker> worker_;
  std::unique_ptr<Journal> journal_;
};

TEST_F(MutationTest, SuccessCallsApplyOnceAndRecordsOutcome) {
  shim::MutationMeta meta;
  meta.set_mutation_id("11111111-1111-1111-1111-111111111111");
  shim::CreateAccountRequest request;
  *request.mutable_meta() = meta;
  request.mutable_account()->set_name("Checking");

  int apply_calls = 0;
  shim::Account response;
  const grpc::Status status = RunMutation(
      worker_.get(), journal_.get(), meta, request, "CreateAccount",
      &response, [&](shim::Account* out) {
        ++apply_calls;
        out->set_guid("abc-guid");
        out->set_name("Checking");
      });

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(apply_calls, 1);
  EXPECT_EQ(response.guid(), "abc-guid");

  const std::optional<internal::Outcome> recorded =
      journal_->GetOutcome(meta.mutation_id());
  ASSERT_TRUE(recorded.has_value());
  EXPECT_TRUE(recorded->ok());
  EXPECT_EQ(recorded->rpc_name(), "CreateAccount");
  shim::Account replayed;
  ASSERT_TRUE(replayed.ParseFromString(recorded->response()));
  EXPECT_EQ(replayed.guid(), "abc-guid");
}

TEST_F(MutationTest, IdempotentReplayReturnsSameResponseWithoutCallingApply) {
  shim::MutationMeta meta;
  meta.set_mutation_id("22222222-2222-2222-2222-222222222222");
  shim::CreateAccountRequest request;
  *request.mutable_meta() = meta;
  request.mutable_account()->set_name("Savings");

  int apply_calls = 0;
  auto apply = [&](shim::Account* out) {
    ++apply_calls;
    out->set_guid("guid-1");
    out->set_name("Savings");
  };

  shim::Account first_response;
  ASSERT_TRUE(RunMutation(worker_.get(), journal_.get(), meta, request,
                          "CreateAccount", &first_response, apply)
                  .ok());
  ASSERT_EQ(apply_calls, 1);

  shim::Account second_response;
  const grpc::Status status =
      RunMutation(worker_.get(), journal_.get(), meta, request,
                 "CreateAccount", &second_response, apply);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(apply_calls, 1);
  EXPECT_EQ(second_response.guid(), first_response.guid());
  EXPECT_EQ(second_response.name(), first_response.name());
}

TEST_F(MutationTest, RecordedFailureReplaysWithoutCallingApplyAgain) {
  shim::MutationMeta meta;
  meta.set_mutation_id("33333333-3333-3333-3333-333333333333");
  shim::PostTransactionRequest request;
  *request.mutable_meta() = meta;

  int apply_calls = 0;
  auto apply = [&](shim::Transaction*) {
    ++apply_calls;
    throw ShimError(shim::UNBALANCED_TRANSACTION, "splits do not sum to zero");
  };

  shim::Transaction first_response;
  grpc::Status status =
      RunMutation(worker_.get(), journal_.get(), meta, request,
                 "PostTransaction", &first_response, apply);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(apply_calls, 1);

  shim::Transaction second_response;
  status = RunMutation(worker_.get(), journal_.get(), meta, request,
                       "PostTransaction", &second_response, apply);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(apply_calls, 1);
}

TEST_F(MutationTest, InvalidMutationIdIsRejectedAndNeverJournaled) {
  shim::MutationMeta meta;
  meta.set_mutation_id("not-a-uuid");
  shim::CreateAccountRequest request;
  *request.mutable_meta() = meta;

  int apply_calls = 0;
  shim::Account response;
  const grpc::Status status = RunMutation(
      worker_.get(), journal_.get(), meta, request, "CreateAccount",
      &response, [&](shim::Account*) { ++apply_calls; });

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(apply_calls, 0);
  EXPECT_TRUE(journal_->ListIndeterminate().empty());
}

TEST_F(MutationTest, NonShimErrorFromApplyLeavesEntryIndeterminate) {
  shim::MutationMeta meta;
  meta.set_mutation_id("44444444-4444-4444-4444-444444444444");
  shim::CreateAccountRequest request;
  *request.mutable_meta() = meta;

  shim::Account response;
  const grpc::Status status = RunMutation(
      worker_.get(), journal_.get(), meta, request, "CreateAccount",
      &response,
      [&](shim::Account*) -> void {
        throw std::runtime_error("engine exploded");
      });

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);

  const std::vector<Journal::IndeterminateEntry> entries =
      journal_->ListIndeterminate();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].mutation_id, meta.mutation_id());
}

}  // namespace
}  // namespace daichod
