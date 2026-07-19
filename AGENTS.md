# Agent notes for daichod

Read [BUILDING.md](BUILDING.md) first: container-only builds, canonical
build/test commands, test-suite map, and the wslc preview quirks and
their workarounds. Nothing builds on the host, ever.

## Conventions

- C++: [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
- Protos: [Protobuf Style Guide](https://protobuf.dev/programming-guides/style/);
  edition 2024 with explicit field presence; enum values carry their
  enum-name prefix. `proto/google/rpc/status.proto` is vendored verbatim —
  never restyle it. `buf lint`/`buf breaking` (config in `proto/buf.yaml`)
  guard the contract in CI; wire numbers never change meaning.
- Design authority: [DESIGN.md](DESIGN.md) and [CONTRACT.md](CONTRACT.md).
  The shim is mechanical engine access — if a change adds policy
  (validation the engine doesn't do, interpretation, defaults with
  business meaning), it belongs in daicho-api, not here.

## Invariants to never break

- The engine is not thread-safe: every libgnucash call belongs on the
  engine thread (`EngineWorker::Run`). There is no engine-side locking
  because there is no engine-side concurrency.
- Mutating RPCs go through `RunMutation` — journal intent, replay
  recorded outcomes, record pending immediately before the engine
  commit, record the outcome after. Never bypass the journal; never
  record non-deterministic failures as outcomes.
- Engine errors cross the boundary verbatim inside `ErrorDetail`; the
  shim classifies, it never rewrites messages.
- Full test suite green (`ctest`, see BUILDING.md) before any commit
  that touches `src/`, `proto/`, or `test/`.
