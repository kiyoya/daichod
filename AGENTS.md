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

## Version control

- Any non-trivial work happens on a descriptively named branch; commits
  land on main only with the human's identity and signature.
- Every commit on main must be signed — no exceptions. Unsigned commits
  are tolerable only on work branches (see below); getting them onto
  main signed is the human's act: agents never merge PRs, never push to
  main, and never create or push release tags (tags are annotated and
  signed with the human's key). A GitHub squash-merge performed by the
  human satisfies this — github.com signs the squash commit with its
  web-flow key. Agents stop at "PR open, CI green, squash message
  prepared in the PR body" and hand over.
- Agent-made commits keep the human as author and add the agent's
  identity as a `Co-Authored-By` footer (platform domain, e.g.
  `noreply@anthropic.com`). Never change git config; override per
  invocation with `git -c` when needed. When the human is not present
  to approve a signature, disable signing for that commit
  (`git -c commit.gpgsign=false commit`) — the human signs later
  (e.g. `git rebase --gpg-sign` over the branch).
- One topic per commit. ~100 changed lines is a comfortable size,
  ~1000 is too large; never bundle unrelated topics. Messages explain
  the WHY, use the history's style (short imperative subject ending
  with a period), and never use conventional-commit prefixes.
- After a chain of commits, ask the human for review with a clear
  summary of what changed (show `--stat` when scope could be doubted).
  Finalize per the human's choice: local squash merge onto main, a
  pushed branch for a GitHub squash-merge, or per-commit sign-off.
  Ask before pushing.
- In PR and issue descriptions, you should leave your identity. Pick an emoji to identify yourself. For example, include
  a footer like `🚀 Generated with [Agent Name]` where "[Agent Name]"
  is a link to the agent's website or documentation (e.g., Antigravity).

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
