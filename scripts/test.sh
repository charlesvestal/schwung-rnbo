#!/usr/bin/env bash
# Host unit tests. Native build: the host has no RNBO dependency, so no cross
# toolchain or Docker is needed. Usage: ./scripts/test.sh
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"
mkdir -p build
CXX="${CXX:-c++}"
"$CXX" -g -O0 -std=c++17 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Isrc/dsp \
    tests/test_state.cpp -o build/test_state -ldl
./build/test_state
