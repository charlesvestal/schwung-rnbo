// RNBO JACK Bridge — connects chain DSP to RNBO Runner via JACK
//
// This header provides a self-contained bridge that:
// 1. Starts jackd with the shadow driver
// 2. Starts rnbooscquery (RNBO Runner)
// 3. Registers a JACK client that receives Runner audio output via ring buffers
// 4. Provides audio/MIDI transfer functions for render_block() integration
//
// Uses weak JACK symbols so dsp.so loads even without libjack present.

#ifndef RNBO_JACK_BRIDGE_H
#define RNBO_JACK_BRIDGE_H

#include <jack/weakjack.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sched.h>
#include <stdarg.h>

#include <atomic>

// ---- Constants ----

#define RNBO_BIN_DIR   "/data/UserData/rnbo/bin"
#define RNBO_LIB_DIR   "/data/UserData/rnbo/lib"
#define RNBO_TMP_DIR   "/data/UserData/tmp"
#define RNBO_OSC_PORT  1234

#define RNBO_BLOCK_SIZE   128
#define RNBO_SAMPLE_RATE  44100

// Ring buffer sizing: 4 slots of 128 frames for audio, 4096 bytes for MIDI
#define RNBO_RB_AUDIO_SLOTS  4
#define RNBO_RB_AUDIO_BYTES  (RNBO_RB_AUDIO_SLOTS * RNBO_BLOCK_SIZE * sizeof(float))
#define RNBO_RB_MIDI_BYTES   4096

// Timeouts
#define RNBO_JACKD_START_TIMEOUT_US   3000000   // 3 seconds
#define RNBO_RUNNER_PORT_TIMEOUT_US   10000000  // 10 seconds
#define RNBO_POLL_INTERVAL_US         100000    // 100ms

#define RNBO_CLIENT_NAME  "rnbo-chain"
#define RNBO_RUNNER_PORT_PREFIX  "rnbooscquery"

// ---- Logging ----

typedef void (*rnbo_log_fn)(const char *msg);

// ---- Bridge struct ----

typedef struct RnboJackBridge {
    // JACK client
    jack_client_t *client;

    // JACK ports
    jack_port_t *audio_in_L;     // receives Runner audio output left
    jack_port_t *audio_in_R;     // receives Runner audio output right
    jack_port_t *audio_out_L;    // sends chain audio to Runner (fx mode)
    jack_port_t *audio_out_R;    // sends chain audio to Runner (fx mode)
    jack_port_t *midi_out;       // sends chain MIDI to Runner

    // Ring buffers (JACK RT thread <-> render_block thread)
    jack_ringbuffer_t *rb_audio_from_runner_L;
    jack_ringbuffer_t *rb_audio_from_runner_R;
    jack_ringbuffer_t *rb_audio_to_runner_L;   // fx mode only
    jack_ringbuffer_t *rb_audio_to_runner_R;   // fx mode only
    jack_ringbuffer_t *rb_midi_to_runner;

    // Process PIDs
    pid_t jackd_pid;
    pid_t runner_pid;

    // State
    int is_effect;
    std::atomic<int> audio_flowing;
    char set_name[256];
    char error[512];

    // Logging
    rnbo_log_fn log_fn;
} RnboJackBridge;

// ---- Single-instance guard ----

static std::atomic<bool> g_rnbo_jack_bridge_active{false};

// ---- Internal helpers ----

static void rnbo_bridge_log(RnboJackBridge *b, const char *fmt, ...) {
    if (!b || !b->log_fn) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    b->log_fn(buf);
}

static void rnbo_bridge_set_error(RnboJackBridge *b, const char *fmt, ...) {
    if (!b) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->error, sizeof(b->error), fmt, ap);
    va_end(ap);
    rnbo_bridge_log(b, "ERROR: %s", b->error);
}

// ---- JACK process callback (RT thread) ----

