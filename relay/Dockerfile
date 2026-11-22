# ── Build stage ──────────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build g++ git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakeLists.txt
COPY relay_server.cpp relay_server.cpp

RUN cmake -S . -B /build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM debian:bookworm-slim

COPY --from=build /build/wwhd_relay /usr/local/bin/wwhd_relay

# UDP port the relay listens on.
EXPOSE 7778/udp

ENV RELAY_PORT=7778

ENTRYPOINT ["/usr/local/bin/wwhd_relay"]
CMD ["7778"]
