# RNBO for Schwung

Run [RNBO](https://rnbo.cycling74.com/) on Ableton Move via [Schwung](https://github.com/charlesvestal/schwung). Two modes:

- **RNBO Runner** (overtake module) — full RNBO Runner experience with Move's pads, knobs, display, and LEDs
- **Signal Chain** — load RNBO synths and effects as chain modules alongside other Schwung modules

## RNBO Runner (Overtake Module)

Takes over Move completely — rnbomovecontrol drives the display, LEDs, and MIDI while RNBO patchers handle audio via JACK.

### Prerequisites

1. [Schwung](https://github.com/charlesvestal/schwung) installed on Move
2. [RNBO Runner](https://rnbo.cycling74.com/explore/ableton-move) installed on Move (Cycling '74's official installer)
3. [schwung-jack](https://github.com/charlesvestal/schwung-jack) shadow driver

### Installation

```bash
# 1. Build and install the JACK shadow driver
cd schwung-jack
./scripts/build.sh
scp build/jack_shadow.so ableton@move.local:/data/UserData/rnbo/lib/jack/

# 2. Build and install the RNBO modules + display_ctl
cd schwung-rnbo
./scripts/build.sh
./scripts/install.sh
```

After installation, **RNBO Runner** appears in Schwung's overtake menu. The first launch takes ~9 seconds (JACK + rnbomovecontrol startup). Subsequent graph changes are instant.

### How it works

```
Schwung (overtake mode)
  └─ ui.js launches rnbomovecontrol
       ├─ Starts JACK with shadow driver (jack_shadow.so)
       │    └─ Communicates with Schwung shim via /dev/shm/schwung_jack
       │         ├─ Audio: mixed into Move output (synchronous wake+wait)
       │         ├─ MIDI: routed to/from Move hardware (sysex LEDs, pad input)
       │         └─ Display: overrides Move's screen
       ├─ Starts rnbooscquery (graph/preset management)
       └─ Starts rnbo-runner-panel (web UI at port 3000)
```

---

## Signal Chain Modules

Load RNBO synths and effects into Schwung's signal chain alongside other modules.

## Adding Packs

Drop `.rnbopack` files into the appropriate module directory on the device:

| Type | Directory on Move |
|------|------------------|
| **Synths** | `.../modules/sound_generators/rnbo-synth/packs/` |
| **Audio FX** | `.../modules/audio_fx/rnbo-fx/packs/` |

```bash
# Copy a synth pack
scp my-synth.rnbopack ableton@move.local:/data/UserData/move-anything/modules/sound_generators/rnbo-synth/packs/

# Copy an effect pack
scp my-effect.rnbopack ableton@move.local:/data/UserData/move-anything/modules/audio_fx/rnbo-fx/packs/
```

The host extracts and loads packs automatically on startup. No compilation or install scripts needed.

## How It Works

Each `.rnbopack` contains a complete RNBO graph — multiple interconnected patchers with pre-compiled ARM64 binaries, a set JSON defining the wiring, and parameter views.

The host:
1. Extracts the `.rnbopack` tar to a cache directory
2. Loads each patcher via a bridge `.so` (handles RNBO SDK version matching)
3. Wires patchers together per the set JSON (audio, MIDI, and message routing)
4. Processes all patchers in topological order each audio block
5. Exposes parameters to Move's knobs via the set's views

```
.rnbopack file
    → host extracts to cache/
    → loads patcher .so files via bridge
    → parses set JSON for graph wiring
    → processes: Synth → Delay → Reverb → Limiter → output
```

## Creating Packs

1. Build your RNBO graph in Max (use the Beta for RNBO 1.4.3)
2. Deploy to the Move RNBO runner target
3. Export the `.rnbopack` from the runner
4. Copy to the appropriate module directory on Move

**Synth graphs**: Generate audio, receive MIDI from pads. Audio output connects to `move-volume`.

**Effect graphs**: Process audio from the chain. Audio input from `system:capture`, output to `move-volume`.

## Building the Host

Only needed once (or when updating host code):

```bash
# Requires Docker and RNBO SDK 1.4.3 in rnbo-sdk/
./scripts/build.sh
./scripts/install.sh
```

## Architecture

```
Host (dsp.so)              Bridge (device.so)           Patcher (patcher.so)
  graph orchestrator  →      RNBO CoreObject wrapper  →   pre-compiled from rnbopack
  flat C API consumer        float↔double conversion      GetPatcherFactoryFunction
  no RNBO dependency         RNBO SDK 1.4.3               RNBO 1.4.3 binary
```

The bridge isolates RNBO SDK ABI — the host never touches RNBO C++ types directly. This allows the host to work with any RNBO version as long as the bridge is compiled with a matching SDK.
