# daichod build images. Engine-agnostic OCI file (standard Dockerfile syntax);
# build with wslc/docker/podman interchangeably:
#
#   <engine> build --target dev     -t daichod-dev .
#   <engine> build --target runtime -t daichod .
#
# GnuCash is compiled from source because no distro ships libgnucash headers.
# The pin below is the single source of truth for the engine version; it is
# baked into the daichod binary and served via GetBookInfo.engine_version.

ARG GNUCASH_VERSION=5.16

# ---------------------------------------------------------------- gnucash
# Full GnuCash build (WITH_GNUCASH=ON is required to get gnucash-cli, which
# the golden-book test harness drives): engine libs, headers, and binaries
# installed under /opt/gnucash.
FROM ubuntu:24.04 AS gnucash
ARG GNUCASH_VERSION
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config \
      curl ca-certificates bzip2 \
      gettext xsltproc \
      guile-3.0-dev \
      libglib2.0-dev \
      libboost-date-time-dev libboost-filesystem-dev libboost-locale-dev \
      libboost-program-options-dev libboost-regex-dev libboost-system-dev \
      libxml2-dev libxslt1-dev zlib1g-dev \
      libicu-dev \
      swig \
      libdbi-dev libdbd-sqlite3 \
      libgtest-dev libgmock-dev \
      libwebkit2gtk-4.1-dev libgtk-3-dev \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL "https://github.com/Gnucash/gnucash/releases/download/${GNUCASH_VERSION}/gnucash-${GNUCASH_VERSION}.tar.bz2" \
      -o /tmp/gnucash.tar.bz2 \
    && mkdir /tmp/gnucash-src \
    && tar -xjf /tmp/gnucash.tar.bz2 -C /tmp/gnucash-src --strip-components=1 \
    && cmake -S /tmp/gnucash-src -B /tmp/gnucash-build -G Ninja \
         -DCMAKE_BUILD_TYPE=RelWithDebInfo \
         -DCMAKE_INSTALL_PREFIX=/opt/gnucash \
         -DWITH_PYTHON=OFF \
         -DWITH_AQBANKING=OFF \
         -DWITH_OFX=OFF \
    && cmake --build /tmp/gnucash-build \
    && cmake --install /tmp/gnucash-build \
    && rm -rf /tmp/gnucash.tar.bz2 /tmp/gnucash-src /tmp/gnucash-build

# -------------------------------------------------------------------- dev
# Interactive build/test environment; run with the repo bind-mounted at /src.
FROM ubuntu:24.04 AS dev
ARG GNUCASH_VERSION
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config gdb ccache git \
      libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
      libsqlite3-dev uuid-dev \
      libgtest-dev libgmock-dev \
      guile-3.0-dev \
      libglib2.0-dev \
      libboost-date-time-dev libboost-filesystem-dev libboost-locale-dev \
      libboost-program-options-dev libboost-regex-dev libboost-system-dev \
      libxml2-dev libxslt1-dev zlib1g-dev \
      libicu-dev \
      libdbi-dev libdbd-sqlite3 \
      libgtk-3-0t64 libwebkit2gtk-4.1-0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=gnucash /opt/gnucash /opt/gnucash
# gnucash-cli's reports are guile modules; a prefix install ships no
# environment file, so the load paths are set here.
ENV PATH=/opt/gnucash/bin:$PATH \
    LD_LIBRARY_PATH=/opt/gnucash/lib:/opt/gnucash/lib/gnucash \
    GUILE_LOAD_PATH=/opt/gnucash/share/guile/site/3.0 \
    GUILE_LOAD_COMPILED_PATH=/opt/gnucash/lib/x86_64-linux-gnu/guile/3.0/site-ccache \
    GNUCASH_VERSION=${GNUCASH_VERSION}
WORKDIR /src

# ------------------------------------------------------------------ build
# Non-interactive daichod compile for the runtime image.
FROM dev AS build
ARG GNUCASH_VERSION
COPY . /src
RUN cmake -S /src -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DGNUCASH_PREFIX=/opt/gnucash \
      -DGNUCASH_PINNED_VERSION=${GNUCASH_VERSION} \
    && cmake --build /build

# ---------------------------------------------------------------- runtime
# Deliverable: daichod + the exact engine it was built against, nothing else.
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      libglib2.0-0t64 guile-3.0-libs \
      libxml2 libxslt1.1 zlib1g libicu74 \
      libboost-date-time1.83.0 libboost-filesystem1.83.0 libboost-locale1.83.0 \
      libboost-program-options1.83.0 libboost-regex1.83.0 \
      libdbi1t64 libdbd-sqlite3 \
      libgrpc++1.51t64 libprotobuf32t64 \
      libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=gnucash /opt/gnucash /opt/gnucash
COPY --from=build /build/daichod /usr/local/bin/daichod
ENV PATH=/opt/gnucash/bin:$PATH \
    LD_LIBRARY_PATH=/opt/gnucash/lib:/opt/gnucash/lib/gnucash \
    GUILE_LOAD_PATH=/opt/gnucash/share/guile/site/3.0 \
    GUILE_LOAD_COMPILED_PATH=/opt/gnucash/lib/x86_64-linux-gnu/guile/3.0/site-ccache
ENTRYPOINT ["/usr/local/bin/daichod"]
