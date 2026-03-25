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

    echo ""
    echo "--- $MODULE_ID ---"
    # Create module dir with packs/ for rnbopack scanning
    # devices/, cache/, and pack/ are generated at runtime from packs/
    ssh "$MOVE_USER@$MOVE_HOST" "mkdir -p $MODULE_DIR/packs"

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
