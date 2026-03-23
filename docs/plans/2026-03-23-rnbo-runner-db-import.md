# RNBO Runner DB Import - Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make RNBO Runner graphs (stored in sqlite DB) appear as chainable synth/fx modules via the existing scan_packs pipeline.

**Architecture:** At scan time, read the RNBO Runner sqlite DB, generate pack directories (info.json + set JSON) for each graph, then let the existing scan_packs_dir pick them up as virtual module entries. Also trigger this on overtake module exit so new graphs appear without full reboot.

**Tech Stack:** C (module_manager.c), sqlite3 amalgamation, existing scan_packs pipeline

---

## Background

### Where things live on device

| What | Path |
|------|------|
| RNBO Runner DB | `/data/UserData/Documents/rnbo/oscqueryrunner.sqlite` |
| Patcher .so files | `/data/UserData/Documents/rnbo/cache/so/` |
| Patcher config JSON | `/data/UserData/Documents/rnbo/cache/src/` |
| rnbo-synth packs dir | `/data/UserData/schwung/modules/sound_generators/rnbo-synth/packs/` |

### DB schema (relevant tables)

```
sets: id, name, filename, runner_rnbo_version, meta
sets_patcher_instances: id, patcher_id, set_id, set_instance_index, config (JSON)
sets_connections: id, set_id, source_name, source_instance_index, source_port_name,
                  sink_name, sink_instance_index, sink_port_name
sets_presets: id, patcher_id, set_id, content (JSON), preset_index
patchers: id, name, so_path, config_path, audio_inputs, audio_outputs, midi_inputs
```

### Target pack directory format

For a set named "FM Splash", generate:
```
packs/FM-Splash/
  info.json          # patchers list + set location
  sets/FM-Splash.json  # instances, connections, presets
```

**info.json format:**
```json
{
  "name": "FM Splash",
  "patchers": [
    {
      "name": "fm-synth-32",
      "binaries": {
        "aarch64-Linux-GNU-11.4.0": "/data/UserData/Documents/rnbo/cache/so/librnbogen.XXX.so"
      },
      "config": "/data/UserData/Documents/rnbo/cache/src/rnbogen.XXX-config.json"
    }
  ],
  "sets": [
    { "name": "FM Splash", "location": "sets/FM-Splash.json" }
  ],
  "targets": {
    "aarch64-Linux-GNU-11.4.0": { "system_processor": "aarch64" }
  }
}
```

**sets/FM-Splash.json format:**
```json
{
  "name": "FM Splash",
  "instances": [
    { "instance_index": 1, "patcher_name": "waves", "config": {...} },
    { "instance_index": 2, "patcher_name": "fm-synth-32", "config": {...} }
  ],
  "connections": [
    {
      "source_name": "fm-synth-32-2", "source_instance_index": 2,
      "source_port_name": "out1",
      "sink_name": "move-volume", "sink_instance_index": -1,
      "sink_port_name": "in1"
    }
  ],
  "presets": {
    "initial": [
      { "instanceindex": 0, "patchername": "waves", "presetindex": 0, "content": {...} }
    ]
  }
}
```

### Repos

| Repo | What changes |
|------|-------------|
| schwung | Add sqlite3 to build, new `export_rnbo_runner_sets()` in module_manager.c, overtake exit rescan |
| schwung-rnbo | No changes needed (existing pack loading works) |

---

## Task 1: Add sqlite3 amalgamation to schwung

**Files:**
- Create: `schwung/libs/sqlite3/sqlite3.c` (copy from schwung-surge)
- Create: `schwung/libs/sqlite3/sqlite3.h` (copy from schwung-surge)
- Modify: `schwung/scripts/build.sh`

**Step 1: Copy sqlite3 amalgamation**

```bash
mkdir -p /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/libs/sqlite3
cp /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-surge/src/dsp/surge/libs/sqlite-3.23.3/sqlite3.c \
   /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/libs/sqlite3/
cp /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-surge/src/dsp/surge/libs/sqlite-3.23.3/sqlite3.h \
   /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/libs/sqlite3/
```

