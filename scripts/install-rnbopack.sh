#!/usr/bin/env bash
# Install rnbopack devices using the bridge approach.
# Each patcher gets: device.so (bridge) + patcher.so (rnbopack binary) + description.json
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

RNBOPACK="$1"

if [ -z "$RNBOPACK" ]; then
    echo "Usage: $0 <path-to-rnbopack>"
    exit 1
fi

[ -f "$RNBOPACK" ] || { echo "ERROR: $RNBOPACK not found"; exit 1; }
[ -f "$REPO_ROOT/build/bridge.so" ] || { echo "ERROR: build/bridge.so not found. Run ./scripts/build.sh first"; exit 1; }

TMPDIR="$(mktemp -d)"
trap "rm -rf '$TMPDIR'" EXIT

echo "=== Installing rnbopack ==="
tar -xf "$RNBOPACK" -C "$TMPDIR"
PACK_DIR="$(ls -d "$TMPDIR"/*/ | head -1)"
[ -f "$PACK_DIR/info.json" ] || { echo "ERROR: no info.json"; exit 1; }

python3 -c "
import json, os, shutil

pack = '$PACK_DIR'
repo = '$REPO_ROOT'
bridge = os.path.join(repo, 'build', 'bridge.so')

with open(os.path.join(pack, 'info.json')) as f:
    info = json.load(f)

# Find aarch64 target
target = None
for k in info.get('targets', {}):
    if 'aarch64' in k:
        target = k
        break
if not target:
    print('ERROR: no aarch64 target')
    exit(1)

print(f'Pack: {info[\"name\"]} (RNBO {info[\"rnbo_version\"]})')

for p in info['patchers']:
    name = p['name']
    dir_name = name.lower().replace(' ', '-').replace('_', '-')

    binary = p.get('binaries', {}).get(target)
    if not binary:
        print(f'  SKIP: {name} (no binary)')
        continue
    binary_path = os.path.join(pack, binary)
    if not os.path.exists(binary_path):
        print(f'  SKIP: {name} (binary missing)')
        continue

    config_path = os.path.join(pack, p.get('config', ''))
    if not os.path.exists(config_path):
        print(f'  SKIP: {name} (no config)')
        continue

    with open(config_path) as cf:
        config = json.load(cf)

    num_in = config.get('numInputChannels', 0)
    num_out = config.get('numOutputChannels', 0)
    num_midi_out = config.get('numMidiOutputPorts', 0)

    if num_out == 0 and num_midi_out > 0:
        dev_type = 'midifx'
    elif num_in > 0:
        dev_type = 'fx'
    else:
        dev_type = 'synth'

    # For graph mode, all devices go into the primary module
    # (determined after the loop). For now, collect them all as synth.
    module_id = 'rnbo-synth'
    dest = os.path.join(repo, 'dist', module_id, 'devices', dir_name)
    os.makedirs(dest, exist_ok=True)

    # Copy bridge as device.so
    shutil.copy2(bridge, os.path.join(dest, 'device.so'))
    os.chmod(os.path.join(dest, 'device.so'), 0o755)

    # Copy rnbopack binary as patcher.so
    shutil.copy2(binary_path, os.path.join(dest, 'patcher.so'))
    os.chmod(os.path.join(dest, 'patcher.so'), 0o755)

    # Copy config as description.json
    shutil.copy2(config_path, os.path.join(dest, 'description.json'))

    print(f'  {name} -> {module_id}/devices/{dir_name}/ ({dev_type})')

    # Determine primary module (the one with the audio output)
    primary_module = 'rnbo-synth'  # default
    for p in info['patchers']:
        cfg_path = os.path.join(pack, p.get('config',''))
        if os.path.exists(cfg_path):
            with open(cfg_path) as f: cfg=json.load(f)
            if cfg.get('numOutputChannels',0) > 0 and cfg.get('numInputChannels',0) > 0:
                primary_module = 'rnbo-fx'
                break

    # Copy full pack structure (info.json + sets/) into the primary module
    pack_dest = os.path.join(repo, 'dist', primary_module, 'pack')
    if os.path.exists(pack_dest):
        shutil.rmtree(pack_dest)
    os.makedirs(pack_dest, exist_ok=True)
    shutil.copy2(os.path.join(pack, 'info.json'), os.path.join(pack_dest, 'info.json'))
    sets_src = os.path.join(pack, 'sets')
    if os.path.exists(sets_src):
        shutil.copytree(sets_src, os.path.join(pack_dest, 'sets'))

print('Done!')
"

echo ""
echo "To deploy: ./scripts/install.sh"
