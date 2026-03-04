# =============================================================================
# dbgate C++ core — multi-stage production build
# =============================================================================
# Build:  docker build -f deploy/Dockerfile -t dbgate:latest .
# Run:    docker run --rm -p 13306:13306 -p 8080:8080 dbgate:latest
# =============================================================================

# ---------------------------------------------------------------------------
# Stage 1: Builder
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive

# Build dependencies
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        build-essential \
        gcc-14 \
        g++-14 \
        cmake \
        ninja-build \
        pkg-config \
        git \
        curl \
        zip \
        unzip \
        tar \
        libssl-dev \
        ca-certificates; \
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100; \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100; \
    apt-get clean; \
    rm -rf /var/lib/apt/lists/*

# vcpkg
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_TRIPLET=x64-linux
ENV CMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

ARG VCPKG_GIT_REF=2026.01.16
ARG VCPKG_EXPECTED_SHA=66c0373dc7fca549e5803087b9487edfe3aca0a1

RUN set -eux; \
    git clone --depth=1 --branch "${VCPKG_GIT_REF}" https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"; \
    actual_sha="$(git -C "${VCPKG_ROOT}" rev-parse HEAD)"; \
    test "${actual_sha}" = "${VCPKG_EXPECTED_SHA}"; \
    "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics

# Copy dependency manifest first (cache layer)
WORKDIR /src
COPY vcpkg.json CMakeLists.txt CMakePresets.json ./

# Copy source tree
COPY src/ src/
COPY config/ config/

# Build release binary
RUN cmake --preset default && cmake --build build/default --target dbgate

# Verify no unexpected dynamic deps
RUN ldd build/default/dbgate || true

# ---------------------------------------------------------------------------
# Stage 2: Runtime
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        libstdc++6 \
        ca-certificates \
        curl; \
    apt-get clean; \
    rm -rf /var/lib/apt/lists/*

# Non-root user
RUN groupadd --gid 1000 dbgate && \
    useradd --uid 1000 --gid dbgate --shell /bin/false --create-home dbgate

# Directories
RUN mkdir -p /etc/dbgate /var/log/dbgate /run/dbgate && \
    chown -R dbgate:dbgate /var/log/dbgate /run/dbgate

# Copy binary and default policy
COPY --from=builder /src/build/default/dbgate /usr/local/bin/dbgate
COPY --from=builder /src/config/policy.yaml /etc/dbgate/policy.yaml

# Environment defaults
ENV PROXY_LISTEN_ADDR=0.0.0.0 \
    PROXY_LISTEN_PORT=13306 \
    MYSQL_HOST=127.0.0.1 \
    MYSQL_PORT=3306 \
    POLICY_PATH=/etc/dbgate/policy.yaml \
    UDS_SOCKET_PATH=/run/dbgate/dbgate.sock \
    LOG_PATH=/var/log/dbgate/dbgate.log \
    LOG_LEVEL=info \
    HEALTH_CHECK_PORT=8080 \
    MAX_CONNECTIONS=1000 \
    CONNECTION_TIMEOUT_SEC=30

EXPOSE 13306 8080

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

USER dbgate
ENTRYPOINT ["dbgate"]