static int rnbo_jack_process_callback(jack_nframes_t nframes, void *arg) {
    RnboJackBridge *b = (RnboJackBridge *)arg;
    if (!b) return 0;

    // Read Runner audio output -> write to ring buffers
    float *in_L = (float *)jack_port_get_buffer(b->audio_in_L, nframes);
    float *in_R = (float *)jack_port_get_buffer(b->audio_in_R, nframes);

    if (in_L && in_R) {
        size_t bytes = nframes * sizeof(float);
        if (jack_ringbuffer_write_space(b->rb_audio_from_runner_L) >= bytes) {
            jack_ringbuffer_write(b->rb_audio_from_runner_L, (const char *)in_L, bytes);
            jack_ringbuffer_write(b->rb_audio_from_runner_R, (const char *)in_R, bytes);
        }
        // Check if we're getting non-silent audio
        for (jack_nframes_t i = 0; i < nframes; i++) {
            if (in_L[i] != 0.0f || in_R[i] != 0.0f) {
                b->audio_flowing = 1;
                break;
            }
        }
    }

    // Read MIDI ring buffer -> write to JACK MIDI output port
    void *midi_buf = jack_port_get_buffer(b->midi_out, nframes);
    if (midi_buf) {
        jack_midi_clear_buffer(midi_buf);

        while (jack_ringbuffer_read_space(b->rb_midi_to_runner) >= 1) {
            // Format: [len:1][data:len]
            uint8_t len;
            if (jack_ringbuffer_peek(b->rb_midi_to_runner, (char *)&len, 1) != 1)
                break;
            if (len == 0 || len > 16) {
                // Invalid, skip byte
                jack_ringbuffer_read_advance(b->rb_midi_to_runner, 1);
                continue;
            }
            if (jack_ringbuffer_read_space(b->rb_midi_to_runner) < (size_t)(1 + len))
                break;

            // Consume length byte
            jack_ringbuffer_read_advance(b->rb_midi_to_runner, 1);

            // Read MIDI data
            uint8_t midi_data[16];
            jack_ringbuffer_read(b->rb_midi_to_runner, (char *)midi_data, len);

            jack_midi_event_write(midi_buf, 0, midi_data, len);
        }
    }

    // For fx mode: read audio_to_runner ring buffer -> write to JACK audio output ports
    if (b->is_effect) {
        float *out_L = (float *)jack_port_get_buffer(b->audio_out_L, nframes);
        float *out_R = (float *)jack_port_get_buffer(b->audio_out_R, nframes);

        if (out_L && out_R) {
            size_t bytes = nframes * sizeof(float);
            if (jack_ringbuffer_read_space(b->rb_audio_to_runner_L) >= bytes) {
                jack_ringbuffer_read(b->rb_audio_to_runner_L, (char *)out_L, bytes);
                jack_ringbuffer_read(b->rb_audio_to_runner_R, (char *)out_R, bytes);
            } else {
                memset(out_L, 0, bytes);
                memset(out_R, 0, bytes);
            }
        }
    }

    return 0;
}

// ---- Helper: write Runner config JSON ----

