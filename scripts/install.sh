#!/usr/bin/env bash
# Install RNBO modules to Move device
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"
BASE_DIR="/data/UserData/schwung/modules"

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

    # Check if pack or devices exist
    local HAS_PACK=false
    local HAS_DEVICES=false
    [ -d "$REPO_ROOT/dist/$MODULE_ID/pack" ] && HAS_PACK=true
    [ -d "$REPO_ROOT/dist/$MODULE_ID/devices" ] && [ -n "$(ls -A "$REPO_ROOT/dist/$MODULE_ID/devices/" 2>/dev/null)" ] && HAS_DEVICES=true

    if ! $HAS_PACK && ! $HAS_DEVICES; then
        echo "Skipping $MODULE_ID (no pack or devices)"
        return
    fi

    echo ""
    echo "--- $MODULE_ID ---"
    ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MODULE_DIR"

    scp "$REPO_ROOT/dist/$MODULE_ID/module.json" "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/"

    # Copy host .so and bridge.so
    if [ -f "$REPO_ROOT/dist/$MODULE_ID/dsp.so" ]; then
        scp "$REPO_ROOT/dist/$MODULE_ID/dsp.so" "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/"
    elif [ -f "$REPO_ROOT/dist/$MODULE_ID/$MODULE_ID.so" ]; then
        scp "$REPO_ROOT/dist/$MODULE_ID/$MODULE_ID.so" "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/"
    fi
    if [ -f "$REPO_ROOT/dist/$MODULE_ID/bridge.so" ]; then
        scp "$REPO_ROOT/dist/$MODULE_ID/bridge.so" "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/"
    fi

    # Copy pack directory (graph mode)
    if $HAS_PACK; then
        echo "  Installing pack (graph)..."
        ssh "$MOVE_USER@$MOVE_HOST" "rm -rf $MODULE_DIR/pack && mkdir -p $MODULE_DIR/pack"
        # Use tar to preserve directory structure efficiently
        (cd "$REPO_ROOT/dist/$MODULE_ID/pack" && tar cf - .) | \
            ssh "$MOVE_USER@$MOVE_HOST" "cd $MODULE_DIR/pack && tar xf -"
    fi

    # Copy devices directory (legacy single-device mode)
    if $HAS_DEVICES; then
        ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MODULE_DIR/devices"
        for dev_dir in "$REPO_ROOT/dist/$MODULE_ID/devices"/*/; do
            [ -d "$dev_dir" ] || continue
            dev_name="$(basename "$dev_dir")"
            echo "  Device: $dev_name"
            ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MODULE_DIR/devices/$dev_name"
            scp "$dev_dir"* "$MOVE_USER@$MOVE_HOST:$MODULE_DIR/devices/$dev_name/"
        done
    fi

    # Chain patches
    if [ -d "$REPO_ROOT/dist/$MODULE_ID/chain_patches" ]; then
        ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p /data/UserData/move-anything/patches"
        scp "$REPO_ROOT/dist/$MODULE_ID/chain_patches/"*.json "$MOVE_USER@$MOVE_HOST:/data/UserData/move-anything/patches/" 2>/dev/null || true
    fi
}

install_module "rnbo-synth"  "sound_generators"
install_module "rnbo-fx"     "audio_fx"
install_module "rnbo-midifx" "midi_fx"

# Install RNBO Runner overtake module (pure JS, no DSP)
if [ -d "$REPO_ROOT/dist/rnbo-runner" ]; then
    echo ""
    echo "--- rnbo-runner (overtake) ---"
    RUNNER_DIR="$BASE_DIR/overtake/rnbo-runner"
    ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $RUNNER_DIR"
    scp "$REPO_ROOT/dist/rnbo-runner/module.json" "$MOVE_USER@$MOVE_HOST:$RUNNER_DIR/"
    scp "$REPO_ROOT/dist/rnbo-runner/ui.js" "$MOVE_USER@$MOVE_HOST:$RUNNER_DIR/"
    if [ -f "$REPO_ROOT/dist/rnbo-runner/exit-hook.sh" ]; then
        scp "$REPO_ROOT/dist/rnbo-runner/exit-hook.sh" "$MOVE_USER@$MOVE_HOST:$RUNNER_DIR/"
        ssh "$MOVE_USER@$MOVE_HOST" "chmod +x $RUNNER_DIR/exit-hook.sh"
    fi
fi

echo ""
echo "=== Install Complete ==="
echo "Restart Schwung to load the modules."
