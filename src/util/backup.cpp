#include "util/backup.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <vector>

#include "rpc/error.h"

namespace daichod {

namespace fs = std::filesystem;

namespace {

std::string TodayStamp() {
  const std::time_t now = std::time(nullptr);
  std::tm parts{};
  localtime_r(&now, &parts);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
  return buffer;
}

bool LooksLikeStamp(const std::string& name) {
  // Exact-length check also rejects "<stamp>.tmp": an in-progress snapshot
  // is never mistaken for a completed one, by this or the prune loop below.
  return name.size() == 10 && name[4] == '-' && name[7] == '-';
}

// SQLite in WAL mode keeps the most recently committed rows in a "-wal"
// sibling until the next checkpoint, not in `path` itself; a "-shm" index
// or a "-journal" rollback log can also be sitting next to it mid-write.
// Copying just `path` after a crash can silently drop committed data, so
// every sidecar that happens to exist is copied alongside it.
void CopyWithSidecars(const std::string& path, const fs::path& dest_dir) {
  static constexpr const char* kSuffixes[] = {"", "-wal", "-shm", "-journal"};
  for (const char* suffix : kSuffixes) {
    const fs::path source = path + suffix;
    std::error_code ec;
    if (fs::exists(source, ec)) {
      fs::copy_file(source, dest_dir / source.filename());
    }
  }
}

}  // namespace

bool SnapshotBeforeOpen(const std::string& book_path,
                        const std::string& journal_path,
                        const std::string& backup_dir, int keep_days) {
  const fs::path root(backup_dir);
  const std::string stamp = TodayStamp();
  const fs::path today = root / stamp;
  std::error_code ec;
  if (fs::exists(today, ec)) return false;

  // Assemble the snapshot in a ".tmp" sibling and rename it into place only
  // once every copy has succeeded. Without this, a copy throwing partway
  // through (e.g. book copied, journal copy fails) leaves a half-written
  // `today` directory behind, and the exists() check above would then treat
  // that partial backup as complete forever. Remove any ".tmp" left by
  // exactly that failure before starting, so this retry begins clean.
  const fs::path tmp = root / (stamp + ".tmp");
  fs::remove_all(tmp, ec);

  try {
    fs::create_directories(tmp);
    CopyWithSidecars(book_path, tmp);
    CopyWithSidecars(journal_path, tmp);
    fs::rename(tmp, today);
  } catch (const fs::filesystem_error& e) {
    throw ShimError(daicho::shim::v1::ERROR_CODE_ENGINE_ERROR,
                    std::string("backup snapshot failed: ") + e.what(),
                    backup_dir);
  }

  // Prune oldest snapshot directories beyond keep_days. Best effort — a
  // failed prune must not block serving.
  std::vector<fs::path> stamps;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (entry.is_directory(ec) &&
        LooksLikeStamp(entry.path().filename().string())) {
      stamps.push_back(entry.path());
    }
  }
  std::sort(stamps.begin(), stamps.end());
  while (stamps.size() > static_cast<size_t>(keep_days)) {
    fs::remove_all(stamps.front(), ec);
    stamps.erase(stamps.begin());
  }
  return true;
}

}  // namespace daichod