static int write_runner_config(RnboJackBridge *b) {
    // Ensure tmp dir exists
    mkdir(RNBO_TMP_DIR, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/rnbo-chain-runner.json", RNBO_TMP_DIR);

    FILE *f = fopen(path, "w");
    if (!f) {
        rnbo_bridge_set_error(b, "Failed to write runner config to %s", path);
        return -1;
    }

    fprintf(f,
        "{\n"
        "  \"audio\": {\n"
        "    \"auto_connect\": false\n"
        "  },\n"
        "  \"patcher\": {\n"
        "    \"auto_start\": false\n"
        "  },\n"
        "  \"osc\": {\n"
        "    \"port\": %d\n"
        "  }\n"
        "}\n",
        RNBO_OSC_PORT);

    fclose(f);
    rnbo_bridge_log(b, "Wrote runner config to %s", path);
    return 0;
}

// ---- Helper: start jackd ----

static pid_t start_jackd(RnboJackBridge *b) {
    /* Check if jackd is already running */
    FILE *p = popen("pgrep -x jackd", "r");
    if (p) {
        char line[64];
        if (fgets(line, sizeof(line), p)) {
            pclose(p);
            rnbo_bridge_log(b, "rnbo-jack: jackd already running, reusing");
            return 0;  /* Return 0 to signal: don't kill on cleanup */
        }
        pclose(p);
    }

    pid_t pid = fork();
    if (pid < 0) {
        rnbo_bridge_set_error(b, "fork() failed for jackd: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child: set up environment and exec jackd
        char ld_path[512];
        snprintf(ld_path, sizeof(ld_path), "%s:%s",
                 RNBO_LIB_DIR,
                 getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
        setenv("LD_LIBRARY_PATH", ld_path, 1);

        char jackd_path[512];
        snprintf(jackd_path, sizeof(jackd_path), "%s/jackd", RNBO_BIN_DIR);

        char rate_str[16], period_str[16];
        snprintf(rate_str, sizeof(rate_str), "%d", RNBO_SAMPLE_RATE);
        snprintf(period_str, sizeof(period_str), "%d", RNBO_BLOCK_SIZE);

        // Redirect output
        freopen("/tmp/rnbo-chain-jackd.log", "w", stdout);
        freopen("/tmp/rnbo-chain-jackd.log", "a", stderr);

        execl(jackd_path, "jackd",
              "-R",                    // realtime
              "-d", "shadow",          // shadow driver
              "-r", rate_str,
              "-p", period_str,
              (char *)NULL);

        _exit(127);
    }

    rnbo_bridge_log(b, "Started jackd pid=%d", pid);
    return pid;
}

// ---- Helper: start Runner (rnbooscquery) ----

static pid_t start_runner(RnboJackBridge *b) {
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/rnbo-chain-runner.json", RNBO_TMP_DIR);

    pid_t pid = fork();
    if (pid < 0) {
        rnbo_bridge_set_error(b, "fork() failed for runner: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child
        char ld_path[512];
        snprintf(ld_path, sizeof(ld_path), "%s:%s",
                 RNBO_LIB_DIR,
                 getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
        setenv("LD_LIBRARY_PATH", ld_path, 1);
        setenv("HOME", "/data/UserData", 1);

        char runner_path[512];
        snprintf(runner_path, sizeof(runner_path), "%s/rnbooscquery", RNBO_BIN_DIR);

        // Redirect output
        freopen("/tmp/rnbo-chain-runner.log", "w", stdout);
        freopen("/tmp/rnbo-chain-runner.log", "a", stderr);

        execl(runner_path, "rnbooscquery",
              "-d",                    // daemonize
              "-q",                    // quiet
              "-c", config_path,       // config file
              (char *)NULL);

        _exit(127);
    }

    rnbo_bridge_log(b, "Started rnbooscquery pid=%d", pid);
    return pid;
}

// ---- Helper: wait for Runner JACK ports ----

static int wait_for_runner_ports(RnboJackBridge *b) {
    int elapsed = 0;

    while (elapsed < RNBO_RUNNER_PORT_TIMEOUT_US) {
        const char **ports = jack_get_ports(b->client, RNBO_RUNNER_PORT_PREFIX, NULL, 0);
        if (ports) {
            if (ports[0] != NULL) {
                jack_free(ports);
                rnbo_bridge_log(b, "Runner ports found after %dms", elapsed / 1000);
                return 0;
            }
            jack_free(ports);
        }
        usleep(RNBO_POLL_INTERVAL_US);
        elapsed += RNBO_POLL_INTERVAL_US;
    }

    rnbo_bridge_set_error(b, "Timeout waiting for Runner JACK ports");
    return -1;
}

// ---- Helper: connect JACK ports ----

static int connect_ports(RnboJackBridge *b) {
    int err = 0;
    const char *my_name = jack_get_client_name(b->client);

    // Find Runner output audio ports and connect to our inputs
    const char **runner_out = jack_get_ports(b->client, RNBO_RUNNER_PORT_PREFIX,
                                              JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
    if (runner_out) {
        char my_in_L[256], my_in_R[256];
        snprintf(my_in_L, sizeof(my_in_L), "%s:audio_in_L", my_name);
        snprintf(my_in_R, sizeof(my_in_R), "%s:audio_in_R", my_name);

        if (runner_out[0]) {
            err = jack_connect(b->client, runner_out[0], my_in_L);
            if (err && err != EEXIST)
                rnbo_bridge_log(b, "Warning: connect runner out L -> in L failed: %d", err);
        }
        if (runner_out[1]) {
            err = jack_connect(b->client, runner_out[1], my_in_R);
            if (err && err != EEXIST)
                rnbo_bridge_log(b, "Warning: connect runner out R -> in R failed: %d", err);
        }
        jack_free(runner_out);
    } else {
        rnbo_bridge_log(b, "Warning: no Runner audio output ports found");
    }

    // Connect our MIDI out to Runner MIDI input
    const char **runner_midi_in = jack_get_ports(b->client, RNBO_RUNNER_PORT_PREFIX,
                                                  JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
    if (runner_midi_in) {
        char my_midi[256];
        snprintf(my_midi, sizeof(my_midi), "%s:midi_out", my_name);

        if (runner_midi_in[0]) {
            err = jack_connect(b->client, my_midi, runner_midi_in[0]);
            if (err && err != EEXIST)
                rnbo_bridge_log(b, "Warning: connect midi out -> runner midi in failed: %d", err);
        }
        jack_free(runner_midi_in);
    }

    // For fx mode: connect our audio outputs to Runner audio inputs
    if (b->is_effect) {
        const char **runner_in = jack_get_ports(b->client, RNBO_RUNNER_PORT_PREFIX,
                                                 JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
        if (runner_in) {
            char my_out_L[256], my_out_R[256];
            snprintf(my_out_L, sizeof(my_out_L), "%s:audio_out_L", my_name);
            snprintf(my_out_R, sizeof(my_out_R), "%s:audio_out_R", my_name);

            if (runner_in[0]) {
                err = jack_connect(b->client, my_out_L, runner_in[0]);
                if (err && err != EEXIST)
                    rnbo_bridge_log(b, "Warning: connect out L -> runner in L failed: %d", err);
            }
            if (runner_in[1]) {
                err = jack_connect(b->client, my_out_R, runner_in[1]);
                if (err && err != EEXIST)
                    rnbo_bridge_log(b, "Warning: connect out R -> runner in R failed: %d", err);
            }
            jack_free(runner_in);
        }
    }

    rnbo_bridge_log(b, "JACK ports connected");
    return 0;
}

// ---- Helper: pin processes to cores 2-3 ----

static void pin_to_cores(RnboJackBridge *b) {
    // Use taskset via system() — simpler than sched_setaffinity across pids
    char cmd[512];
    if (b->jackd_pid > 0) {
        snprintf(cmd, sizeof(cmd), "taskset -p 0xc %d 2>/dev/null", b->jackd_pid);
        system(cmd);
    }
    if (b->runner_pid > 0) {
        snprintf(cmd, sizeof(cmd), "taskset -p 0xc %d 2>/dev/null", b->runner_pid);
        system(cmd);
    }
    rnbo_bridge_log(b, "Pinned jackd+runner to cores 2-3");
}

// ---- Helper: send OSC to load a set ----

static int send_osc_load_set(RnboJackBridge *b, const char *set_name) {
    // Build raw OSC message for /rnbo/inst/control/sets/load
    // OSC format: address\0 (padded to 4), type tag ,s\0 (padded to 4), string arg (padded to 4)
    uint8_t packet[512];
    int pos = 0;

    // Address: /rnbo/inst/control/sets/load
    const char *addr = "/rnbo/inst/control/sets/load";
    int addr_len = strlen(addr) + 1;
    int addr_padded = (addr_len + 3) & ~3;
    memcpy(packet + pos, addr, addr_len);
    memset(packet + pos + addr_len, 0, addr_padded - addr_len);
    pos += addr_padded;

    // Type tag: ,s
    const char *tag = ",s";
    int tag_len = strlen(tag) + 1;
    int tag_padded = (tag_len + 3) & ~3;
    memcpy(packet + pos, tag, tag_len);
    memset(packet + pos + tag_len, 0, tag_padded - tag_len);
    pos += tag_padded;

    // String argument: set_name
    int name_len = strlen(set_name) + 1;
    int name_padded = (name_len + 3) & ~3;
    if (pos + name_padded > (int)sizeof(packet)) {
        rnbo_bridge_set_error(b, "OSC packet too large for set name '%s'", set_name);
        return -1;
    }
    memcpy(packet + pos, set_name, name_len);
    memset(packet + pos + name_len, 0, name_padded - name_len);
    pos += name_padded;

    // Send UDP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        rnbo_bridge_set_error(b, "socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(RNBO_OSC_PORT);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);

    ssize_t sent = sendto(sock, packet, pos, 0,
                           (struct sockaddr *)&dest, sizeof(dest));
    close(sock);

    if (sent != pos) {
        rnbo_bridge_set_error(b, "OSC sendto failed: %s", strerror(errno));
        return -1;
    }

    rnbo_bridge_log(b, "Sent OSC load set '%s' (%d bytes)", set_name, pos);
    return 0;
}

// ---- JACK shutdown callback ----

static void rnbo_jack_shutdown_cb(void *arg) {
    RnboJackBridge *b = (RnboJackBridge *)arg;
    b->audio_flowing = false;
    b->client = NULL;  /* Don't try to close */
}

// ---- Create bridge ----

static RnboJackBridge *rnbo_jack_bridge_create(const char *set_name,
                                                int is_effect,
                                                rnbo_log_fn log_fn) {
    // Check for JACK availability (weak symbols)
    if (jack_client_open == NULL) {
        if (log_fn) log_fn("JACK not available (weak symbol null)");
        return NULL;
    }

    // Enforce single-instance
    bool expected = false;
    if (!g_rnbo_jack_bridge_active.compare_exchange_strong(expected, true)) {
        if (log_fn) log_fn("JACK bridge already active (single-instance guard)");
        return NULL;
    }

    RnboJackBridge *b = (RnboJackBridge *)calloc(1, sizeof(RnboJackBridge));
    if (!b) {
        g_rnbo_jack_bridge_active.store(false);
        return NULL;
    }

    b->log_fn = log_fn;
    b->is_effect = is_effect;
    snprintf(b->set_name, sizeof(b->set_name), "%s", set_name ? set_name : "");

    rnbo_bridge_log(b, "Creating JACK bridge for set '%s' (fx=%d)", b->set_name, is_effect);

    /* Clean up stale processes from previous crash */
    system("killall -9 rnbooscquery 2>/dev/null");
    system("killall -9 jackd 2>/dev/null");
    usleep(200000);

    // Step 1: Write runner config
    if (write_runner_config(b) < 0) {
        free(b);
        g_rnbo_jack_bridge_active.store(false);
        return NULL;
    }

    // Step 2: Start jackd
    b->jackd_pid = start_jackd(b);
    if (b->jackd_pid < 0) {
        free(b);
        g_rnbo_jack_bridge_active.store(false);
        return NULL;
    }

    // Wait for jackd to start
    usleep(RNBO_JACKD_START_TIMEOUT_US);

    // Step 3: Register JACK client
    jack_status_t status;
    b->client = jack_client_open(RNBO_CLIENT_NAME, JackNoStartServer, &status);
    if (!b->client) {
        rnbo_bridge_set_error(b, "jack_client_open failed: status=%d", (int)status);
        if (b->jackd_pid > 0) { kill(b->jackd_pid, SIGTERM); waitpid(b->jackd_pid, NULL, 0); }
        free(b);
        g_rnbo_jack_bridge_active.store(false);
        return NULL;
    }

    rnbo_bridge_log(b, "JACK client '%s' opened", jack_get_client_name(b->client));

    // Register shutdown callback
    jack_on_shutdown(b->client, rnbo_jack_shutdown_cb, b);

    // Step 4: Create ring buffers
    b->rb_audio_from_runner_L = jack_ringbuffer_create(RNBO_RB_AUDIO_BYTES);
    b->rb_audio_from_runner_R = jack_ringbuffer_create(RNBO_RB_AUDIO_BYTES);
    b->rb_audio_to_runner_L   = jack_ringbuffer_create(RNBO_RB_AUDIO_BYTES);
    b->rb_audio_to_runner_R   = jack_ringbuffer_create(RNBO_RB_AUDIO_BYTES);
    b->rb_midi_to_runner      = jack_ringbuffer_create(RNBO_RB_MIDI_BYTES);

    if (!b->rb_audio_from_runner_L || !b->rb_audio_from_runner_R ||
        !b->rb_audio_to_runner_L || !b->rb_audio_to_runner_R ||
        !b->rb_midi_to_runner) {
        rnbo_bridge_set_error(b, "Failed to create ring buffers");
        jack_client_close(b->client);
        if (b->jackd_pid > 0) { kill(b->jackd_pid, SIGTERM); waitpid(b->jackd_pid, NULL, 0); }
        // Free any that succeeded
        if (b->rb_audio_from_runner_L) jack_ringbuffer_free(b->rb_audio_from_runner_L);
        if (b->rb_audio_from_runner_R) jack_ringbuffer_free(b->rb_audio_from_runner_R);
        if (b->rb_audio_to_runner_L)   jack_ringbuffer_free(b->rb_audio_to_runner_L);
        if (b->rb_audio_to_runner_R)   jack_ringbuffer_free(b->rb_audio_to_runner_R);
        if (b->rb_midi_to_runner)      jack_ringbuffer_free(b->rb_midi_to_runner);
        free(b);
        g_rnbo_jack_bridge_active.store(false);
        return NULL;
    }

    // Step 5: Register JACK ports
    b->audio_in_L = jack_port_register(b->client, "audio_in_L",
                                        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    b->audio_in_R = jack_port_register(b->client, "audio_in_R",
                                        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    b->midi_out   = jack_port_register(b->client, "midi_out",
                                        JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    if (is_effect) {
        b->audio_out_L = jack_port_register(b->client, "audio_out_L",
                                             JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        b->audio_out_R = jack_port_register(b->client, "audio_out_R",
                                             JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }

    // Step 6: Set process callback and activate
    jack_set_process_callback(b->client, rnbo_jack_process_callback, b);

    if (jack_activate(b->client) != 0) {
        rnbo_bridge_set_error(b, "jack_activate failed");
        jack_client_close(b->client);
        if (b->jackd_pid > 0) { kill(b->jackd_pid, SIGTERM); waitpid(b->jackd_pid, NULL, 0); }
        jack_ringbuffer_free(b->rb_audio_from_runner_L);
        jack_ringbuffer_free(b->rb_audio_from_runner_R);
        jack_ringbuffer_free(b->rb_audio_to_runner_L);
        jack_ringbuffer_free(b->rb_audio_to_runner_R);
        jack_ringbuffer_free(b->rb_midi_to_runner);
        free(b);
        g_rnbo_jack_bridge_active.store(false);
        return NULL;
    }

    rnbo_bridge_log(b, "JACK client activated");

    // Step 7: Start Runner
    b->runner_pid = start_runner(b);
    if (b->runner_pid < 0) {
        rnbo_bridge_set_error(b, "Failed to start Runner");
        jack_deactivate(b->client);
        jack_client_close(b->client);
        if (b->jackd_pid > 0) { kill(b->jackd_pid, SIGTERM); waitpid(b->jackd_pid, NULL, 0); }
        jack_ringbuffer_free(b->rb_audio_from_runner_L);
        jack_ringbuffer_free(b->rb_audio_from_runner_R);
        jack_ringbuffer_free(b->rb_audio_to_runner_L);
        jack_ringbuffer_free(b->rb_audio_to_runner_R);
        jack_ringbuffer_free(b->rb_midi_to_runner);
        free(b);
        g_rnbo_jack_bridge_active.store(false);
        return NULL;
    }

    // Step 8: Wait for Runner JACK ports to appear
    if (wait_for_runner_ports(b) < 0) {
        // Non-fatal — ports may appear later, but log it
        rnbo_bridge_log(b, "Warning: Runner ports not yet visible, connections deferred");
    } else {
        // Step 9: Connect ports
        connect_ports(b);
    }

    // Step 10: Pin processes to cores 2-3
    pin_to_cores(b);

    // Step 11: Send OSC to load set (if set_name provided)
    if (b->set_name[0] != '\0') {
        // Give runner a moment to initialize before sending OSC
        usleep(500000);  // 500ms
        send_osc_load_set(b, b->set_name);
    }

    rnbo_bridge_log(b, "JACK bridge created successfully");
    return b;
}

// ---- Destroy bridge ----

static void rnbo_jack_bridge_destroy(RnboJackBridge *b) {
    if (!b) return;

    rnbo_bridge_log(b, "Destroying JACK bridge");

    // Deactivate and close JACK client
    if (b->client) {
        jack_deactivate(b->client);
        jack_client_close(b->client);
        b->client = NULL;
    }

    // Kill Runner
    if (b->runner_pid > 0) {
        rnbo_bridge_log(b, "Killing runner pid=%d", b->runner_pid);
        kill(b->runner_pid, SIGTERM);
        int wstatus;
        // Give it a moment, then force kill
        usleep(500000);
        if (waitpid(b->runner_pid, &wstatus, WNOHANG) == 0) {
            kill(b->runner_pid, SIGKILL);
            waitpid(b->runner_pid, &wstatus, 0);
        }
        b->runner_pid = 0;
    }

    // Kill jackd (only if we started it — pid 0 means we reused an existing one)
    if (b->jackd_pid > 0) {
        rnbo_bridge_log(b, "Killing jackd pid=%d", b->jackd_pid);
        kill(b->jackd_pid, SIGTERM);
        int wstatus;
        usleep(500000);
        if (waitpid(b->jackd_pid, &wstatus, WNOHANG) == 0) {
            kill(b->jackd_pid, SIGKILL);
            waitpid(b->jackd_pid, &wstatus, 0);
        }
        system("killall -9 jackd 2>/dev/null");
        b->jackd_pid = 0;
    }

    // Free ring buffers
    if (b->rb_audio_from_runner_L) jack_ringbuffer_free(b->rb_audio_from_runner_L);
    if (b->rb_audio_from_runner_R) jack_ringbuffer_free(b->rb_audio_from_runner_R);
    if (b->rb_audio_to_runner_L)   jack_ringbuffer_free(b->rb_audio_to_runner_L);
    if (b->rb_audio_to_runner_R)   jack_ringbuffer_free(b->rb_audio_to_runner_R);
    if (b->rb_midi_to_runner)      jack_ringbuffer_free(b->rb_midi_to_runner);

    // Clear global flag
    g_rnbo_jack_bridge_active.store(false);

    rnbo_bridge_log(b, "JACK bridge destroyed");
    free(b);
}

// ---- Audio/MIDI transfer functions ----

// Read audio from Runner ring buffer into interleaved int16 output
// Called from render_block() — NOT the JACK RT thread
static int rnbo_jack_bridge_read_audio(RnboJackBridge *b,
                                        int16_t *out_lr,
                                        int frames) {
    if (!b || !out_lr) return -1;

    /* Periodically check Runner health (every ~1000 blocks ~ 3 seconds) */
    static int health_check_counter = 0;
    if (++health_check_counter >= 1000) {
        health_check_counter = 0;
        if (b->runner_pid > 0) {
            int status;
            pid_t result = waitpid(b->runner_pid, &status, WNOHANG);
            if (result != 0) {
                b->audio_flowing = false;
                rnbo_bridge_log(b, "rnbo-jack: Runner process died");
            }
        }
    }

    size_t bytes = frames * sizeof(float);

    if (jack_ringbuffer_read_space(b->rb_audio_from_runner_L) < bytes ||
        jack_ringbuffer_read_space(b->rb_audio_from_runner_R) < bytes) {
        // Not enough data — output silence
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return 0;
    }

    // Read float buffers
    float buf_L[RNBO_BLOCK_SIZE];
    float buf_R[RNBO_BLOCK_SIZE];
    int to_read = (frames > RNBO_BLOCK_SIZE) ? RNBO_BLOCK_SIZE : frames;

    jack_ringbuffer_read(b->rb_audio_from_runner_L, (char *)buf_L, to_read * sizeof(float));
    jack_ringbuffer_read(b->rb_audio_from_runner_R, (char *)buf_R, to_read * sizeof(float));

    // Convert float -> int16 interleaved
    for (int i = 0; i < to_read; i++) {
        float L = buf_L[i];
        float R = buf_R[i];

        // Clamp
        if (L > 1.0f) L = 1.0f;
        if (L < -1.0f) L = -1.0f;
        if (R > 1.0f) R = 1.0f;
        if (R < -1.0f) R = -1.0f;

        out_lr[i * 2]     = (int16_t)(L * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(R * 32767.0f);
    }

    return to_read;
}

// Write interleaved int16 audio to Runner ring buffer (fx mode)
// Called from process_block() — NOT the JACK RT thread
static int rnbo_jack_bridge_write_audio(RnboJackBridge *b,
                                         const int16_t *in_lr,
                                         int frames) {
    if (!b || !in_lr || !b->is_effect) return -1;

    int to_write = (frames > RNBO_BLOCK_SIZE) ? RNBO_BLOCK_SIZE : frames;
    size_t bytes = to_write * sizeof(float);

    if (jack_ringbuffer_write_space(b->rb_audio_to_runner_L) < bytes)
        return 0;  // Ring buffer full, drop

    // Convert int16 interleaved -> float separate channels
    float buf_L[RNBO_BLOCK_SIZE];
    float buf_R[RNBO_BLOCK_SIZE];

    for (int i = 0; i < to_write; i++) {
        buf_L[i] = (float)in_lr[i * 2]     / 32768.0f;
        buf_R[i] = (float)in_lr[i * 2 + 1] / 32768.0f;
    }

    jack_ringbuffer_write(b->rb_audio_to_runner_L, (const char *)buf_L, bytes);
    jack_ringbuffer_write(b->rb_audio_to_runner_R, (const char *)buf_R, bytes);

    return to_write;
}

// Send MIDI message to Runner via ring buffer
// Format in ring buffer: [len:1][data:len]
static int rnbo_jack_bridge_send_midi(RnboJackBridge *b,
                                       const uint8_t *msg,
                                       int len) {
    if (!b || !msg || len <= 0 || len > 16) return -1;

    size_t needed = 1 + len;
    if (jack_ringbuffer_write_space(b->rb_midi_to_runner) < needed)
        return 0;  // Full, drop

    uint8_t len_byte = (uint8_t)len;
    jack_ringbuffer_write(b->rb_midi_to_runner, (const char *)&len_byte, 1);
    jack_ringbuffer_write(b->rb_midi_to_runner, (const char *)msg, len);

    return len;
}

#endif // RNBO_JACK_BRIDGE_H
