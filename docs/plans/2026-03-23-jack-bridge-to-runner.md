# JACK Bridge to RNBO Runner — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace direct .so loading in rnbo-synth/rnbo-fx with a JACK audio bridge to the RNBO Runner (rnbooscquery), so complex graphs like FM Splash work faithfully with full inter-patcher routing.

**Architecture:** The DSP module (dsp.so) becomes a JACK client that spawns an rnbooscquery process, connects to its JACK ports, and bridges audio/MIDI between JACK and the chain's render_block/on_midi calls via lock-free ring buffers. One Runner instance at a time; second slot shows error.

**Tech Stack:** C++ (rnbo_host.cpp), JACK client API (libjack), JACK ring buffer, OSC via fork+exec oscsend or raw UDP, rnbooscquery binary on device

---

## Background

### Current architecture (being replaced for Runner DB sets)

rnbo_host.cpp loads patcher .so files via dlopen through bridge.so, builds a DAG from set JSON, and processes audio/MIDI/messages in topological order. This works for simple graphs but fails for complex inter-patcher routing (e.g., FM Splash's cascading pad response from the "waves" patcher).

### New architecture

```
Chain → on_midi() → MIDI ring buffer → JACK callback → rnbooscquery MIDI in
                                                              ↓
Chain ← render_block() ← audio ring buffer ← JACK callback ← rnbooscquery audio out
```

For audio FX (rnbo-fx), also:
```
Chain → process_block() → audio_in ring buffer → JACK callback → rnbooscquery audio in
```

### Key constraints

- **One Runner instance at a time** — rnbooscquery has hardcoded OSC ports (1234 UDP, 5678 HTTP) and no config field for JACK client name. Second slot returns error via get_error().
- **Ring buffer bridge** — JACK callback (RT thread) and render_block (chain thread) run on same ~2.9ms cadence but not phase-locked. JACK's built-in jack_ringbuffer handles this.
- **Runner audio stays in chain** — auto_connect_audio=false prevents Runner from connecting to system:playback. Audio flows through our JACK client → ring buffer → render_block → chain FX → output.

### On-device paths

| What | Path |
|------|------|
| rnbooscquery | `/data/UserData/rnbo/bin/rnbooscquery` |
| jackd | `/data/UserData/rnbo/bin/jackd` |
| libjack.so | `/data/UserData/rnbo/lib/libjack.so` |
| JACK shadow driver | `/data/UserData/rnbo/lib/jack/jack_shadow.so` |
| Runner DB | `/data/UserData/Documents/rnbo/oscqueryrunner.sqlite` |
| Runner config | `/data/UserData/.config/rnbo/runner.json` |
| Runtime temp dir | `/data/UserData/tmp/` |

### JACK headers (for cross-compilation)

Headers: `schwung-jack/common/jack/` (jack.h, midiport.h, ringbuffer.h, types.h, transport.h)

### OSC API for set loading

- `/rnbo/inst/control/sets/load` with string arg (set name) — UDP port 1234
- CLI: `rnbooscquery -d` (delay-start: don't load anything until told via OSC)
- CLI: `rnbooscquery -q` (quiet: no stdout spam)
- CLI: `rnbooscquery -c FILE` (custom runner.json)

### Phase 2 (parameters — not in this plan)

After audio/MIDI works: query `/rnbo/inst/<n>/params` via OSCQuery HTTP (port 5678) to discover parameters, build chain_params/ui_hierarchy JSON, send parameter changes via OSC.

---

## Task 1: Add JACK headers to build and create rnbo_jack_bridge.h

**Files:**
- Create: `src/dsp/rnbo_jack_bridge.h` — JACK client wrapper + ring buffer bridge
- Modify: `scripts/build.sh` — add JACK include path and link against libjack

**Step 1: Copy JACK headers into the repo**

We need jack.h, types.h, midiport.h, ringbuffer.h, systemdeps.h, weakmacros.h, transport.h for compilation. Copy from schwung-jack:

```bash
mkdir -p /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-rnbo/jack-headers/jack
for h in jack.h types.h midiport.h ringbuffer.h systemdeps.h weakmacros.h transport.h weakjack.h; do
  cp /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-jack/common/jack/$h \
     /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-rnbo/jack-headers/jack/
done
```

**Step 2: Create rnbo_jack_bridge.h**

This is the core bridge: JACK client lifecycle, ring buffers, audio/MIDI transfer.

```cpp
// rnbo_jack_bridge.h — JACK client bridge for RNBO Runner
//
// Spawns rnbooscquery, registers as JACK client, bridges audio/MIDI
// between JACK and the chain's render_block/on_midi via ring buffers.

#ifndef RNBO_JACK_BRIDGE_H
#define RNBO_JACK_BRIDGE_H

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <atomic>

#define RNBO_BRIDGE_SAMPLE_RATE 44100
#define RNBO_BRIDGE_BLOCK_SIZE 128
#define RNBO_BRIDGE_RING_SLOTS 4
#define RNBO_BRIDGE_AUDIO_RING_SIZE (RNBO_BRIDGE_BLOCK_SIZE * sizeof(float) * RNBO_BRIDGE_RING_SLOTS)
#define RNBO_BRIDGE_MIDI_RING_SIZE 4096

#define RNBO_BIN_DIR "/data/UserData/rnbo/bin"
#define RNBO_LIB_DIR "/data/UserData/rnbo/lib"
#define RNBO_TMP_DIR "/data/UserData/tmp"
#define RNBO_RUNNER_CONFIG "/data/UserData/.config/rnbo/runner.json"
#define RNBO_OSC_PORT 1234

/* Global: only one Runner instance at a time */
static std::atomic<bool> g_runner_active{false};

struct RnboJackBridge {
    /* JACK client */
    jack_client_t *client;
    jack_port_t *audio_in_L;    /* receives Runner audio out L */
    jack_port_t *audio_in_R;    /* receives Runner audio out R */
    jack_port_t *audio_out_L;   /* sends chain audio to Runner (fx mode) */
    jack_port_t *audio_out_R;   /* sends chain audio to Runner (fx mode) */
    jack_port_t *midi_out;      /* sends chain MIDI to Runner */

    /* Ring buffers: JACK callback ↔ chain thread */
    jack_ringbuffer_t *rb_audio_from_runner_L;  /* Runner audio → render_block */
    jack_ringbuffer_t *rb_audio_from_runner_R;
    jack_ringbuffer_t *rb_audio_to_runner_L;    /* process_block → Runner (fx) */
    jack_ringbuffer_t *rb_audio_to_runner_R;
    jack_ringbuffer_t *rb_midi_to_runner;       /* on_midi → Runner */

    /* Processes */
    pid_t jackd_pid;
    pid_t runner_pid;

    /* State */
    bool is_effect;
    bool audio_flowing;
    std::string set_name;
    std::string error;

    /* Logging callback (from host API) */
    void (*log_fn)(const char *msg);
};

/* ---- Logging helper ---- */
static void bridge_log(RnboJackBridge *b, const char *fmt, ...) {
    if (!b || !b->log_fn) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    b->log_fn(buf);
}

/* ---- JACK process callback (RT thread) ---- */
static int jack_process_cb(jack_nframes_t nframes, void *arg) {
    RnboJackBridge *b = (RnboJackBridge *)arg;

    /* Read Runner's audio output → ring buffer for render_block */
    float *in_L = (float *)jack_port_get_buffer(b->audio_in_L, nframes);
    float *in_R = (float *)jack_port_get_buffer(b->audio_in_R, nframes);
    size_t bytes = nframes * sizeof(float);

    if (jack_ringbuffer_write_space(b->rb_audio_from_runner_L) >= bytes) {
        jack_ringbuffer_write(b->rb_audio_from_runner_L, (const char *)in_L, bytes);
        jack_ringbuffer_write(b->rb_audio_from_runner_R, (const char *)in_R, bytes);
    }

    /* Write chain audio to Runner (fx mode) */
    if (b->is_effect) {
        float *out_L = (float *)jack_port_get_buffer(b->audio_out_L, nframes);
        float *out_R = (float *)jack_port_get_buffer(b->audio_out_R, nframes);
        if (jack_ringbuffer_read_space(b->rb_audio_to_runner_L) >= bytes) {
            jack_ringbuffer_read(b->rb_audio_to_runner_L, (char *)out_L, bytes);
            jack_ringbuffer_read(b->rb_audio_to_runner_R, (char *)out_R, bytes);
        } else {
            memset(out_L, 0, bytes);
            memset(out_R, 0, bytes);
        }
    }

    /* Write pending MIDI to Runner */
    void *midi_buf = jack_port_get_buffer(b->midi_out, nframes);
    jack_midi_clear_buffer(midi_buf);
    /* MIDI ring stores: [len:1][data:3] per event */
    while (jack_ringbuffer_read_space(b->rb_midi_to_runner) >= 4) {
        uint8_t pkt[4];
        jack_ringbuffer_read(b->rb_midi_to_runner, (char *)pkt, 4);
        uint8_t len = pkt[0];
        if (len > 0 && len <= 3) {
            jack_midi_data_t *ev = jack_midi_event_reserve(midi_buf, 0, len);
            if (ev) memcpy(ev, pkt + 1, len);
        }
    }

    return 0;
}

/* ---- Write runner config JSON ---- */
static bool write_runner_config(RnboJackBridge *b) {
    const char *dir_cmd[] = { "mkdir", "-p", RNBO_TMP_DIR, NULL };
    /* Use system() for simplicity in header — plan calls for proper fork/exec */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", RNBO_TMP_DIR);
    system(cmd);

    char path[256];
    snprintf(path, sizeof(path), "%s/rnbo-chain-runner.json", RNBO_TMP_DIR);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f,
        "{\n"
        "    \"compile_cache_dir\": \"/data/UserData/Documents/rnbo/cache/so\",\n"
        "    \"control_auto_connect_midi\": false,\n"
        "    \"datafile_dir\": \"/data/UserData/Documents/rnbo/datafiles/\",\n"
        "    \"db_path\": \"/data/UserData/Documents/rnbo/oscqueryrunner.sqlite\",\n"
        "    \"instance_auto_connect_audio\": false,\n"
        "    \"instance_auto_connect_midi\": false,\n"
        "    \"instance_auto_connect_midi_hardware\": false,\n"
        "    \"instance_auto_start_last\": false,\n"
        "    \"instance_port_to_osc\": true,\n"
        "    \"save_dir\": \"/data/UserData/Documents/rnbo/saves/\",\n"
        "    \"source_cache_dir\": \"/data/UserData/Documents/rnbo/cache/src\"\n"
        "}\n");
    fclose(f);
    return true;
}

/* ---- Start JACK server ---- */
static pid_t start_jackd(RnboJackBridge *b) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: set up environment and exec jackd */
        char ld_path[512];
        snprintf(ld_path, sizeof(ld_path), "%s:%s",
            RNBO_LIB_DIR, getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
        setenv("LD_LIBRARY_PATH", ld_path, 1);

        /* Redirect stdout/stderr to log */
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        execl(RNBO_BIN_DIR "/jackd", "jackd", "-d", "shadow", NULL);
        _exit(127);
    }
    return pid;
}

/* ---- Start rnbooscquery ---- */
static pid_t start_runner(RnboJackBridge *b) {
    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/rnbo-chain-runner.json", RNBO_TMP_DIR);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char ld_path[512];
        snprintf(ld_path, sizeof(ld_path), "%s:%s",
            RNBO_LIB_DIR, getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
        setenv("LD_LIBRARY_PATH", ld_path, 1);
        setenv("HOME", "/data/UserData", 1);

        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        execl(RNBO_BIN_DIR "/rnbooscquery", "rnbooscquery",
              "-d", "-q", "-c", config_path, NULL);
        _exit(127);
    }
    return pid;
}

/* ---- Send OSC message to load a set ---- */
static bool send_osc_load_set(RnboJackBridge *b, const char *set_name) {
    /* Build raw OSC message: /rnbo/inst/control/sets/load ,s <name> */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RNBO_OSC_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* OSC address: /rnbo/inst/control/sets/load (pad to 4-byte boundary) */
    const char *osc_addr = "/rnbo/inst/control/sets/load";
    int addr_len = strlen(osc_addr) + 1;
    int addr_padded = (addr_len + 3) & ~3;

    /* OSC type tag: ,s (pad to 4-byte boundary) */
    const char *type_tag = ",s";
    int tag_len = strlen(type_tag) + 1;
    int tag_padded = (tag_len + 3) & ~3;

    /* OSC string arg (pad to 4-byte boundary) */
    int name_len = strlen(set_name) + 1;
    int name_padded = (name_len + 3) & ~3;

    int total = addr_padded + tag_padded + name_padded;
    char *buf = (char *)calloc(1, total);
    if (!buf) { close(sock); return false; }

    memcpy(buf, osc_addr, strlen(osc_addr));
    memcpy(buf + addr_padded, type_tag, strlen(type_tag));
    memcpy(buf + addr_padded + tag_padded, set_name, strlen(set_name));

    int sent = sendto(sock, buf, total, 0, (struct sockaddr *)&addr, sizeof(addr));
    free(buf);
    close(sock);

    bridge_log(b, "rnbo-jack: OSC load set '%s' (%d bytes sent)", set_name, sent);
    return sent == total;
}

/* ---- Wait for Runner JACK ports to appear ---- */
static bool wait_for_runner_ports(RnboJackBridge *b, int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        const char **ports = jack_get_ports(b->client, "rnbooscquery", NULL, JackPortIsOutput);
        if (ports) {
            jack_free(ports);
            return true;
        }
        usleep(100000); /* 100ms */
        elapsed += 100;
    }
    return false;
}

/* ---- Connect JACK ports ---- */
static bool connect_ports(RnboJackBridge *b) {
    /* Find Runner's audio output ports */
    const char **out_ports = jack_get_ports(b->client, "rnbooscquery", NULL, JackPortIsOutput | JackPortIsPhysical);
    if (!out_ports) {
        /* Try without physical flag */
        out_ports = jack_get_ports(b->client, "rnbooscquery", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
    }
    if (!out_ports) {
        bridge_log(b, "rnbo-jack: no Runner audio output ports found");
        return false;
    }

    /* Connect first two output ports to our inputs */
    if (out_ports[0]) {
        jack_connect(b->client, out_ports[0], jack_port_name(b->audio_in_L));
        bridge_log(b, "rnbo-jack: connected %s → audio_in_L", out_ports[0]);
    }
    if (out_ports[1]) {
        jack_connect(b->client, out_ports[1], jack_port_name(b->audio_in_R));
        bridge_log(b, "rnbo-jack: connected %s → audio_in_R", out_ports[1]);
    }
    jack_free(out_ports);

    /* Connect our MIDI output to Runner's MIDI input */
    const char **midi_ports = jack_get_ports(b->client, "rnbooscquery", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    if (midi_ports && midi_ports[0]) {
        jack_connect(b->client, jack_port_name(b->midi_out), midi_ports[0]);
        bridge_log(b, "rnbo-jack: connected midi_out → %s", midi_ports[0]);
        jack_free(midi_ports);
    }

    /* FX mode: connect our audio outputs to Runner's audio inputs */
    if (b->is_effect) {
        const char **in_ports = jack_get_ports(b->client, "rnbooscquery", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
        if (in_ports) {
            if (in_ports[0]) jack_connect(b->client, jack_port_name(b->audio_out_L), in_ports[0]);
            if (in_ports[1]) jack_connect(b->client, jack_port_name(b->audio_out_R), in_ports[1]);
            jack_free(in_ports);
        }
    }

    return true;
}

/* ---- Pin processes to cores 2-3 (same as overtake module) ---- */
static void pin_to_cores(RnboJackBridge *b) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "for p in $(pgrep -f jackd) $(pgrep rnbooscquery); do taskset -p 0xc $p 2>/dev/null; done");
    system(cmd);
}

/* ==== PUBLIC API ==== */

/* Create and start the JACK bridge. Returns nullptr on error (check b->error). */
static RnboJackBridge* rnbo_jack_bridge_create(const char *set_name, bool is_effect,
                                                 void (*log_fn)(const char *)) {
    /* Enforce single-instance */
    bool expected = false;
    if (!g_runner_active.compare_exchange_strong(expected, true)) {
        /* Already active — caller should check get_error */
        return nullptr;
    }

    auto *b = new RnboJackBridge();
    memset(b, 0, sizeof(RnboJackBridge));  /* zero POD fields */
    b->log_fn = log_fn;
    b->is_effect = is_effect;
    b->set_name = set_name;
    b->jackd_pid = -1;
    b->runner_pid = -1;

    /* Create ring buffers */
    b->rb_audio_from_runner_L = jack_ringbuffer_create(RNBO_BRIDGE_AUDIO_RING_SIZE);
    b->rb_audio_from_runner_R = jack_ringbuffer_create(RNBO_BRIDGE_AUDIO_RING_SIZE);
    b->rb_audio_to_runner_L = jack_ringbuffer_create(RNBO_BRIDGE_AUDIO_RING_SIZE);
    b->rb_audio_to_runner_R = jack_ringbuffer_create(RNBO_BRIDGE_AUDIO_RING_SIZE);
    b->rb_midi_to_runner = jack_ringbuffer_create(RNBO_BRIDGE_MIDI_RING_SIZE);

    /* Write runner config */
    if (!write_runner_config(b)) {
        b->error = "Failed to write runner config";
        bridge_log(b, "rnbo-jack: %s", b->error.c_str());
        return b;
    }

    /* Start JACK server */
    bridge_log(b, "rnbo-jack: starting jackd -d shadow");
    b->jackd_pid = start_jackd(b);
    if (b->jackd_pid < 0) {
        b->error = "Failed to start jackd";
        bridge_log(b, "rnbo-jack: %s", b->error.c_str());
        return b;
    }

    /* Wait for JACK to be ready, then register as client */
    usleep(500000); /* 500ms for jackd startup */
    jack_status_t status;
    int retries = 0;
    while (retries < 10) {
        /* Set LD_LIBRARY_PATH so libjack finds jackd */
        char ld_path[512];
        snprintf(ld_path, sizeof(ld_path), "%s:%s",
            RNBO_LIB_DIR, getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
        setenv("LD_LIBRARY_PATH", ld_path, 1);

        b->client = jack_client_open("rnbo-chain", JackNoStartServer, &status);
        if (b->client) break;
        usleep(200000);
        retries++;
    }
    if (!b->client) {
        b->error = "Failed to connect to JACK server";
        bridge_log(b, "rnbo-jack: %s (status=%d)", b->error.c_str(), status);
        return b;
    }
    bridge_log(b, "rnbo-jack: JACK client connected");

    /* Register ports */
    b->audio_in_L = jack_port_register(b->client, "audio_in_L",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    b->audio_in_R = jack_port_register(b->client, "audio_in_R",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    b->midi_out = jack_port_register(b->client, "midi_out",
        JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    if (is_effect) {
        b->audio_out_L = jack_port_register(b->client, "audio_out_L",
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        b->audio_out_R = jack_port_register(b->client, "audio_out_R",
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }

    /* Set process callback and activate */
    jack_set_process_callback(b->client, jack_process_cb, b);
    jack_activate(b->client);
    bridge_log(b, "rnbo-jack: JACK client activated");

    /* Start rnbooscquery */
    bridge_log(b, "rnbo-jack: starting rnbooscquery");
    b->runner_pid = start_runner(b);
    if (b->runner_pid < 0) {
        b->error = "Failed to start rnbooscquery";
        bridge_log(b, "rnbo-jack: %s", b->error.c_str());
        return b;
    }

    /* Pin to cores 2-3 */
    usleep(200000);
    pin_to_cores(b);

    /* Wait for Runner's JACK ports to appear */
    bridge_log(b, "rnbo-jack: waiting for Runner ports...");
    if (!wait_for_runner_ports(b, 10000)) {
        b->error = "Runner JACK ports not found (timeout)";
        bridge_log(b, "rnbo-jack: %s", b->error.c_str());
        return b;
    }

    /* Connect ports */
    if (!connect_ports(b)) {
        b->error = "Failed to connect JACK ports";
        bridge_log(b, "rnbo-jack: %s", b->error.c_str());
        return b;
    }

    /* Load the set via OSC */
    usleep(200000); /* let Runner finish initializing */
    if (!send_osc_load_set(b, set_name)) {
        b->error = "Failed to send OSC load command";
        bridge_log(b, "rnbo-jack: %s", b->error.c_str());
        return b;
    }

    /* Give Runner time to load the set and start producing audio */
    usleep(500000);
    b->audio_flowing = true;
    bridge_log(b, "rnbo-jack: bridge ready, set '%s' loaded", set_name);
    return b;
}

/* Destroy the bridge: kill processes, free resources */
static void rnbo_jack_bridge_destroy(RnboJackBridge *b) {
    if (!b) return;

    bridge_log(b, "rnbo-jack: shutting down bridge");

    /* Kill Runner first, then JACK */
    if (b->runner_pid > 0) {
        kill(b->runner_pid, SIGKILL);
        waitpid(b->runner_pid, NULL, 0);
    }
    /* Also killall in case of orphaned processes */
    system("killall -9 rnbooscquery 2>/dev/null");

    /* Deactivate and close JACK client */
    if (b->client) {
        jack_deactivate(b->client);
        jack_client_close(b->client);
    }

    /* Kill JACK server */
    if (b->jackd_pid > 0) {
        kill(b->jackd_pid, SIGKILL);
        waitpid(b->jackd_pid, NULL, 0);
    }
    system("killall -9 jackd 2>/dev/null");

    /* Free ring buffers */
    if (b->rb_audio_from_runner_L) jack_ringbuffer_free(b->rb_audio_from_runner_L);
    if (b->rb_audio_from_runner_R) jack_ringbuffer_free(b->rb_audio_from_runner_R);
    if (b->rb_audio_to_runner_L) jack_ringbuffer_free(b->rb_audio_to_runner_L);
    if (b->rb_audio_to_runner_R) jack_ringbuffer_free(b->rb_audio_to_runner_R);
    if (b->rb_midi_to_runner) jack_ringbuffer_free(b->rb_midi_to_runner);

    /* Clean up temp config */
    char path[256];
    snprintf(path, sizeof(path), "%s/rnbo-chain-runner.json", RNBO_TMP_DIR);
    unlink(path);

    /* Release global lock */
    g_runner_active.store(false);

    bridge_log(b, "rnbo-jack: bridge destroyed");
    delete b;
}

/* Read audio from Runner into int16 stereo interleaved output */
static void rnbo_jack_bridge_read_audio(RnboJackBridge *b, int16_t *out_lr, int frames) {
    size_t bytes = frames * sizeof(float);

    if (!b || !b->audio_flowing ||
        jack_ringbuffer_read_space(b->rb_audio_from_runner_L) < bytes) {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    float buf_L[RNBO_BRIDGE_BLOCK_SIZE];
    float buf_R[RNBO_BRIDGE_BLOCK_SIZE];
    jack_ringbuffer_read(b->rb_audio_from_runner_L, (char *)buf_L, bytes);
    jack_ringbuffer_read(b->rb_audio_from_runner_R, (char *)buf_R, bytes);

    for (int i = 0; i < frames; i++) {
        float l = buf_L[i];
        float r = buf_R[i];
        l = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        r = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
        out_lr[i * 2 + 0] = (int16_t)(l * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
}

/* Write chain audio to Runner (for FX mode) */
static void rnbo_jack_bridge_write_audio(RnboJackBridge *b, const int16_t *in_lr, int frames) {
    if (!b || !b->is_effect || !b->audio_flowing) return;

    float buf_L[RNBO_BRIDGE_BLOCK_SIZE];
    float buf_R[RNBO_BRIDGE_BLOCK_SIZE];
    for (int i = 0; i < frames; i++) {
        buf_L[i] = in_lr[i * 2 + 0] / 32768.0f;
        buf_R[i] = in_lr[i * 2 + 1] / 32768.0f;
    }

    size_t bytes = frames * sizeof(float);
    if (jack_ringbuffer_write_space(b->rb_audio_to_runner_L) >= bytes) {
        jack_ringbuffer_write(b->rb_audio_to_runner_L, (const char *)buf_L, bytes);
        jack_ringbuffer_write(b->rb_audio_to_runner_R, (const char *)buf_R, bytes);
    }
}

/* Send MIDI event to Runner */
static void rnbo_jack_bridge_send_midi(RnboJackBridge *b, const uint8_t *msg, int len) {
    if (!b || !b->audio_flowing || len < 1 || len > 3) return;

    /* Ring format: [len:1][d0:1][d1:1][d2:1] */
    uint8_t pkt[4] = {0};
    pkt[0] = (uint8_t)len;
    for (int i = 0; i < len; i++) pkt[1 + i] = msg[i];

    if (jack_ringbuffer_write_space(b->rb_midi_to_runner) >= 4)
        jack_ringbuffer_write(b->rb_midi_to_runner, (const char *)pkt, 4);
}

#endif /* RNBO_JACK_BRIDGE_H */
```

**Step 3: Update build.sh to link against libjack**

In `scripts/build.sh`, modify the host compilation to include JACK headers and link dynamically against libjack on device (use dlopen at runtime so the .so loads even when JACK isn't available):

Actually, since libjack is only on device and we cross-compile, we should use `dlopen`/`dlsym` for libjack at runtime rather than linking directly. But that adds complexity. Simpler approach: use weak linking or just link with `-ljack` and set `LD_LIBRARY_PATH` at runtime (the module is loaded with `LD_LIBRARY_PATH` already set by the host). For now, we'll use weak symbols via `jack/weakjack.h` which JACK provides for exactly this case.

Change the host compile command:

```bash
# Before:
${CROSS_PREFIX}g++ -g -O2 -fPIC -std=c++17 \
    -Isrc/dsp \
    -c src/dsp/rnbo_host.cpp \
    -o build/rnbo_host.o

# After:
${CROSS_PREFIX}g++ -g -O2 -fPIC -std=c++17 \
    -Isrc/dsp -Ijack-headers \
    -c src/dsp/rnbo_host.cpp \
    -o build/rnbo_host.o
```

No link-time change needed — `jack/weakjack.h` makes all JACK functions weak symbols resolved at runtime. The dsp.so only needs JACK available when the bridge is actually used.

**Step 4: Verify the build compiles**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-rnbo && ./scripts/build.sh
```

**Step 5: Commit**

```bash
git add jack-headers/ src/dsp/rnbo_jack_bridge.h scripts/build.sh
git commit -m "Add JACK bridge header and headers for RNBO Runner integration"
```

---

## Task 2: Integrate JACK bridge into rnbo_host.cpp

**Files:**
- Modify: `src/dsp/rnbo_host.cpp`

This task rewires the plugin API functions to use the JACK bridge when a Runner DB set is loaded (indicated by config_json containing `{"runner_set":"<name>"}`) while keeping the existing direct .so mode for rnbopack-loaded graphs.

**Step 1: Add bridge include and modify RnboInstance**

At the top of rnbo_host.cpp, after the existing includes:

```cpp
#include "rnbo_jack_bridge.h"
```

Add a bridge pointer to RnboInstance (find the struct around line ~600):

```cpp
// Add to RnboInstance:
RnboJackBridge *jack_bridge;  /* non-null = JACK bridge mode */
```

**Step 2: Modify rnbo_create to detect Runner set mode**

In `rnbo_create` (line 1247), after parsing config_json, add Runner set detection:

```cpp
/* Check config_json for a Runner DB set name */
std::string runner_set_name;
if (json_defaults && json_defaults[0]) {
    runner_set_name = json_get_string(json_defaults, "runner_set");
}

if (!runner_set_name.empty()) {
    /* JACK bridge mode: delegate to Runner */
    host_log("rnbo: JACK bridge mode, set='%s'", runner_set_name.c_str());
    inst->jack_bridge = rnbo_jack_bridge_create(
        runner_set_name.c_str(), false /* is_effect */, g_host->log);
    if (!inst->jack_bridge) {
        inst->error = "RNBO Runner already in use by another slot";
        host_log("rnbo: %s", inst->error.c_str());
    } else if (!inst->jack_bridge->error.empty()) {
        inst->error = inst->jack_bridge->error;
    }
    return inst;
}
```

This early-returns before any .so loading when in bridge mode.

**Step 3: Modify rnbo_destroy for bridge cleanup**

In `rnbo_destroy` (line 1342), add before existing cleanup:

```cpp
if (inst->jack_bridge) {
    rnbo_jack_bridge_destroy(inst->jack_bridge);
    inst->jack_bridge = nullptr;
    delete inst;
    return;
}
```

**Step 4: Modify rnbo_render_block for bridge mode**

In `rnbo_render_block` (line 1441):

```cpp
static void rnbo_render_block(void *instance, int16_t *out_lr, int frames) {
    auto *inst = (RnboInstance*)instance;
    if (!inst) { memset(out_lr, 0, frames * 2 * sizeof(int16_t)); return; }

    /* JACK bridge mode */
    if (inst->jack_bridge) {
        rnbo_jack_bridge_read_audio(inst->jack_bridge, out_lr, frames);
        /* Apply volume */
        float vol = inst->volume;
        if (vol < 0.99f) {
            for (int i = 0; i < frames * 2; i++) {
                out_lr[i] = (int16_t)(out_lr[i] * vol);
            }
        }
        return;
    }

    /* Existing graph/single-device mode... */
    if (inst->graph_mode) {
        process_graph(inst, out_lr, frames);
    } else {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
    }
}
```

**Step 5: Modify rnbo_on_midi for bridge mode**

In `rnbo_on_midi` (line 1355):

```cpp
static void rnbo_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    auto *inst = (RnboInstance*)instance;
    if (!inst || len < 1) return;

    /* JACK bridge mode */
    if (inst->jack_bridge) {
        rnbo_jack_bridge_send_midi(inst->jack_bridge, msg, len);
        return;
    }

    /* Existing graph mode... */
    rnbo_midi_event_t m;
    m.len = (len > 3) ? 3 : len;
    m.data[0] = msg[0];
    m.data[1] = (len >= 2) ? msg[1] : 0;
    m.data[2] = (len >= 3) ? msg[2] : 0;
    inst->pending_midi.push_back(m);
}
```

**Step 6: Modify rnbo_get_error for bridge mode**

In `rnbo_get_error` (line 1433):

```cpp
static int rnbo_get_error(void *instance, char *buf, int buf_len) {
    auto *inst = (RnboInstance*)instance;
    if (!inst) return 0;

    if (!inst->error.empty())
        return snprintf(buf, buf_len, "%s", inst->error.c_str());

    if (inst->graph.nodes.empty() && inst->devices.empty() && !inst->jack_bridge)
        return snprintf(buf, buf_len, "No RNBO devices found");
    return 0;
}
```

**Step 7: Handle FX mode in process_block**

The `process_block` path for audio FX (used by rnbo-fx) needs to write chain audio to Runner and read processed audio back. Since both APIs use the same `rnbo_render_block` function, and rnbo-fx calls it as `process_block(instance, audio_inout, frames)`, the audio_inout buffer contains input audio AND expects output audio:

```cpp
/* In rnbo_render_block, JACK bridge mode: */
if (inst->jack_bridge) {
    if (inst->jack_bridge->is_effect) {
        /* Write input audio to Runner, then read processed audio */
        rnbo_jack_bridge_write_audio(inst->jack_bridge, out_lr, frames);
    }
    rnbo_jack_bridge_read_audio(inst->jack_bridge, out_lr, frames);
    /* Apply volume */
    float vol = inst->volume;
    if (vol < 0.99f) {
        for (int i = 0; i < frames * 2; i++)
            out_lr[i] = (int16_t)(out_lr[i] * vol);
    }
    return;
}
```

**Step 8: Add error field to RnboInstance**

Find the RnboInstance struct and add:

```cpp
std::string error;  /* Error message for get_error */
```

**Step 9: Build and verify compilation**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-rnbo && ./scripts/build.sh
```

**Step 10: Commit**

```bash
git add src/dsp/rnbo_host.cpp
git commit -m "Integrate JACK bridge into rnbo_host.cpp for Runner DB sets"
```

---

## Task 3: Wire up scan_packs to generate Runner DB entries with runner_set config

**Files:**
- Modify: schwung repo: `src/host/module_manager.c` (if DB import plan is implemented)
- OR: Generate test config_json manually for first test

This task ensures the chain passes `{"runner_set":"FM Splash"}` when the user selects a Runner DB set from the menu. This depends on the DB import plan (Task 1-5 in `2026-03-23-rnbo-runner-db-import.md`). If that's not yet implemented, we can test with a hardcoded config.

**Step 1: For testing — create a test script**

Create a test script that manually triggers the bridge with a known set name:

```bash
# scripts/test-bridge.sh
#!/bin/bash
# Test the JACK bridge by loading a known Runner set
# Usage: ./scripts/test-bridge.sh "FM Splash"
SET_NAME="${1:-FM Splash}"
echo "Testing JACK bridge with set: $SET_NAME"
echo "Config JSON: {\"runner_set\":\"$SET_NAME\"}"
echo ""
echo "To test: load rnbo-synth in chain with this config."
echo "The host will detect runner_set and use the JACK bridge."
```

**Step 2: Modify scan_packs entry generation (in schwung DB import)**

When the DB import plan generates pack directories, the `config_json` passed to create_instance needs to include `"runner_set"` instead of (or in addition to) `"pack"`. In the schwung module_manager.c `export_rnbo_runner_sets()` function, the menu entry should encode:

```json
{"runner_set": "FM Splash"}
```

This tells rnbo_host.cpp to use the JACK bridge instead of direct .so loading.

**Step 3: Commit**

```bash
git commit -m "Add runner_set config_json key for JACK bridge mode"
```

---

## Task 4: Deploy and test on device

**Step 1: Build**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-rnbo && ./scripts/build.sh
```

**Step 2: Install to device**

```bash
./scripts/install.sh
```

**Step 3: Test basic audio flow**

1. Reboot device (shim creates /dev/shm/schwung_jack)
2. Load rnbo-synth in chain
3. Select a Runner DB set (e.g., "FM Splash" or "Granulator III")
4. Play pads — audio should come through the chain
5. Verify: audio plays through chain FX (add a reverb after the synth)

**Step 4: Test MIDI flow**

1. Play pads — notes should trigger the Runner's synth
2. Add a MIDI FX (arp/chord) before the synth — verify it affects the Runner

**Step 5: Test error case**

1. Load rnbo-synth in slot 1
2. Try to load rnbo-synth in slot 2
3. Verify: error message displayed ("RNBO Runner already in use by another slot")

**Step 6: Test cleanup**

1. Load rnbo-synth, verify audio
2. Switch to a different synth (e.g., Braids)
3. Verify: Runner processes killed, JACK stopped
4. Load rnbo-synth again — should work

**Step 7: Commit any fixes**

```bash
git commit -m "Fix issues found during on-device testing"
```

---

## Task 5: Handle edge cases and robustness

**Files:**
- Modify: `src/dsp/rnbo_jack_bridge.h`
- Modify: `src/dsp/rnbo_host.cpp`

**Step 1: Handle JACK already running**

If the overtake module was recently used, JACK might still be running. Check in `start_jackd`:

```cpp
/* Check if jackd is already running */
FILE *p = popen("pgrep -f jackd", "r");
if (p) {
    char line[64];
    if (fgets(line, sizeof(line), p)) {
        pclose(p);
        bridge_log(b, "rnbo-jack: jackd already running, reusing");
        return 0; /* signal: don't kill on cleanup */
    }
    pclose(p);
}
```

**Step 2: Handle Runner process crash**

In render_block, check if the Runner process is still alive:

```cpp
if (b->runner_pid > 0 && waitpid(b->runner_pid, NULL, WNOHANG) != 0) {
    b->audio_flowing = false;
    b->error = "RNBO Runner process died";
}
```

**Step 3: Handle JACK connection loss**

Register a JACK shutdown callback:

```cpp
static void jack_shutdown_cb(void *arg) {
    RnboJackBridge *b = (RnboJackBridge *)arg;
    b->audio_flowing = false;
    b->client = nullptr; /* don't try to close */
}
// In create: jack_on_shutdown(b->client, jack_shutdown_cb, b);
```

**Step 4: Clean up stale processes on create**

Before starting anything, kill any orphaned processes from a previous crash:

```cpp
system("killall -9 rnbooscquery 2>/dev/null");
system("killall -9 jackd 2>/dev/null");
usleep(200000);
```

**Step 5: Build and test**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-rnbo && ./scripts/build.sh && ./scripts/install.sh
```

**Step 6: Commit**

```bash
git commit -m "Add robustness handling for JACK bridge edge cases"
```

---

## Notes

- **Latency**: One JACK buffer (128 frames = 2.9ms) added by the ring buffer bridge. Same as what the overtake module already has. Imperceptible.
- **CPU**: Running rnbooscquery + jackd alongside the schwung host. The overtake module proves this works on Move's hardware (pinned to cores 2-3).
- **Existing .so mode preserved**: The JACK bridge only activates when `config_json` contains `"runner_set"`. Existing rnbopack-based loading via `"pack"` key continues to work unchanged.
- **Phase 2 (parameters)**: After this plan, add OSCQuery parameter discovery. Query `http://localhost:5678/rnbo/inst/0/params` as JSON, parse parameter names/ranges, build chain_params/ui_hierarchy, route set_param/get_param via OSC.
- **rnbo-fx support**: The bridge handles bidirectional audio. When creating for FX mode, pass `is_effect=true` to `rnbo_jack_bridge_create`. The chain detects effect sets via the Runner DB's audio_inputs column.
- **weakjack.h**: JACK functions are resolved at runtime via weak symbols. This means dsp.so loads even if libjack isn't available (e.g., on a system without RNBO Runner installed). The bridge code checks `jack_client_open != NULL` before proceeding.
