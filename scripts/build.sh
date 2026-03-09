#!/usr/bin/env bash
# build.sh — flock 기반 빌드 직렬화 래퍼
# 같은 build 디렉토리에 대한 동시 빌드를 방지한다.
#
# 사용법:
#   scripts/build.sh [preset]        # default: "default"
#   scripts/build.sh debug
#
# CMAKE_BUILD_PARALLEL_LEVEL 환경변수가 설정되어 있으면 그 값을 사용하고,
# 없으면 2로 제한한다 (devcontainer 4vCPU/7.7GB 환경 OOM 방지).

set -euo pipefail

# 프로젝트 루트로 이동 (어디서 호출해도 동일하게 동작)
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${PROJECT_ROOT}"

PRESET="${1:-default}"
BUILD_DIR="build/${PRESET}"
LOCK_FILE="/tmp/dbgate-build-${PRESET}.lock"

export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

# configure if build directory doesn't exist
if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "[build.sh] Configuring preset '${PRESET}'..."
    cmake --preset "${PRESET}"
fi

# flock: 동일 preset에 대해 한 번에 하나의 빌드만 실행
exec 9>"${LOCK_FILE}"
if ! flock -n 9; then
    echo "[build.sh] 다른 빌드가 '${PRESET}' 대상으로 실행 중입니다. 대기합니다..." >&2
    flock 9
fi

echo "[build.sh] Building preset '${PRESET}' (-j${CMAKE_BUILD_PARALLEL_LEVEL})..."
cmake --build "${BUILD_DIR}" 2>&1
