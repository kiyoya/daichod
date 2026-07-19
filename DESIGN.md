# daichod — libgnucash Engine Shim Daemon

A small C++ daemon that links libgnucash directly and serves the `daicho-proto` contract over a Unix domain socket. It is the only process that ever opens the book. Its reliability argument is that it is too small and too boring to be wrong: mechanical engine access, zero policy.

**Non-goals:** authentication, authorization, validation the engine already performs, reporting, categorization, multi-book routing. All policy lives in daicho-api. Target size is a few thousand lines; growth beyond that means logic is leaking in that belongs upstairs.

## Process and concurrency model

The engine is not thread-safe and assumes a single writer; the shim embraces this. One process owns exactly one book. gRPC I/O threads decode requests onto a bounded FIFO queue; a single dedicated **engine thread** executes them strictly in order — including reads — and returns results via futures. There is no engine-side locking because there is no engine-side concurrency. Serialization is the concurrency model.

Exclusivity is keyed to book identity, not to `--socket`: a `flock` plus PID file on `<book>.daichod.lock` for sqlite3 books, or on a hashed-URI path next to the journal for postgres books (credentials stripped before hashing), so a second instance pointed at the same book fails loudly at startup regardless of which socket it was given. The socket-path flock/PID file is kept alongside it as a same-socket guard. Underneath both sits the engine's own book lock; daichod breaks it only when provably stale — same hostname, dead PID — for sqlite3 books, so a crash is a restart and desktop GnuCash's live lock is never stolen. postgres books retain an unconditional break of the engine lock as a documented residual risk (checking a postgres lock row needs libpq, which isn't linked).

## Mutation protocol

Every mutating RPC carries a client UUID. Lifecycle:

1. Append the full request to an intent journal (sidecar SQLite next to the book); fsync.
2. Consult the applied-mutations table; a previously applied ID returns its recorded outcome untouched.
3. Execute on the engine thread inside `xaccTransBeginEdit`/`CommitEdit` (or account equivalent); any engine error rolls back the edit — partial application is impossible. Immediately before the engine commit, journal the would-be response (engine GUIDs are assigned at allocation, so it is complete) as a *pending* record; fsync.
4. Record outcome (success + GUIDs, or typed failure); fsync; respond.

The pending record closes the gap between the engine's commit and the journal's outcome — the one window where "applied" and "recorded" can disagree. On startup, each journaled intent with a pending record but no outcome is reconciled against the book: if the book shows the mutation's effect (the created GUID exists, the entity matches the pending response, the deleted entity is gone), the outcome is recorded as applied; otherwise the pending record is dropped. No later mutation can have intervened — the engine thread is serial and the indeterminate mutation was the last thing before the crash — so the check is exact.

After reconciliation, journal entries with no recorded outcome are reported via `ListIndeterminateMutations` — never silently replayed. daicho-api resolves them by re-issuing with the same ID, which step 2 (plus reconciliation) makes safe: a reported-indeterminate mutation is guaranteed unapplied. The book must use the SQLite/Postgres backend so engine commits persist incrementally; XML-backed books are refused rather than pretending whole-file saves are crash-safe.

## Reads, errors, reconcile metadata

Reads run on the same engine thread — fast, always consistent with writes; caching is daicho-api's job. Errors are the closed `ErrorCode` enum plus the engine's verbatim message; the shim classifies but never interprets, so policy disagreements with the engine get resolved in exactly one place (daicho-api). `GetReconcileInfo`/`SetReconcileInfo` read and write the account slots (last-reconciled date and balance) that the desktop reconcile dialog uses, so API-driven reconciliation and desktop GnuCash never disagree about where reconciliation left off.

## Startup, shutdown, supervision

Startup: acquire locks → open sidecar journal → open book → run engine scrub/check routines → assert trial balance is zero in every currency → report indeterminate mutations → serve. Any failure exits nonzero with a machine-readable reason. SIGTERM drains the queue, closes the book cleanly, releases locks. If the final save fails, teardown still completes and locks are still released, but the daemon reports the failure (stderr JSON) and exits nonzero. The shim runs as a supervised child of daicho-api with restart-on-failure; because all state is the book plus the journal, a crash is a restart plus the indeterminate handshake, never a recovery procedure. Before the first open of each day the book and sidecar are snapshotted to a rotating backup directory.

## Build and compatibility

CMake, linking the distribution's libgnucash development packages pinned to an exact GnuCash version compiled into the binary and served via `GetBookInfo`. The supported deployment is a container image whose base carries the matching GnuCash build, so shim and engine cannot skew. GnuCash upgrades are deliberate events: bump the pin, rebuild, rerun the full compatibility suite.

## Test posture — the heart of the correctness story

Golden-book round-trips: scripted mutations through the shim, then the same book opened by the pinned desktop `gnucash-cli` and checked for scrub warnings, trial-balance zero, and expected report values. Property tests generate random balanced transactions and assert engine acceptance plus invariants. Crash tests kill the daemon at each mutation-protocol step and assert the handshake converges without loss or duplication. All of it runs on every change and on every version bump.

## Code layout

```
daichod/
  src/main.cpp              # locks, startup checks, lifecycle
  src/rpc/                  # gRPC service impls: decode, enqueue, await
  src/engine/worker.{h,cpp} # engine thread + bounded queue
  src/engine/session.{h,cpp}# book open/close/scrub wrapper
  src/engine/map.{h,cpp}    # proto <-> engine mapping, GUID handling
  src/journal/              # sidecar: intent journal + applied mutations
  test/golden/              # round-trip books + gnucash-cli harness
```

Depends on a tagged `daicho-proto` release for generated stubs.
