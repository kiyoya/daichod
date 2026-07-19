#pragma once

#include <cstddef>

#include "journal/journal.h"

extern "C" {
typedef struct QofBook QofBook;
}

namespace daichod {

// Startup reconciliation (DESIGN.md, mutation protocol): for every intent
// that reached the pending record but has no outcome, decide from the book
// whether the engine commit happened. Applied → record the pending response
// as the outcome; not applied → drop the pending record so the entry is
// reported plainly indeterminate (guaranteed unapplied, safe to re-issue).
// The check is exact because the engine thread is serial: an unresolved
// mutation was the last engine action before the crash. Runs on the engine
// thread after the book opens, before serving. Returns the number of
// entries resolved as applied.
std::size_t ReconcilePendingMutations(Journal* journal, QofBook* book);

}  // namespace daichod
