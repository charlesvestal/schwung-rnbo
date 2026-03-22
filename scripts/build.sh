#!/usr/bin/env bash
# Build RNBO host + bridge for Move Anything (ARM64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== RNBO Build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    echo "=== Done ==="
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building RNBO Host + Bridge ==="

if [ ! -f "rnbo-sdk/RNBO.h" ]; then
    echo "ERROR: rnbo-sdk/RNBO.h not found"
    exit 1
fi

mkdir -p build

# 1) Build host plugin (flat C API loader — no RNBO dependency)
echo "Compiling host..."
${CROSS_PREFIX}g++ -g -O2 -fPIC -std=c++17 \
    -Isrc/dsp \
    -c src/dsp/rnbo_host.cpp \
    -o build/rnbo_host.o

echo "Linking host dsp.so..."
${CROSS_PREFIX}g++ -shared \
    build/rnbo_host.o \
    -o build/dsp.so \
    -ldl -lm

# 2) Build bridge (wraps rnbopack .so in flat C API — needs RNBO SDK)
RNBO_INCLUDES="-Irnbo-sdk -Irnbo-sdk/src -Irnbo-sdk/common -Irnbo-sdk/src/3rdparty"

echo "Compiling bridge..."
${CROSS_PREFIX}g++ -g -O2 -fPIC -std=c++17 \
    -DRNBO_NO_PATCHERFACTORY \
    -Isrc/dsp \
    $RNBO_INCLUDES \
    -c src/dsp/rnbo_bridge.cpp \
    -o build/rnbo_bridge.o

echo "Compiling RNBO runtime..."
${CROSS_PREFIX}g++ -g -O2 -fPIC -std=c++17 \
    -DRNBO_NO_PATCHERFACTORY \
    $RNBO_INCLUDES \
    -c rnbo-sdk/RNBO.cpp \
    -o build/rnbo_runtime.o

echo "Linking bridge.so..."
${CROSS_PREFIX}g++ -shared \
    build/rnbo_bridge.o \
    build/rnbo_runtime.o \
    -o build/bridge.so \
    -ldl -lm

# Package modules
for MODULE in rnbo-synth rnbo-fx rnbo-midifx; do
    echo "Packaging $MODULE..."
    mkdir -p "dist/$MODULE/devices"
    cat "src/modules/$MODULE.json" > "dist/$MODULE/module.json"
    if [ "$MODULE" = "rnbo-synth" ] || [ "$MODULE" = "rnbo-midifx" ]; then
        cat build/dsp.so > "dist/$MODULE/dsp.so"
        chmod +x "dist/$MODULE/dsp.so"
    else
        cat build/dsp.so > "dist/$MODULE/$MODULE.so"
        chmod +x "dist/$MODULE/$MODULE.so"
    fi
    # Include bridge.so for runtime rnbopack extraction
    cat build/bridge.so > "dist/$MODULE/bridge.so"
    chmod +x "dist/$MODULE/bridge.so"
done

# Package RNBO Runner overtake module (no DSP — pure JS)
echo "Packaging rnbo-runner..."
mkdir -p "dist/rnbo-runner"
cp src/modules/rnbo-runner/module.json "dist/rnbo-runner/"
cp src/modules/rnbo-runner/ui.js "dist/rnbo-runner/"

echo ""
echo "=== Build Complete ==="
echo "Host: build/dsp.so"
echo "Bridge: build/bridge.so"
echo ""
echo "Next: install rnbopack files:"
echo "  ./scripts/install-rnbopack.sh /path/to/file.rnbopack"
