#!/usr/bin/env bash
#
# Run `scripts/linux-sanitizers.sh tsan` to check for thread races on Linux.
# With no argument, the script runs all three sanitizer presets.
#
# Builds run on a copy so they leave the host checkout alone.

set -euo pipefail

usage() {
    echo "usage: $0 [asan|tsan|msan|all]" >&2
    exit 2
}

[[ $# -le 1 ]] || usage
case "${1:-all}" in
    asan | tsan | msan) presets="$1" ;;
    all) presets="asan tsan msan" ;;
    *) usage ;;
esac

if [[ -n "${PS_CONTAINER:-}" ]]; then
    runtime="$PS_CONTAINER"
elif command -v podman > /dev/null 2>&1; then
    runtime=podman
elif command -v docker > /dev/null 2>&1; then
    runtime=docker
else
    echo "$0: no container runtime (set PS_CONTAINER, or install podman or docker)" >&2
    exit 1
fi

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

inner='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    clang libclang-rt-dev cmake ninja-build libc6-dev > /dev/null
mkdir -p /work
tar -C /src --exclude=./build --exclude=./.cache -cf - . | tar -C /work -xf -
cd /work
for p in "$@"; do
    echo "=== preset $p ==="
    cmake --preset "$p" -DCMAKE_C_COMPILER=clang
    cmake --build --preset "$p"
    ctest --preset "$p"
done
echo "=== all presets passed: $* ==="
'

opts=()
if [[ " $presets " == *" tsan "* ]]; then
    # TSan may need to restart with address randomization disabled. The default
    # container policy blocks that request, so tests can crash on startup.
    opts+=(--security-opt seccomp=unconfined)
fi

# shellcheck disable=SC2086 # $presets is a word list on purpose
exec "$runtime" run --rm "${opts[@]}" \
    -v "$repo:/src:ro" \
    ubuntu:24.04 \
    bash -c "$inner" bash $presets
