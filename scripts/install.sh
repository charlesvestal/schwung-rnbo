#!/usr/bin/env bash
# Install RNBO modules to Move device
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"
BASE_DIR="/data/UserData/move-anything/modules"

echo "=== Installing RNBO Modules ==="
echo "Target: $MOVE_USER@$MOVE_HOST"

install_module() {
    local MODULE_ID="$1"
    local CATEGORY="$2"
    local MODULE_DIR="$BASE_DIR/$CATEGORY/$MODULE_ID"

    if [ ! -d "$REPO_ROOT/dist/$MODULE_ID" ]; then
        echo "Skipping $MODULE_ID (not built)"
        return
    fi

    # Check if any devices exist
    if [ ! -d "$REPO_ROOT/dist/$MODULE_ID/devices" ] || [ -z "$(ls -A "$REPO_ROOT/dist/$MODULE_ID/devices/" 2>/dev/null)" ]; then
        echo "Skipping $MODULE_ID (no devices)"
        return
    fi

    echo ""
    echo "--- $MODULE_ID ---"
    ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MODULE_DIR/devices"

    scp "$REPO_ROOT/dist/$MODULE_ID/module.json" "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/"
    scp "$REPO_ROOT/dist/$MODULE_ID/dsp.so" "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/"

    for dev_dir in "$REPO_ROOT/dist/$MODULE_ID/devices"/*/; do
        [ -d "$dev_dir" ] || continue
        dev_name="$(basename "$dev_dir")"
        echo "  Device: $dev_name"
        ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MODULE_DIR/devices/$dev_name"
        scp "$dev_dir"* "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/devices/$dev_name/"
    done

    # Chain patches
    if [ -d "$REPO_ROOT/dist/$MODULE_ID/chain_patches" ]; then
        ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p /data/UserData/move-anything/patches"
        scp "$REPO_ROOT/dist/$MODULE_ID/chain_patches/"*.json "$MOVE_USER@$MOVE_HOST:/data/UserData/move-anything/patches/" 2>/dev/null || true
    fi
}

install_module "rnbo-synth"  "sound_generators"
install_module "rnbo-fx"     "audio_fx"
install_module "rnbo-midifx" "midi_fx"

echo ""
echo "=== Install Complete ==="
echo "Restart Move Anything to load the modules."
