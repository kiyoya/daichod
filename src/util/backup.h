#pragma once

#include <string>

namespace daichod {

// Before the first open of each day, snapshots the book and its sidecar
// journal into backup_dir/YYYY-MM-DD/ and prunes snapshot directories
// beyond keep_days. A snapshot is taken only when today's directory does
// not exist yet, so restarts within a day are free. Only meaningful for
// file-backed (sqlite3) books; callers skip it otherwise. Returns true if
// a snapshot was taken. Throws ShimError on copy failure — refusing to
// serve without the safety net is the design's call, not a policy choice.
bool SnapshotBeforeOpen(const std::string& book_path,
                        const std::string& journal_path,
                        const std::string& backup_dir, int keep_days);

}  // namespace daichod
