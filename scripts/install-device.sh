#!/usr/bin/env bash
# Install a compiled RNBO device to Move
#
# Usage: ./scripts/install-device.sh <device-name> <device.so> [description.json] [synth|fx|midifx]
#
# Examples:
#   ./scripts/install-device.sh my-synth /path/to/device.so /path/to/description.json
#   ./scripts/install-device.sh my-delay /path/to/device.so /path/to/description.json fx
#
# If type is not specified, auto-detects from description.json.
# If no description.json is provided, installs as synth with no parameter metadata.
#
set -e

MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"

DEVICE_NAME="$1"
DEVICE_SO="$2"
DESC_JSON="$3"
DEVICE_TYPE="$4"

if [ -z "$DEVICE_NAME" ] || [ -z "$DEVICE_SO" ]; then
    echo "Usage: $0 <device-name> <device.so> [description.json] [synth|fx|midifx]"
    echo ""
    echo "Examples:"
    echo "  $0 my-synth ./device.so ./description.json"
    echo "  $0 my-delay ./device.so ./description.json fx"
    echo "  $0 my-synth ./device.so   # no params, defaults to synth"
    exit 1
fi

if [ ! -f "$DEVICE_SO" ]; then
    echo "ERROR: device.so not found at $DEVICE_SO"
    exit 1
fi

# Auto-detect type from description.json
if [ -z "$DEVICE_TYPE" ]; then
    if [ -n "$DESC_JSON" ] && [ -f "$DESC_JSON" ]; then
        NUM_IN=$(grep -o '"numInputChannels"[[:space:]]*:[[:space:]]*[0-9]*' "$DESC_JSON" | grep -o '[0-9]*$')
        NUM_OUT=$(grep -o '"numOutputChannels"[[:space:]]*:[[:space:]]*[0-9]*' "$DESC_JSON" | grep -o '[0-9]*$')
        NUM_MIDI_OUT=$(grep -o '"numMidiOutputPorts"[[:space:]]*:[[:space:]]*[0-9]*' "$DESC_JSON" | grep -o '[0-9]*$')

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
        echo "Auto-detected type: $DEVICE_TYPE"
    else
        DEVICE_TYPE="synth"
        echo "No description.json — defaulting to synth"
    fi
fi

# Determine install path
case "$DEVICE_TYPE" in
    synth)  MODULE_DIR="/data/UserData/move-anything/modules/sound_generators/rnbo-synth" ;;
    fx)     MODULE_DIR="/data/UserData/move-anything/modules/audio_fx/rnbo-fx" ;;
    midifx) MODULE_DIR="/data/UserData/move-anything/modules/midi_fx/rnbo-midifx" ;;
    *)
        echo "ERROR: Unknown type '$DEVICE_TYPE'. Use synth, fx, or midifx."
        exit 1
        ;;
esac

DEST="$MODULE_DIR/devices/$DEVICE_NAME"

echo "=== Installing RNBO Device ==="
echo "Device:  $DEVICE_NAME"
echo "Type:    $DEVICE_TYPE"
echo "Target:  $MOVE_USER@$MOVE_HOST:$DEST"
echo ""

# Create directory and copy files
ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $DEST"
scp "$DEVICE_SO" "$MOVE_USER@$MOVE_HOST:$DEST/device.so"
ssh "$MOVE_USER@$MOVE_HOST" "chmod +x $DEST/device.so"

if [ -n "$DESC_JSON" ] && [ -f "$DESC_JSON" ]; then
    scp "$DESC_JSON" "$MOVE_USER@$MOVE_HOST:$DEST/description.json"
fi

echo ""
echo "=== Done ==="
echo "Device '$DEVICE_NAME' installed to rnbo-$DEVICE_TYPE."
echo "Restart Move Anything to pick it up."
