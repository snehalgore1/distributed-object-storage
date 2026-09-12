# Multi-stage build for the distributed object store (spec Milestone 11).
# Build context is the repository root:  docker build -f deploy/docker/Dockerfile .

# ---- build stage -------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build g++ pkg-config ca-certificates \
      protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev \
      libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src
RUN cmake -S . -B build -G Ninja -DBUILD_TESTING=OFF \
    && cmake --build build --target dos_node dos_metadata dos_gateway dos_cli

# ---- runtime stage -----------------------------------------------------------
FROM ubuntu:24.04 AS runtime
# Runtime shared libraries only (gRPC/Protobuf/Abseil pulled in transitively),
# plus curl for the gateway health check.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libgrpc++1.51t64 libprotobuf32t64 libsqlite3-0 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/dos_node /src/build/dos_metadata \
                  /src/build/dos_gateway /src/build/dos_cli /usr/local/bin/

# Storage nodes write here; mount a volume for durability.
RUN mkdir -p /data
