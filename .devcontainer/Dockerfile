FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG CLAUDE_CODE_VERSION=latest
ARG GO_VERSION=1.22.10
ARG ZSH_IN_DOCKER_VERSION=1.2.1

# -----------------------------
# Base OS deps (+ enable universe)
# -----------------------------
RUN set -eux; \
  apt-get update; \
  apt-get install -y --no-install-recommends \
    ca-certificates \
    gnupg \
    curl \
    wget \
    lsb-release \
    software-properties-common; \
  add-apt-repository -y universe; \
  apt-get update; \
  apt-get install -y --no-install-recommends \
    # Build tools
    build-essential \
    gcc-14 \
    g++-14 \
    cmake \
    ninja-build \
    pkg-config \
    # VCS / CLI
    git \
    gh \
    fzf \
    man-db \
    # Shell
    zsh \
    # Archivers
    unzip \
    zip \
    tar \
    # MySQL client
    mysql-client \
    libmysqlclient-dev \
    # SSL
    libssl-dev \
    # clang tools
    clang-tools \
    clang-format \
    clang-tidy \
    # Debugging / networking
    gdb \
    valgrind \
    iptables \
    ipset \
    iproute2 \
    dnsutils \
    aggregate \
    jq \
    ruby \
    # Locale
    locales \
    nano \
    vim \
  ; \
  apt-get clean; \
  rm -rf /var/lib/apt/lists/*

# -----------------------------
# Set GCC 14 as default
# -----------------------------
RUN set -eux; \
  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100; \
  update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100; \
  update-alternatives --install /usr/bin/cc  cc  /usr/bin/gcc-14 100; \
  update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-14 100

# -----------------------------
# Persist shell history (bash)
# -----------------------------
RUN set -eux; \
  mkdir -p /commandhistory; \
  touch /commandhistory/.bash_history

# -----------------------------
# Locale
# -----------------------------
RUN set -eux; \
  locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 \
    LANGUAGE=en_US:en \
    LC_ALL=en_US.UTF-8

# -----------------------------
# Go (multi-arch: amd64/arm64)
# -----------------------------
RUN set -eux; \
  ARCH="$(dpkg --print-architecture)"; \
  case "$ARCH" in \
    amd64) GO_ARCH="amd64" ;; \
    arm64) GO_ARCH="arm64" ;; \
    *) echo "Unsupported arch: $ARCH" >&2; exit 1 ;; \
  esac; \
  curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz" | tar -C /usr/local -xz
ENV GOPATH=/root/go \
    PATH=/usr/local/go/bin:/root/go/bin:${PATH}

# -----------------------------
# Go tools (pin versions for reproducible devcontainer builds)
# -----------------------------
RUN go install golang.org/x/tools/gopls@v0.16.2

# -----------------------------
# Devcontainer marker + workspace
# -----------------------------
ENV DEVCONTAINER=true
RUN set -eux; \
  mkdir -p /workspace /root/.claude

WORKDIR /workspace

# -----------------------------
# Node.js 22 LTS (NodeSource via keyring)
# - Avoid piping remote script to bash
# -----------------------------
RUN set -eux; \
  mkdir -p /etc/apt/keyrings; \
  curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key \
    | gpg --dearmor -o /etc/apt/keyrings/nodesource.gpg; \
  echo "deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_22.x nodistro main" \
    > /etc/apt/sources.list.d/nodesource.list; \
  apt-get update; \
  apt-get install -y --no-install-recommends nodejs; \
  apt-get clean; \
  rm -rf /var/lib/apt/lists/*

# -----------------------------
# Default shell (for terminal UX)
# -----------------------------
ENV SHELL=/bin/zsh

# Set the default editor and visual
ENV EDITOR=nano
ENV VISUAL=nano

# -----------------------------
# zsh-in-docker (powerline10k default)
# -----------------------------
RUN set -eux; \
  sh -c "$(wget -O- https://github.com/deluan/zsh-in-docker/releases/download/v${ZSH_IN_DOCKER_VERSION}/zsh-in-docker.sh)" -- \
    -p git \
    -a "export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
    -p https://github.com/zsh-users/zsh-autosuggestions \
    -p https://github.com/zsh-users/zsh-completions \
    -x

# -----------------------------
# Install Codex CLI & Claude Code
# -----------------------------
RUN set -eux; \
  npm install -g @openai/codex "@anthropic-ai/claude-code@${CLAUDE_CODE_VERSION}"

# -----------------------------
# Install vcpkg
# -----------------------------
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_TRIPLET=x64-linux
ENV CMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
RUN set -eux; \
  git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"; \
  "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics; \
  ln -sf "${VCPKG_ROOT}/vcpkg" /usr/local/bin/vcpkg

# -----------------------------
# Firewall helper (root-only; no sudoers needed)
# -----------------------------
COPY .devcontainer/init-firewall.sh /usr/local/bin/
RUN set -eux; \
  chmod +x /usr/local/bin/init-firewall.sh

# Final user (root)
USER root