**Step 2: Add sqlite3 to the build**

In `scripts/build.sh`, add sqlite3 compilation before the host build. Find the host compile command (around line 161) and:

1. Add a sqlite3 object compilation step (like the existing arc4random pattern):
```bash
echo "Compiling sqlite3..."
"${CROSS_PREFIX}gcc" -g -O2 -c \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_THREADSAFE=0 \
    libs/sqlite3/sqlite3.c \
    -o build/sqlite3.o
```

2. Add `build/sqlite3.o` to the host link command and `-Ilibs/sqlite3` to includes:
```bash
"${CROSS_PREFIX}gcc" -g -O3 \
    src/schwung_host.c \
    src/host/module_manager.c \
    src/host/settings.c \
    src/host/unified_log.c \
    build/sqlite3.o \
    -o build/schwung \
    -Isrc -Isrc/lib -Ilibs/sqlite3 \
    ...
```

**Step 3: Verify build compiles**

```bash
cd schwung && ./scripts/build.sh
```

**Step 4: Commit**

```bash
git add libs/sqlite3/sqlite3.c libs/sqlite3/sqlite3.h scripts/build.sh
git commit -m "Add sqlite3 amalgamation for RNBO Runner DB access"
```

---

## Task 2: Implement export_rnbo_runner_sets() in module_manager.c

**Files:**
- Modify: `schwung/src/host/module_manager.c`

This function reads the RNBO Runner sqlite DB and generates pack directories.

**Step 1: Add sqlite3 include and DB path constant**

At the top of module_manager.c, add:
```c
#include "sqlite3.h"

#define RNBO_RUNNER_DB "/data/UserData/Documents/rnbo/oscqueryrunner.sqlite"
#define RNBO_CACHE_SO "/data/UserData/Documents/rnbo/cache/so"
#define RNBO_CACHE_SRC "/data/UserData/Documents/rnbo/cache/src"
```

**Step 2: Implement the export function**

Add `export_rnbo_runner_sets(const char *packs_path)` before `scan_packs_dir`.

The function should:

1. Open the DB (read-only). If it doesn't exist, return silently (runner not installed).
2. Query all sets: `SELECT id, name FROM sets`
3. For each set:
   a. Create safe directory name (spaces → dashes)
   b. Skip if pack dir already has a newer info.json than the DB file (mtime check)
   c. Query patcher instances: `SELECT si.set_instance_index, si.config, p.name, p.so_path, p.config_path FROM sets_patcher_instances si JOIN patchers p ON si.patcher_id = p.id WHERE si.set_id = ?`
   d. Query connections: `SELECT source_name, source_instance_index, source_port_name, sink_name, sink_instance_index, sink_port_name FROM sets_connections WHERE set_id = ?`
   e. Query presets: `SELECT sp.preset_index, sp.content, p.name AS patcher_name FROM sets_presets sp JOIN patchers p ON sp.patcher_id = p.id WHERE sp.set_id = ?`
   f. Generate info.json with patchers list (using absolute paths to cache/so/ and cache/src/)
   g. Generate sets/SetName.json with instances, connections, presets
   h. Write files via mkdir -p + fopen/fprintf/fclose
4. Close DB.

**Key details for info.json generation:**
- `so_path` in DB is just the filename (e.g., `librnbogen.XXX.so`). Prepend `RNBO_CACHE_SO/`.
- `config_path` in DB is just the filename (e.g., `rnbogen.XXX-config.json`). Prepend `RNBO_CACHE_SRC/`.
- The `binaries` key must be `"aarch64-Linux-GNU-11.4.0"` to match what rnbo_host.cpp looks for.
- Deduplicate patchers (a patcher may appear in multiple instances).

