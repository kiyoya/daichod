#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "daemon_fixture.h"

// --backup-dir lifecycle: a daily snapshot before the book (and its sidecar
// journal) is ever opened, taken once per day regardless of how many times
// the daemon restarts within that day (util/backup.{h,cpp}, main.cpp).

namespace daichod::testing {
namespace {

class LifecycleTest : public DaemonFixture {
 protected:
  void SetUp() override {
    DaemonFixture::SetUp();
    backup_dir_ = dir_ / "backups";
  }

  // Every dated snapshot directory currently under backup_dir_.
  std::vector<std::filesystem::path> DatedBackupDirs() {
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    if (!std::filesystem::exists(backup_dir_, ec)) return dirs;
    for (const auto& entry :
        std::filesystem::directory_iterator(backup_dir_, ec)) {
      if (entry.is_directory()) dirs.push_back(entry.path());
    }
    return dirs;
  }

  // Matches main.cpp's default: a sidecar next to the book, not a sibling
  // path — plain string concatenation, no separator.
  std::filesystem::path JournalPath() {
    return book_path_.string() + ".daichod-journal";
  }

  // Matches TodayStamp() in backup.cpp: localtime YYYY-MM-DD.
  static std::string TodayStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
    localtime_r(&now, &parts);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                 parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
    return buffer;
  }

  static void WriteFile(const std::filesystem::path& path,
                        const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
  }

  std::filesystem::path backup_dir_;
};

TEST_F(LifecycleTest, BackupDirSnapshotsOnceAndLeavesItUntouchedOnRestart) {
  StopDaemon();
  StartDaemon(daichod_bin_, /*extra_env=*/{},
             {"--backup-dir", backup_dir_.string()});
  Connect();

  const std::vector<std::filesystem::path> dirs_after_first_start =
      DatedBackupDirs();
  ASSERT_EQ(dirs_after_first_start.size(), 1u)
      << "expected exactly one dated snapshot directory";
  const std::filesystem::path snapshot_dir = dirs_after_first_start[0];
  const std::filesystem::path snapshotted_book =
      snapshot_dir / book_path_.filename();
  ASSERT_TRUE(std::filesystem::exists(snapshotted_book))
      << "snapshot directory is missing the book file copy";

  const auto snapshot_size_before =
      std::filesystem::file_size(snapshotted_book);
  const auto snapshot_mtime_before =
      std::filesystem::last_write_time(snapshotted_book);

  // Restart with the same args, same day: SnapshotBeforeOpen only acts when
  // today's directory does not exist yet, so this must be a no-op.
  StopDaemon();
  StartDaemon(daichod_bin_, /*extra_env=*/{},
             {"--backup-dir", backup_dir_.string()});
  Connect();

  const std::vector<std::filesystem::path> dirs_after_restart =
      DatedBackupDirs();
  ASSERT_EQ(dirs_after_restart.size(), 1u)
      << "restart within the same day must not create a second snapshot";
  EXPECT_EQ(dirs_after_restart[0], snapshot_dir);
  ASSERT_TRUE(std::filesystem::exists(snapshotted_book));
  EXPECT_EQ(std::filesystem::file_size(snapshotted_book),
           snapshot_size_before);
  EXPECT_EQ(std::filesystem::last_write_time(snapshotted_book),
           snapshot_mtime_before);
}

TEST_F(LifecycleTest, SnapshotIncludesSidecarFiles) {
  StopDaemon();

  // Bad-magic rollback journals: SQLite ignores (and removes) a "-journal"
  // file that doesn't start with its magic number, so the daemon still
  // starts cleanly — but SnapshotBeforeOpen runs before SQLite ever gets a
  // chance to do that, and must have already copied them.
  const std::filesystem::path book_journal(book_path_.string() + "-journal");
  const std::filesystem::path journal_journal(JournalPath().string() +
                                               "-journal");
  WriteFile(book_journal, "garbage");
  WriteFile(journal_journal, "garbage");

  StartDaemon(daichod_bin_, /*extra_env=*/{},
             {"--backup-dir", backup_dir_.string()});
  Connect();

  const std::vector<std::filesystem::path> dirs = DatedBackupDirs();
  ASSERT_EQ(dirs.size(), 1u)
      << "expected exactly one dated snapshot directory";
  const std::filesystem::path snapshot_dir = dirs[0];
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir / book_journal.filename()))
      << "snapshot is missing the book's -journal sidecar";
  EXPECT_TRUE(
      std::filesystem::exists(snapshot_dir / journal_journal.filename()))
      << "snapshot is missing the journal's -journal sidecar";
}

TEST_F(LifecycleTest, StaleTmpSnapshotIsCleanedAndRetried) {
  StopDaemon();

  // Simulates a prior run that copied the book but threw partway through
  // the journal copy: a ".tmp" directory left behind, no dated directory.
  const std::filesystem::path tmp_dir = backup_dir_ / (TodayStamp() + ".tmp");
  std::filesystem::create_directories(tmp_dir);
  WriteFile(tmp_dir / "junk", "leftover from a failed attempt");

  StartDaemon(daichod_bin_, /*extra_env=*/{},
             {"--backup-dir", backup_dir_.string()});
  Connect();

  const std::filesystem::path snapshot_dir = backup_dir_ / TodayStamp();
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir))
      << "a stale .tmp must not block today's snapshot from being retried";
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir / book_path_.filename()));
  EXPECT_FALSE(std::filesystem::exists(tmp_dir))
      << "the stale .tmp directory should have been removed";
}

}  // namespace
}  // namespace daichod::testing
