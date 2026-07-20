# daichod

[![CI](https://github.com/kiyoya/daichod/actions/workflows/ci.yml/badge.svg)](https://github.com/kiyoya/daichod/actions/workflows/ci.yml) [![Publish](https://github.com/kiyoya/daichod/actions/workflows/publish.yml/badge.svg)](https://github.com/kiyoya/daichod/actions/workflows/publish.yml)

The engine boundary of Daicho. One process, one book, one engine thread — deliberately too small to be wrong. GPL-3.0-or-later; the protobuf contract in `proto/` is Apache-2.0.

A small C++ daemon that links libgnucash and serves a gRPC contract over a Unix domain socket. It is the only process that ever opens the book; all policy lives upstairs in daicho-api. A single engine thread executes all requests in order — serialization is the concurrency model. Mutations are journaled and idempotent; after a crash, unresolved ones are reported, never silently replayed.

- [DESIGN.md](DESIGN.md) — process model, mutation protocol, supervision, test posture.
- [CONTRACT.md](CONTRACT.md) — the decisions behind [`proto/shim.proto`](proto/shim.proto) (`daicho.shim.v1`) and its versioning rules.

**Status:** under implementation, following the design documents.

**Code style:** C++ follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html); protocol buffers follow the [Protobuf Style Guide](https://protobuf.dev/programming-guides/style/).

**Building and testing:** container-only, engine-agnostic (wslc, docker, or podman). See [BUILDING.md](BUILDING.md); coding agents should also read [AGENTS.md](AGENTS.md). A [VS Code Dev Container](.devcontainer/devcontainer.json) wrapping the same `dev` image is included.

**License:** GPL-3.0-or-later ([LICENSE](LICENSE)), matching the linked libgnucash. `proto/` is Apache-2.0 so clients can generate stubs without inheriting the GPL.
