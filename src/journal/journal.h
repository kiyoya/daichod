#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "internal.pb.h"

struct sqlite3;
struct sqlite3_stmt;

namespace daichod {

// The sidecar intent journal: SQLite next to the book, WAL,
// synchronous=FULL so every step of the mutation protocol is on disk before
// the daemon proceeds. Engine-thread only — the mutation protocol executes
// entirely on the engine thread, so the journal needs no locking.
class Journal {
 public:
  struct IndeterminateEntry {
    std::string mutation_id;
    std::string rpc_name;
    std::string payload_sha256;  // raw 32 bytes
    int64_t journaled_at_utc = 0;
  };

  // Opens (creating if absent) the journal database. Throws ShimError on
  // any SQLite failure — an unusable journal is fatal, mutations must never
  // proceed unjournaled.
  static std::unique_ptr<Journal> Open(const std::string& path);
  ~Journal();

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Step 1: durably records the intent. A row that already exists for this
  // mutation_id is left untouched (the retry of an indeterminate mutation).
  void RecordIntent(const std::string& mutation_id, const std::string& rpc_name,
                    const std::string& payload, int64_t now_utc);

  // Step 2: the recorded outcome of a previously applied mutation, if any.
  std::optional<internal::Outcome> GetOutcome(const std::string& mutation_id);

  // Step 3a: durably records the would-be response immediately before the
  // engine commit. Closes the applied-but-unrecorded crash window: startup
  // reconciliation compares this against the book to decide the mutation's
  // fate exactly.
  void RecordPending(const std::string& mutation_id,
                     const std::string& response_bytes);

  struct PendingEntry {
    std::string mutation_id;
    std::string rpc_name;
    std::string request_payload;
    std::string pending_response;
  };
  // Intents that reached step 3a but have no recorded outcome — the inputs
  // to startup reconciliation.
  std::vector<PendingEntry> ListPendingUnresolved();

  // Reconciliation verdict "not applied": drop the pending record so the
  // entry is reported plainly indeterminate.
  void ClearPending(const std::string& mutation_id);

  // Step 4: durably records the outcome. A mutation_id may be resolved only
  // once; recording twice indicates a protocol bug and throws.
  void RecordOutcome(const std::string& mutation_id,
                     const internal::Outcome& outcome, int64_t now_utc);

  // Journaled intents with no recorded outcome — the crash-recovery
  // handshake. Never replayed by the shim; daicho-api decides.
  std::vector<IndeterminateEntry> ListIndeterminate();

 private:
  explicit Journal(sqlite3* db) : db_(db) {}

  sqlite3* db_;
};

// SHA-256 of a payload as raw bytes, for IndeterminateMutations.
std::string Sha256(const std::string& data);

}  // namespace daichod