**Key details for set JSON generation:**
- Instance `config` field comes directly from `sets_patcher_instances.config` (already JSON).
- Connection fields map 1:1 from DB columns to JSON keys.
- Presets go in `"presets": {"initial": [...]}` array, each with `instanceindex`, `patchername`, `presetindex`, `content`.
- The `content` in sets_presets is already JSON — embed it as-is.
- Use instance `set_instance_index` from DB as `instance_index` in JSON.

**Step 3: Call from scan_packs_dir**

In `scan_packs_dir`, add the call before the directory scan loop:
```c
/* Import sets from RNBO Runner database if available */
export_rnbo_runner_sets(packs_path);
```

**Step 4: Build and test**

```bash
cd schwung && ./scripts/build.sh
scp build/schwung root@move.local:/data/UserData/schwung/schwung
# Reboot device, check that runner graphs appear in menu
```

**Step 5: Commit**

```bash
git add src/host/module_manager.c
git commit -m "Import RNBO Runner graphs from sqlite DB as scan_packs entries"
```

---

## Task 3: Trigger rescan on overtake module exit

**Files:**
- Explore: How overtake exit currently works (exit hook in schwung)
- Modify: Whatever triggers module rescan after overtake exits

When the RNBO Runner overtake module exits (user leaves overtake mode), schwung restarts. If schwung fully restarts, the scan happens automatically at boot — no extra work needed.

If schwung does NOT fully restart on overtake exit (just reloads modules), then we need to call `mm_scan_modules()` again or at minimum re-run `export_rnbo_runner_sets()` + `scan_packs_dir()`.

**Step 1: Verify overtake exit behavior**

Check the exit hook / overtake exit flow. If schwung restarts fully, this task is done — the boot scan covers it.

If not, find where modules are reloaded after overtake exit and add a rescan call.

**Step 2: Test**

1. Boot device, load RNBO Runner overtake module
2. Create or modify a graph in the runner
3. Exit overtake mode
4. Check if new/modified graph appears in chain synth list

**Step 3: Commit if changes needed**

```bash
git commit -m "Trigger module rescan on overtake exit for new RNBO graphs"
```

---

## Task 4: Handle edge cases

**Files:**
- Modify: `schwung/src/host/module_manager.c` (export function)

**Edge cases to handle:**
1. **DB doesn't exist** — return silently (runner not installed). Already handled by sqlite3_open failing.
2. **Empty sets table** — no output, no crash.
3. **Patcher .so missing from cache** — skip that set (log warning). Check file exists before writing info.json.
4. **Set name with special characters** — sanitize for directory name (replace spaces, slashes, etc. with dashes).
5. **Pack dir already exists from .rnbopack extraction** — the .rnbopack-extracted packs and DB-exported packs may overlap if user exported the same graph both ways. Use DB mtime vs info.json mtime to decide whether to regenerate.
6. **DB is locked** — open read-only with `SQLITE_OPEN_READONLY` flag. The runner may be writing while we read.

**Step 1: Implement edge case handling in export function**

**Step 2: Test each case**

**Step 3: Commit**

```bash
git commit -m "Handle edge cases in RNBO Runner DB import"
```

---

## Task 5: Final integration test and push

**Steps:**
1. Clean build both repos
2. Deploy to device
3. Verify: runner graphs appear as chain synths
4. Verify: loading a runner graph produces audio
5. Verify: existing .rnbopack-based packs still work
6. Verify: device works when RNBO Runner DB doesn't exist
7. Push both repos

---

## Notes

- The sqlite3 amalgamation adds ~250KB to the binary. Using `SQLITE_OMIT_LOAD_EXTENSION` and `SQLITE_THREADSAFE=0` keeps it minimal.
- The export generates files on the device filesystem at boot. These are ephemeral — regenerated each boot from the DB. They could go in a temp/cache dir instead of packs/ if we want to keep packs/ clean for user-placed .rnbopack files.
- The rnbo_host.cpp DSP doesn't need changes — it already loads from pack directories via the `{"pack":"/path"}` config_json mechanism we just built.
