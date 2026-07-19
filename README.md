# daichod

The engine boundary of Daicho. One process, one book, one engine thread — deliberately too small to be wrong. GPL-3.0-or-later; the protobuf contract in `proto/` is Apache-2.0.

A small C++ daemon that links libgnucash and serves a gRPC contract over a Unix domain socket. It is the only process that ever opens the book; all policy lives upstairs in daicho-api. A single engine thread executes all requests in order — serialization is the concurrency model. Mutations are journaled and idempotent; after a crash, unresolved ones are reported, never silently replayed.

- [DESIGN.md](DESIGN.md) — process model, mutation protocol, supervision, test posture.
- [CONTRACT.md](CONTRACT.md) — the decisions behind [`proto/shim.proto`](proto/shim.proto) (`daicho.shim.v1`) and its versioning rules.

**Status:** under implementation, following the design documents.

## Building

The only supported build environment is a container (no distro ships libgnucash
headers; the image compiles the pinned GnuCash from source). The `Containerfile`
is engine-agnostic — use wslc, docker, or podman interchangeably:

```sh
# Development image (toolchain + GnuCash under /opt/gnucash):
<engine> build --target dev -t daichod-dev .

# Configure, build, and test with the repo bind-mounted:
<engine> run --rm -v .:/src daichod-dev cmake -S /src -B /src/build -G Ninja
<engine> run --rm -v .:/src daichod-dev cmake --build /src/build
<engine> run --rm -v .:/src daichod-dev ctest --test-dir /src/build --output-on-failure

# Deliverable runtime image:
<engine> build --target runtime -t daichod .
```

**License:** GPL-3.0-or-later ([LICENSE](LICENSE)), matching the linked libgnucash. `proto/` is Apache-2.0 so clients can generate stubs without inheriting the GPL.
