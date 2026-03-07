# RNBO Host Module for Move Anything

Run [RNBO](https://rnbo.cycling74.com/) C++ exports from Max/MSP on Ableton Move via the Signal Chain.

## How It Works

Three host modules (`rnbo-synth`, `rnbo-fx`, `rnbo-midifx`) dynamically load compiled RNBO device `.so` files at runtime. Each device is a stock RNBO C++ export cross-compiled for ARM64 — no code changes needed.

```
Move Anything Signal Chain
    └── RNBO Host Module (dsp.so)
            └── devices/
                ├── my-synth/device.so + description.json
                └── my-delay/device.so + description.json
```

Parameters from `description.json` are automatically exposed to Move's knobs and Shadow UI.

## Building a Device from an RNBO Export

### Prerequisites

- Docker (for ARM64 cross-compilation)
- An RNBO C++ Source Code export from Max/MSP (contains `rnbo_source.cpp`, `rnbo/` directory, and `description.json`)

### Steps

1. **Export from Max/MSP**: In your RNBO patcher, use the "C++ Source Code Export" target. The export directory should contain:
   - `rnbo_source.cpp` (or another `.cpp` file)
   - `rnbo/` (the RNBO C++ runtime)
   - `description.json`

2. **Build the device**:
   ```bash
   ./scripts/build-rnbo.sh /path/to/rnbo-export [device-name] [synth|fx|midifx]
   ```
   - Device name defaults to the export directory name
   - Type is auto-detected from `description.json` (audio inputs → fx, MIDI output only → midifx, otherwise → synth)

3. **Deploy to Move**:
   ```bash
   # Deploy just the device
   ./scripts/install-device.sh dist/rnbo-synth/devices/my-synth

   # Or deploy everything
   ./scripts/install.sh
   ```

### Example

```bash
# Build waveguide string synth from a GitHub example
git clone https://github.com/bynarlogic/string-vst.git /tmp/string-vst
./scripts/build-rnbo.sh /tmp/string-vst/export waveguide-string

# Deploy to Move
./scripts/install-device.sh dist/rnbo-synth/devices/waveguide-string
```

## Building the Host Modules

The host modules only need to be built once (or when updating the host code):

```bash
./scripts/build.sh
./scripts/install.sh
```

This produces three modules that share the same `dsp.so`:
- `rnbo-synth` — Sound generator (Signal Chain synth slot)
- `rnbo-fx` — Audio effect (Signal Chain FX slot)
- `rnbo-midifx` — MIDI effect (Signal Chain MIDI FX slot)

## Important Notes

- **RNBO version matching**: Each export includes its own `rnbo/` runtime. The patcher `.cpp` must be compiled with its matching runtime — mixing versions causes build failures.
- **No code changes needed**: Stock RNBO exports compile as-is. The build script adds a thin C API wrapper automatically.
- **Multiple devices**: Place multiple devices in `devices/` — use the "Choose Device" menu to switch between them.
- **Parameter visibility**: Hidden parameters (e.g. polyphony sub-patcher params) are automatically filtered out.

## Architecture

The host and device communicate via a flat C API (`rnbo_device_api.h`) to avoid cross-shared-library C++ vtable issues:

```
Host (dsp.so)                    Device (device.so)
  plugin_api_v2          dlopen    rnbo_device_api.h (C API)
  rnbo_host.cpp  ──────────────>  rnbo_device_wrapper.cpp
  (Move plugin)          dlsym    RNBO::CoreObject (C++ internals)
```

The device wrapper uses `RNBO::CoreObject` to properly initialize the patcher with its Engine, then exports flat C functions for create/destroy/prepare/process/MIDI/parameters.

## File Structure

```
src/
  dsp/
    rnbo_host.cpp           # Host plugin (Move plugin_api_v2)
    rnbo_device_api.h       # Flat C API between host and device
    rnbo_device_wrapper.cpp # Included in each device build
  modules/
    rnbo-synth.json         # Module metadata
    rnbo-fx.json
    rnbo-midifx.json
scripts/
    build.sh                # Build host modules
    build-rnbo.sh           # Build a device from RNBO export
    install.sh              # Deploy to Move
    install-device.sh       # Deploy a single device
    Dockerfile              # ARM64 cross-compilation environment
```
