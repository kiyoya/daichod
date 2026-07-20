# Building and testing daichod

Everything here is engine-agnostic: the same `Containerfile` works with
any OCI-compatible engine (docker, podman, …). There is no host toolchain and none should be
installed — no distro ships libgnucash headers, and the contract's
protobuf edition 2024 is newer than any distro gRPC/protobuf. Both stacks
are compiled from source inside the image (pins: `GNUCASH_VERSION`,
`GRPC_VERSION` at the top of the `Containerfile`).

## Images

```sh
<engine> build --target dev     -t daichod-dev .   # toolchain + GnuCash + gRPC
<engine> build --target runtime -t daichod .       # deliverable daemon image
```

The `gnucash` and `grpc` stages take ~40 and ~20 minutes cold; they are
layer-cached and only rebuild when their pins change.

The runtime image ships `daichod-mkbook` alongside the daemon: the daemon
only opens existing books, so deployment tooling creates the initial book
with

```sh
<engine> run --rm -v /path/to/data:/data --entrypoint daichod-mkbook daichod \
  sqlite3:///data/book.gnucash
```

which prints the new book's root account GUID.

## Build and test

Sources are bind-mounted read-mostly at `/src`; the build tree and the
compiler cache live on named volumes (`/build`, `/ccache`) — do not move
them onto the bind mount (see the bind-mount limitations below for why).

```sh
<engine> volume create daichod-build daichod-ccache   # once
<engine> run --rm -v .:/src -v daichod-build:/build -v daichod-ccache:/ccache daichod-dev \
  cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug
<engine> run --rm -v .:/src -v daichod-build:/build -v daichod-ccache:/ccache daichod-dev \
  cmake --build /build
<engine> run --rm -v .:/src -v daichod-build:/build daichod-dev \
  ctest --test-dir /build/test -j "$(nproc)" --output-on-failure
```

- Always pass `-j` to ctest: every test is isolated (own temp dir,
  socket, book); the full suite is ~11s parallel vs ~275s serial.
- ccache makes from-scratch rebuilds essentially free (~3s at 100%
  hits vs ~3m20s cold); it warms automatically, nothing to manage.
- Proto stubs regenerate automatically; protoc and grpc_cpp_plugin come
  from `/opt/grpc` inside the image, never from the distro.

## Running the daemon locally

```sh
<engine> run --rm -v .:/src -v daichod-build:/build daichod-dev bash -c \
  '/build/daichod-mkbook sqlite3:///tmp/book.gnucash \
   && /build/daichod --book-uri sqlite3:///tmp/book.gnucash \
        --socket /tmp/daichod.sock'
```

`daichod --version` prints the shim and pinned engine versions.

## Test suites

| Binary | What it proves |
|---|---|
| `daichod_unit_tests` | worker ordering/backpressure, error contract, mapping, journal, mutation protocol |
| `daichod_integration_tests` / `daichod_account_tests` / `daichod_transaction_tests` | every RPC end-to-end over the Unix socket |
| `daichod_crash_tests` | kill matrix over `DAICHOD_CRASH_AT` points; recovery converges without loss or duplication |
| `daichod_lifecycle_tests` | daily snapshot behavior across restarts |
| `daichod_golden_tests` | the pinned `gnucash-cli` opens shim-written books and agrees |
| `daichod_property_tests` | seeded random balanced transactions hold invariants |

## Bind-mount limitations

Bind-mount fidelity varies by engine and host OS: on several engines
the mount leaves POSIX gaps, which is why the layout above bind-mounts
sources only. Symptoms seen in practice:

- **A build tree or compiler cache on the mount misbehaves.** On
  affected engines, processes spawned inside freshly-created
  directories lose `getcwd()` and file locks: `sh: 0: getcwd() failed`
  noise from every ninja step, ctest's recursive test discovery finds
  nothing, and ccache errors out of every compile (falling back to the
  real compiler, so nothing caches). Named volumes (`/build`,
  `/ccache`) have none of these problems — and are faster everywhere.
- **Unix sockets may not bind on a bind mount** (`Error in bind ...
  Operation not permitted`): point `--socket` at the container
  filesystem or a named volume; only the book can live on the mount.

### VS Code Dev Container

[`.devcontainer/devcontainer.json`](.devcontainer/devcontainer.json)
wraps the same layout for "Reopen in Container": it builds the
`Containerfile`'s `dev` target and mounts exactly as above — sources
bind-mounted at `/src`, `/build` and `/ccache` on the named volumes —
so the bind-mount limitations stay worked around no matter who starts
the container.

The Dev Containers extension drives whatever engine its **Docker
Path** setting points at — a per-user VS Code setting, deliberately
not committed, so each contributor picks their own engine.

On first open, `postCreateCommand` configures the build tree (`Debug`,
with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`), so CMake Tools has a ready
tree and clangd picks up `/build/compile_commands.json` immediately.

The container name is pinned to `daichod-devcontainer`, so host-side
tooling can run commands in the running dev container
deterministically:

```sh
<engine> exec daichod-devcontainer cmake --build /build
```

Starting a stopped one with `<engine> start` from the host also works,
but bypasses VS Code's orchestration (`postCreateCommand`, port
forwarding) — fine for build/test exec, not a substitute for opening
it in VS Code.
