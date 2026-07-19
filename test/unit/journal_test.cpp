#include "journal/journal.h"

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "internal.pb.h"
#include "rpc/error.h"

namespace daichod {
namespace {

// Each test gets its own temp directory (keyed on pid + test name) so
// journals from different tests never collide, mirroring
// test/integration/daemon_fixture.h.
class JournalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::path(::testing::TempDir()) /
           ("daichod-journal-test-" + std::to_string(getpid()) + "-" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir_);
    path_ = (dir_ / "journal.db").string();
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
  std::string path_;
};

TEST_F(JournalTest, OpenCreatesDbFileAndReopeningWorks) {
  { auto journal = Journal::Open(path_); }
  EXPECT_TRUE(std::filesystem::exists(path_));

  // Reopening an existing journal (rather than creating a new one) works.
  auto journal = Journal::Open(path_);
  EXPECT_TRUE(journal->ListIndeterminate().empty());
}

TEST_F(JournalTest, RecordIntentTwiceForSameIdIsNoOp) {
  auto journal = Journal::Open(path_);
  const std::string id = "11111111-1111-1111-1111-111111111111";

  journal->RecordIntent(id, "CreateAccount", "payload-v1", 1000);
  journal->RecordIntent(id, "CreateAccount", "payload-v1", 2000);

  EXPECT_EQ(journal->ListIndeterminate().size(), 1u);
}

TEST_F(JournalTest, GetOutcomeIsNulloptUntilRecorded) {
  auto journal = Journal::Open(path_);
  const std::string id = "22222222-2222-2222-2222-222222222222";
  journal->RecordIntent(id, "CreateAccount", "payload", 1000);

  EXPECT_FALSE(journal->GetOutcome(id).has_value());

  internal::Outcome outcome;
  outcome.set_rpc_name("CreateAccount");
  outcome.set_ok(true);
  outcome.set_response("response-bytes");
  journal->RecordOutcome(id, outcome, 1001);

  const std::optional<internal::Outcome> recorded = journal->GetOutcome(id);
  ASSERT_TRUE(recorded.has_value());
  EXPECT_TRUE(recorded->ok());
  EXPECT_EQ(recorded->response(), "response-bytes");
  EXPECT_EQ(recorded->rpc_name(), "CreateAccount");
}

TEST_F(JournalTest, RecordOutcomeTwiceForSameIdThrows) {
  auto journal = Journal::Open(path_);
  const std::string id = "33333333-3333-3333-3333-333333333333";
  journal->RecordIntent(id, "CreateAccount", "payload", 1000);

  internal::Outcome outcome;
  outcome.set_ok(true);
  journal->RecordOutcome(id, outcome, 1001);

  EXPECT_THROW(journal->RecordOutcome(id, outcome, 1002), ShimError);
}

TEST_F(JournalTest, ListIndeterminateListsOnlyUnresolvedIntents) {
  auto journal = Journal::Open(path_);
  const std::string resolved_id = "44444444-4444-4444-4444-444444444444";
  const std::string pending_id = "55555555-5555-5555-5555-555555555555";
  const std::string payload = "the-payload-bytes";

  journal->RecordIntent(resolved_id, "CreateAccount", payload, 1000);
  journal->RecordIntent(pending_id, "PostTransaction", payload, 1001);

  internal::Outcome outcome;
  outcome.set_ok(true);
  journal->RecordOutcome(resolved_id, outcome, 1002);

  const std::vector<Journal::IndeterminateEntry> entries =
      journal->ListIndeterminate();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].mutation_id, pending_id);
  EXPECT_EQ(entries[0].rpc_name, "PostTransaction");
  EXPECT_EQ(entries[0].payload_sha256, Sha256(payload));
}

TEST(Sha256Test, MatchesWellKnownDigestForAbc) {
  const std::string digest = Sha256("abc");
  static constexpr unsigned char kExpected[32] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

  ASSERT_EQ(digest.size(), sizeof(kExpected));
  EXPECT_EQ(std::memcmp(digest.data(), kExpected, sizeof(kExpected)), 0);
}

}  // namespace
}  // namespace daichod
