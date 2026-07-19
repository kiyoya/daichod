# daicho-proto — Engine Boundary Contract

The protobuf contract between daichod and daicho-api. It lives in the **daichod repo** — the engine boundary's contract and implementation share one home — and daicho-api generates its stubs from a pinned, tagged release of that repo, never from HEAD. The normative schema is `shim.proto` (package `daicho.shim.v1`); this doc records the decisions it encodes.

## Design rules

**Exact money.** All amounts are `Numeric { int64 num; int64 denom }`, mirroring `gnc_numeric`. Floats and decimal strings do not appear anywhere in the schema. Rounding, where a caller needs it, is requested explicitly and performed by the engine's own conversion functions so shim and desktop agree bit-for-bit.

**Engine identity.** Entities are addressed by GnuCash GUIDs; the contract invents no parallel ID space. Commodities are `{space, mnemonic}` pairs as GnuCash defines them.

**Timezone-free posting dates.** Posting dates are calendar `Date {y,m,d}` messages, never timestamps — GnuCash posting dates carry no timezone, and encoding them as epochs is how off-by-one-day bugs are born. Entry timestamps, which are genuine instants, are epoch seconds UTC.

**Idempotent mutations.** Every mutating RPC carries `MutationMeta { mutation_id }`, a client-generated UUIDv4. Re-sending an applied ID returns the recorded outcome. This is the wire half of the system-wide double-post defense (see daicho-api.md for the client half, daichod.md for the journal half).

**Typed errors, verbatim engine text.** Failures carry `ErrorDetail { code, engine_message, context }` in gRPC status details. The code enum is closed and small (`UNBALANCED_TRANSACTION`, `ACCOUNT_NOT_FOUND`, `CURRENCY_MISMATCH`, …); the engine's own message is always preserved unmodified. The shim never interprets, only classifies.

**Full-replacement updates.** `UpdateTransaction` and `UpdateAccount` take the complete desired entity; the shim diffs and applies inside one engine edit. Partial-update field masks were rejected as a source of merge ambiguity in a financial record.

## Surface summary

Five services, twenty-two RPCs — the ceiling is intentional, and additions require justifying why the operation is mechanical engine access rather than policy:

- **SessionService** — `OpenBook`, `CloseBook`, `GetBookInfo`, `Ping`, `ListIndeterminateMutations` (crash-recovery handshake: journaled mutations with no recorded outcome).
- **AccountService** — `ListAccounts`, `GetAccount`, `CreateAccount`, `UpdateAccount`, `DeleteAccount` (fails unless empty), plus `GetReconcileInfo` / `SetReconcileInfo` for the account-slot metadata (last reconciled date and balance) that desktop GnuCash's reconcile dialog reads — keeping API and desktop reconciliation in agreement about where you left off.
- **TransactionService** — `GetTransaction`, `PostTransaction`, `UpdateTransaction`, `DeleteTransaction`, `QuerySplits` (filtered, cursor-paginated, built on the engine Query API).
- **CommodityService** — `ListCommodities`, `GetPrices`, `AddPrice`.
- **BalanceService** — `GetBalance` (as-of, children, optional price-db currency conversion), `GetTrialBalance` (per-currency split sums; zero for a healthy book — the health-check primitive).

Splits carry both `value` (transaction currency) and `quantity` (account commodity), the engine's native model, so multi-currency and investment transactions need no schema change later. Reconcile state (`n/c/y/f/v`) is a first-class split field, which is what lets the reconciliation session flip cleared→reconciled through ordinary transaction updates.

## Versioning and distribution

Breaking changes require a new package (`v2`) served alongside `v1`; additive changes (new optional fields, new RPCs) are allowed within `v1`. The shim serves `shim_version` and `engine_version` in `GetBookInfo`, and the Rust client refuses at startup on contract mismatch rather than failing mid-flight. CI in this repo runs `buf` lint and breaking-change detection; daichod and daicho-api pin a tagged release, never HEAD.
