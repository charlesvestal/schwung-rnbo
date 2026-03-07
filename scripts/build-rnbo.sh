#!/usr/bin/env bash
# Compile a stock RNBO C++ export to an ARM64 .so for Move Anything
#
# Usage: ./scripts/build-rnbo.sh /path/to/rnbo-export [device-name] [synth|fx|midifx]
#
# The export directory should contain:
#   - rnbo_source.cpp (the generated patcher code)
#   - rnbo/           (the RNBO C++ runtime)
#   - description.json
#
# The type (synth/fx/midifx) is auto-detected from description.json if not specified.
#
# Output: dist/rnbo-<type>/devices/<device-name>/device.so + description.json
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

EXPORT_DIR="$1"
DEVICE_NAME="$2"
DEVICE_TYPE="$3"

if [ -z "$EXPORT_DIR" ]; then
    echo "Usage: $0 <rnbo-export-dir> [device-name] [synth|fx|midifx]"
    echo ""
    echo "Examples:"
    echo "  $0 ~/Documents/RNBO/exports/my-synth"
    echo "  $0 ~/Documents/RNBO/exports/my-delay my-delay fx"
    echo ""
    echo "Type is auto-detected from description.json if not specified."
    exit 1
fi

EXPORT_DIR="$(cd "$EXPORT_DIR" && pwd)"

# Auto-detect device name from directory
if [ -z "$DEVICE_NAME" ]; then
    DEVICE_NAME="$(basename "$EXPORT_DIR")"
fi

# Auto-detect type from description.json
if [ -z "$DEVICE_TYPE" ]; then
    if [ -f "$EXPORT_DIR/description.json" ]; then
        NUM_IN=$(grep -o '"numInputChannels"[[:space:]]*:[[:space:]]*[0-9]*' "$EXPORT_DIR/description.json" | grep -o '[0-9]*$')
        NUM_OUT=$(grep -o '"numOutputChannels"[[:space:]]*:[[:space:]]*[0-9]*' "$EXPORT_DIR/description.json" | grep -o '[0-9]*$')
        NUM_MIDI_OUT=$(grep -o '"numMidiOutputPorts"[[:space:]]*:[[:space:]]*[0-9]*' "$EXPORT_DIR/description.json" | grep -o '[0-9]*$')

        NUM_IN="${NUM_IN:-0}"
        NUM_OUT="${NUM_OUT:-0}"
        NUM_MIDI_OUT="${NUM_MIDI_OUT:-0}"

        if [ "$NUM_OUT" -eq 0 ] && [ "$NUM_MIDI_OUT" -gt 0 ]; then
            DEVICE_TYPE="midifx"
        elif [ "$NUM_IN" -gt 0 ]; then
            DEVICE_TYPE="fx"
        else
            DEVICE_TYPE="synth"
        fi
        echo "Auto-detected type: $DEVICE_TYPE (${NUM_IN}in/${NUM_OUT}out, ${NUM_MIDI_OUT} midi out)"
    else
        DEVICE_TYPE="synth"
        echo "No description.json — defaulting to synth"
    fi
fi

MODULE_ID="rnbo-$DEVICE_TYPE"

# Validate export directory
if [ ! -d "$EXPORT_DIR/rnbo" ]; then
    echo "ERROR: No rnbo/ directory found in $EXPORT_DIR"
    echo "This doesn't look like an RNBO C++ export."
    exit 1
fi

# Find the generated patcher source file
PATCHER_SRC=""
if [ -f "$EXPORT_DIR/rnbo_source.cpp" ]; then
    PATCHER_SRC="rnbo_source.cpp"
else
    # Look for any .cpp that isn't in rnbo/
    for f in "$EXPORT_DIR"/*.cpp; do
        [ -f "$f" ] || continue
        base="$(basename "$f")"
        if [ "$base" != "RNBO.cpp" ]; then
            PATCHER_SRC="$base"
            break
        fi
    done
fi

if [ -z "$PATCHER_SRC" ]; then
    echo "ERROR: No patcher source file found in $EXPORT_DIR"
    echo "Expected rnbo_source.cpp or a .cpp file."
    exit 1
fi

echo "=== Building RNBO Device: $DEVICE_NAME ($MODULE_ID) ==="
echo "Export dir: $EXPORT_DIR"
echo "Patcher:   $PATCHER_SRC"

# Copy the device API header and wrapper into the export dir
mkdir -p "$EXPORT_DIR/_build"
cp "$REPO_ROOT/src/dsp/rnbo_device_api.h" "$EXPORT_DIR/_build/"
cp "$REPO_ROOT/src/dsp/rnbo_device_wrapper.cpp" "$EXPORT_DIR/_build/"

# Create wrapper that includes RNBO runtime + patcher + C API wrapper
cat > "$EXPORT_DIR/_build_wrapper.cpp" << WRAPPER_EOF
#define RNBO_USE_FLOAT32
#include "rnbo/RNBO.cpp"
#include "$PATCHER_SRC"
#include "_build/rnbo_device_wrapper.cpp"
WRAPPER_EOF

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi

    echo "Compiling via Docker..."
    docker run --rm \
        -v "$EXPORT_DIR:/export" \
        -w /export \
        "$IMAGE_NAME" \
        aarch64-linux-gnu-g++ -shared -fPIC -DRNBO_USE_FLOAT32 -std=c++17 -O2 \
            -I. -Irnbo -Irnbo/common -Irnbo/src -I_build \
            -o _build/device.so _build_wrapper.cpp \
            -lm
else
    CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
    echo "Compiling with $CROSS_PREFIX..."
    cd "$EXPORT_DIR"
    ${CROSS_PREFIX}g++ -shared -fPIC -DRNBO_USE_FLOAT32 -std=c++17 -O2 \
        -I. -Irnbo -Irnbo/common -Irnbo/src -I_build \
        -o _build/device.so _build_wrapper.cpp \
        -lm
fi

# Install to dist under the correct module
DEST="$REPO_ROOT/dist/$MODULE_ID/devices/$DEVICE_NAME"
mkdir -p "$DEST"
cat "$EXPORT_DIR/_build/device.so" > "$DEST/device.so"
chmod +x "$DEST/device.so"

if [ -f "$EXPORT_DIR/description.json" ]; then
    cat "$EXPORT_DIR/description.json" > "$DEST/description.json"
else
    echo "WARNING: No description.json found — parameter metadata will not be available"
fi

# Clean up
rm -rf "$EXPORT_DIR/_build" "$EXPORT_DIR/_build_wrapper.cpp"

# Determine install path on device
case "$DEVICE_TYPE" in
    synth)  INSTALL_DIR="sound_generators/rnbo-synth" ;;
    fx)     INSTALL_DIR="audio_fx/rnbo-fx" ;;
    midifx) INSTALL_DIR="midi_fx/rnbo-midifx" ;;
esac

echo ""
echo "=== Device Built ==="
echo "Output: $DEST/"
echo ""
echo "To deploy to Move, copy to the module's devices directory:"
echo "  scp -r $DEST ableton@move.local:/data/UserData/move-anything/modules/$INSTALL_DIR/devices/"
