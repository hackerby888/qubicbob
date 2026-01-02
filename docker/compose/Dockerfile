# Build stage
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    uuid-dev \
    libhiredis-dev \
    zlib1g-dev \
    libjsoncpp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc)

# Runtime stage - bob only (connects to external Redis/KeyDB and Kvrocks)
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libssl3t64 \
    libuuid1 \
    libhiredis1.1.0 \
    libjsoncpp25 \
    ca-certificates \
    wget \
    unzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy built binary
COPY --from=builder /app/build/bob /app/bob

# Create directories
RUN mkdir -p /app/config /data/bob

# Expose ports
# 21842 - bob server port
# 40420 - REST API port
EXPOSE 21842 40420

# Data volume for snapshots
VOLUME ["/data/bob"]

WORKDIR /data/bob

CMD ["/app/bob", "/app/config/bob.json"]
