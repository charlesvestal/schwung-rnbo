#!/usr/bin/env bash
# Build RNBO host modules for Move Anything (ARM64)
# Produces three modules: rnbo-synth, rnbo-fx, rnbo-midifx
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== RNBO Host Modules Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building RNBO Host Modules ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build

echo "Compiling RNBO host plugin..."
${CROSS_PREFIX}g++ -g -O2 -fPIC -std=c++17 \
    -Isrc/dsp \
    -c src/dsp/rnbo_host.cpp \
    -o build/rnbo_host.o

echo "Linking dsp.so..."
${CROSS_PREFIX}g++ -shared \
    build/rnbo_host.o \
    -o build/dsp.so \
    -ldl -lm

# Package three modules, each with the same dsp.so but different module.json
for MODULE in rnbo-synth rnbo-fx rnbo-midifx; do
    echo "Packaging $MODULE..."
    mkdir -p "dist/$MODULE/devices"
    cat "src/modules/$MODULE.json" > "dist/$MODULE/module.json"
    cat build/dsp.so > "dist/$MODULE/dsp.so"
    chmod +x "dist/$MODULE/dsp.so"

    # Include chain patches if any
    if [ -d "src/chain_patches/$MODULE" ]; then
        mkdir -p "dist/$MODULE/chain_patches"
        for f in "src/chain_patches/$MODULE"/*.json; do
            [ -f "$f" ] && cat "$f" > "dist/$MODULE/chain_patches/$(basename "$f")"
        done
    fi

    # Create tarball
    cd dist
    tar -czvf "$MODULE-module.tar.gz" "$MODULE/"
    cd ..
done

echo ""
echo "=== Build Complete ==="
echo "Output:"
echo "  dist/rnbo-synth/      (sound generator)"
echo "  dist/rnbo-fx/         (audio effect)"
echo "  dist/rnbo-midifx/     (MIDI effect)"
echo ""
echo "Next: compile your RNBO exports with:"
echo "  ./scripts/build-rnbo.sh /path/to/rnbo-export <device-name> [synth|fx|midifx]"
