FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Basic tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++-14 \
    gcc-14 \
    cmake \
    ninja-build \
    git \
    curl \
    wget \
    unzip \
    zip \
    tar \
    pkg-config \
    ca-certificates \
    gnupg \
    lsb-release \
    software-properties-common \
    # MySQL client
    mysql-client \
    libmysqlclient-dev \
    # SSL
    libssl-dev \
    # clang tools (for clang-tidy, clang-format)
    clang-tools \
    clang-format \
    clang-tidy \
    # Debugging
    gdb \
    valgrind \
    # Other
    sudo \
    locales \
    && rm -rf /var/lib/apt/lists/*

# Set GCC 14 as default
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-14 100

# Set locale
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# Install Go 1.22
ARG GO_VERSION=1.22.10
RUN curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" | tar -C /usr/local -xz
ENV PATH="/usr/local/go/bin:/root/go/bin:${PATH}"
ENV GOPATH="/root/go"

# Install Node.js 22 LTS (for Claude Code, Codex CLI)
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# Install Claude Code & Codex CLI
RUN npm install -g @anthropic-ai/claude-code @openai/codex

# Install vcpkg
ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics \
    && ln -s ${VCPKG_ROOT}/vcpkg /usr/local/bin/vcpkg
ENV VCPKG_DEFAULT_TRIPLET=x64-linux

# Set working directory
WORKDIR /workspace
