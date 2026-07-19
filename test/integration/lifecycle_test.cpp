#include <chrono>
#include <filesystem>
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

}  // namespace
}  // namespace daichod::testing
