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
  char buffer[11];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);
  return buffer;
}

bool LooksLikeStamp(const std::string& name) {
  return name.size() == 10 && name[4] == '-' && name[7] == '-';
}

}  // namespace

bool SnapshotBeforeOpen(const std::string& book_path,
                        const std::string& journal_path,
                        const std::string& backup_dir, int keep_days) {
  const fs::path root(backup_dir);
  const fs::path today = root / TodayStamp();
  std::error_code ec;
  if (fs::exists(today, ec)) return false;

  try {
    fs::create_directories(today);
    if (fs::exists(book_path)) {
      fs::copy_file(book_path, today / fs::path(book_path).filename());
    }
    if (fs::exists(journal_path)) {
      fs::copy_file(journal_path, today / fs::path(journal_path).filename());
    }
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
