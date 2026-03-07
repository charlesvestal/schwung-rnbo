/*
 * RNBO Host Plugin for Move Anything
 *
 * Dynamically loads compiled RNBO device .so files via a flat C API
 * (rnbo_device_api.h), avoiding cross-SO C++ vtable issues.
 *
 * Users place compiled devices in: <module_dir>/devices/<device_name>/
 * Each device directory contains: device.so + description.json
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <dlfcn.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <algorithm>

/* Plugin API */
extern "C" {
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;
}

/* RNBO device C API (resolved via dlsym) */
#include "rnbo_device_api.h"

/* Function pointer types for dlsym */
typedef rnbo_device_t (*fn_create)(void);
typedef void (*fn_destroy)(rnbo_device_t);
typedef void (*fn_get_info)(rnbo_device_t, rnbo_device_info_t*);
typedef void (*fn_prepare)(rnbo_device_t, double, int);
typedef void (*fn_process)(rnbo_device_t, float**, int, float**, int, int);
typedef void (*fn_midi_in)(rnbo_device_t, const uint8_t*, int);
typedef int (*fn_get_num_params)(rnbo_device_t);
typedef void (*fn_get_param_info)(rnbo_device_t, int, rnbo_param_info_t*);
typedef const char* (*fn_get_param_name)(rnbo_device_t, int);
typedef double (*fn_get_param)(rnbo_device_t, int);
typedef void (*fn_set_param)(rnbo_device_t, int, double);

/* ======================================================================
 * Globals
 * ====================================================================== */

static const host_api_v1_t *g_host = nullptr;

static void host_log(const char *fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host->log(buf);
}

/* ======================================================================
 * JSON helpers
 * ====================================================================== */

static std::string json_get_string(const char *json, const char *key) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return "";
    p = strchr(p + strlen(needle), ':');
    if (!p) return "";
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return "";
        return std::string(p, end - p);
    }
    return "";
}

static int json_get_int(const char *json, const char *key, int fallback = 0) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':');
    if (!p) return fallback;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static double json_get_double(const char *json, const char *key, double fallback = 0.0) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':');
    if (!p) return fallback;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atof(p);
}

static bool json_get_bool(const char *json, const char *key, bool fallback = false) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':');
    if (!p) return fallback;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "true", 4) == 0;
}

static std::string read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string content(len, '\0');
    size_t nread = fread(&content[0], 1, len, f);
    content.resize(nread);
    fclose(f);
    return content;
}

/* ======================================================================
 * Parameter metadata from description.json
 * ====================================================================== */

struct ParamMeta {
    std::string paramId;
    std::string name;
    std::string displayName;
    std::string unit;
    double minimum, maximum, initialValue, exponent;
    int steps;
    bool isEnum;
    std::vector<std::string> enumValues;
    int index;
};

static std::vector<ParamMeta> parse_params_from_description(const char *json) {
    std::vector<ParamMeta> params;
    const char *arr = strstr(json, "\"parameters\"");
    if (!arr) return params;
    arr = strchr(arr, '[');
    if (!arr) return params;
    arr++;

    while (*arr) {
        const char *obj_start = strchr(arr, '{');
        if (!obj_start) break;
        const char *p = obj_start + 1;
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        if (depth != 0) break;

        std::string obj(obj_start, p - obj_start);
        const char *s = obj.c_str();

        ParamMeta pm;
        pm.paramId = json_get_string(s, "paramId");
        pm.name = json_get_string(s, "name");
        pm.displayName = json_get_string(s, "displayName");
        pm.unit = json_get_string(s, "unit");
        pm.minimum = json_get_double(s, "minimum", 0.0);
        pm.maximum = json_get_double(s, "maximum", 1.0);
        pm.initialValue = json_get_double(s, "initialValue", 0.0);
        pm.exponent = json_get_double(s, "exponent", 1.0);
        pm.steps = json_get_int(s, "steps", 0);
        pm.isEnum = json_get_bool(s, "isEnum", false);
        pm.index = json_get_int(s, "index", (int)params.size());

        if (pm.isEnum) {
            const char *ev = strstr(s, "\"enumValues\"");
            if (ev) {
                ev = strchr(ev, '[');
                if (ev) {
                    ev++;
                    while (*ev) {
                        while (*ev == ' ' || *ev == '\t' || *ev == '\n' || *ev == ',') ev++;
                        if (*ev == ']') break;
                        if (*ev == '"') {
                            ev++;
                            const char *end = strchr(ev, '"');
                            if (!end) break;
                            pm.enumValues.push_back(std::string(ev, end - ev));
                            ev = end + 1;
                        } else break;
                    }
                }
            }
        }

        /* Skip invisible params (polyphony sub-patcher duplicates) and signal params */
        bool visible = json_get_bool(s, "visible", true);
        std::string ptype = json_get_string(s, "type");
        if (!pm.paramId.empty() && visible && ptype != "ParameterTypeSignal") params.push_back(pm);
        arr = p;
        if (*arr == ',') arr++;
        while (*arr == ' ' || *arr == '\n' || *arr == '\t') arr++;
        if (*arr == ']') break;
    }
    return params;
}

/* ======================================================================
 * Device — a loaded RNBO .so with C API function pointers
 * ====================================================================== */

struct RnboDevice {
    std::string name;
    std::string dir_path;
    std::string so_path;
    void *dl_handle;

    /* C API function pointers (resolved via dlsym) */
    fn_create      api_create;
    fn_destroy     api_destroy;
    fn_get_info    api_get_info;
    fn_prepare     api_prepare;
    fn_process     api_process;
    fn_midi_in     api_midi_in;
    fn_get_num_params  api_get_num_params;
    fn_get_param_info  api_get_param_info;
    fn_get_param_name  api_get_param_name;
    fn_get_param       api_get_param;
    fn_set_param       api_set_param;

    int num_input_channels;
    int num_output_channels;
    std::vector<ParamMeta> params;
    bool is_effect;
};

/* ======================================================================
 * Instance
 * ====================================================================== */

#define MAX_RNBO_CHANNELS 2

struct RnboInstance {
    RnboDevice *device;
    rnbo_device_t rnbo_dev;  /* opaque handle from device C API */

    float in_buf_l[MOVE_FRAMES_PER_BLOCK];
    float in_buf_r[MOVE_FRAMES_PER_BLOCK];
    float out_buf_l[MOVE_FRAMES_PER_BLOCK];
    float out_buf_r[MOVE_FRAMES_PER_BLOCK];
    float *in_ptrs[MAX_RNBO_CHANNELS];
    float *out_ptrs[MAX_RNBO_CHANNELS];

    struct MidiMsg { uint8_t data[3]; int len; };
    std::vector<MidiMsg> pending_midi;

    std::vector<double> param_values;
    float volume;

    std::string module_dir;
    std::vector<RnboDevice> devices;
    int current_device_index;

    std::string chain_params_json;
    std::string ui_hierarchy_json;
    std::string device_list_json;
};

/* ======================================================================
 * Device loading
 * ====================================================================== */

static bool load_device(RnboDevice &dev) {
    /* Load description.json */
    std::string desc_path = dev.dir_path + "/description.json";
    std::string desc = read_file(desc_path.c_str());
    if (desc.empty()) {
        host_log("rnbo: no description.json in %s", dev.dir_path.c_str());
        return false;
    }

    dev.num_input_channels = json_get_int(desc.c_str(), "numInputChannels", 0);
    dev.num_output_channels = json_get_int(desc.c_str(), "numOutputChannels", 2);
    dev.is_effect = (dev.num_input_channels > 0);
    dev.params = parse_params_from_description(desc.c_str());

    /* dlopen */
    dev.dl_handle = dlopen(dev.so_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!dev.dl_handle) {
        host_log("rnbo: dlopen failed: %s", dlerror());
        return false;
    }

    /* Resolve C API functions */
    #define RESOLVE(name) dev.api_##name = (fn_##name)dlsym(dev.dl_handle, "rnbo_device_" #name)
    RESOLVE(create);
    RESOLVE(destroy);
    RESOLVE(get_info);
    RESOLVE(prepare);
    RESOLVE(process);
    RESOLVE(midi_in);
    RESOLVE(get_num_params);
    RESOLVE(get_param_info);
    RESOLVE(get_param_name);
    RESOLVE(get_param);
    RESOLVE(set_param);
    #undef RESOLVE

    if (!dev.api_create || !dev.api_destroy || !dev.api_prepare || !dev.api_process) {
        host_log("rnbo: missing C API symbols in %s", dev.so_path.c_str());
        dlclose(dev.dl_handle);
        dev.dl_handle = nullptr;
        return false;
    }

    host_log("rnbo: loaded device '%s' (%s, %d params, %din/%dout)",
             dev.name.c_str(), dev.is_effect ? "effect" : "instrument",
             (int)dev.params.size(), dev.num_input_channels, dev.num_output_channels);
    return true;
}

static void unload_device(RnboDevice &dev) {
    if (dev.dl_handle) { dlclose(dev.dl_handle); dev.dl_handle = nullptr; }
}

static std::vector<RnboDevice> scan_devices(const char *module_dir) {
    std::vector<RnboDevice> devices;
    std::string devices_dir = std::string(module_dir) + "/devices";
    DIR *dir = opendir(devices_dir.c_str());
    if (!dir) {
        host_log("rnbo: no devices/ directory at %s", devices_dir.c_str());
        return devices;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string dev_dir = devices_dir + "/" + entry->d_name;
        std::string so_path = dev_dir + "/device.so";
        FILE *f = fopen(so_path.c_str(), "r");
        if (!f) continue;
        fclose(f);

        RnboDevice dev;
        dev.name = entry->d_name;
        dev.dir_path = dev_dir;
        dev.so_path = so_path;
        dev.dl_handle = nullptr;
        if (load_device(dev)) devices.push_back(std::move(dev));
    }
    closedir(dir);
    return devices;
}

/* ======================================================================
 * JSON generation
 * ====================================================================== */

static std::string escape_json(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

static std::string build_chain_params(const RnboDevice &dev) {
    std::string j = "[{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.7}";
    for (auto &pm : dev.params) {
        j += ",{\"key\":\"" + escape_json(pm.paramId) + "\"";
        j += ",\"name\":\"" + escape_json(pm.displayName.empty() ? pm.name : pm.displayName) + "\"";
        if (pm.isEnum && !pm.enumValues.empty()) {
            j += ",\"type\":\"enum\",\"options\":[";
            for (size_t i = 0; i < pm.enumValues.size(); i++) {
                if (i) j += ",";
                j += "\"" + escape_json(pm.enumValues[i]) + "\"";
            }
            j += "],\"default\":" + std::to_string((int)pm.initialValue);
        } else if (pm.steps == 1) {
            j += ",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"default\":" + std::to_string(pm.initialValue > 0.5 ? 1 : 0);
        } else {
            char nb[64];
            j += ",\"type\":\"float\"";
            snprintf(nb, sizeof(nb), "%.6g", pm.minimum); j += std::string(",\"min\":") + nb;
            snprintf(nb, sizeof(nb), "%.6g", pm.maximum); j += std::string(",\"max\":") + nb;
            double step = (pm.steps > 1) ? (pm.maximum - pm.minimum) / (pm.steps - 1) : (pm.maximum - pm.minimum) / 100.0;
            snprintf(nb, sizeof(nb), "%.6g", step); j += std::string(",\"step\":") + nb;
            snprintf(nb, sizeof(nb), "%.6g", pm.initialValue); j += std::string(",\"default\":") + nb;
            if (!pm.unit.empty()) j += ",\"unit\":\"" + escape_json(pm.unit) + "\"";
        }
        j += "}";
    }
    j += "]";
    return j;
}

static std::string build_ui_hierarchy(const RnboDevice &dev, int num_devices) {
    std::string j = "{\"levels\":{\"root\":{\"label\":\"RNBO\",\"params\":[{\"key\":\"volume\",\"label\":\"Volume\"}";
    if (num_devices > 1) j += ",{\"level\":\"device_select\",\"label\":\"Choose Device\"}";
    for (auto &pm : dev.params) {
        j += ",{\"key\":\"" + escape_json(pm.paramId) + "\",\"label\":\"" + escape_json(pm.displayName.empty() ? pm.name : pm.displayName) + "\"}";
    }
    j += "],\"knobs\":[\"volume\"";
    int k = 0;
    for (auto &pm : dev.params) {
        if (k++ >= 7) break;
        j += ",\"" + escape_json(pm.paramId) + "\"";
    }
    j += "]}";
    if (num_devices > 1) j += ",\"device_select\":{\"label\":\"Device\",\"items_param\":\"device_list\",\"select_param\":\"device_index\",\"knobs\":[],\"params\":[]}";
    j += "}}";
    return j;
}

static std::string build_device_list(const std::vector<RnboDevice> &devices) {
    std::string j = "[";
    for (size_t i = 0; i < devices.size(); i++) {
        if (i) j += ",";
        char idx[16];
        snprintf(idx, sizeof(idx), "%d", (int)i);
        j += "{\"label\":\"" + escape_json(devices[i].name) + "\",\"index\":" + idx + "}";
    }
    j += "]";
    return j;
}

/* ======================================================================
 * Activate device
 * ====================================================================== */

static void activate_device(RnboInstance *inst, int index) {
    if (index < 0 || index >= (int)inst->devices.size()) return;

    /* Destroy current */
    if (inst->rnbo_dev && inst->device && inst->device->api_destroy) {
        inst->device->api_destroy(inst->rnbo_dev);
        inst->rnbo_dev = nullptr;
    }

    inst->current_device_index = index;
    RnboDevice *dev = &inst->devices[index];
    inst->device = dev;

    host_log("rnbo: creating device '%s'", dev->name.c_str());
    inst->rnbo_dev = dev->api_create();
    if (!inst->rnbo_dev) {
        host_log("rnbo: create failed for '%s'", dev->name.c_str());
        return;
    }

    host_log("rnbo: preparing device '%s'", dev->name.c_str());
    dev->api_prepare(inst->rnbo_dev, MOVE_SAMPLE_RATE, MOVE_FRAMES_PER_BLOCK);
    host_log("rnbo: device '%s' ready", dev->name.c_str());

    inst->param_values.resize(dev->params.size());
    for (size_t i = 0; i < dev->params.size(); i++)
        inst->param_values[i] = dev->params[i].initialValue;

    inst->chain_params_json = build_chain_params(*dev);
    inst->ui_hierarchy_json = build_ui_hierarchy(*dev, (int)inst->devices.size());
    inst->device_list_json = build_device_list(inst->devices);
}

/* ======================================================================
 * Plugin API v2
 * ====================================================================== */

static void* rnbo_create(const char *module_dir, const char *json_defaults) {
    auto *inst = new RnboInstance();
    inst->module_dir = module_dir;
    inst->volume = 0.7f;
    inst->device = nullptr;
    inst->rnbo_dev = nullptr;
    inst->current_device_index = -1;
    inst->in_ptrs[0] = inst->in_buf_l;
    inst->in_ptrs[1] = inst->in_buf_r;
    inst->out_ptrs[0] = inst->out_buf_l;
    inst->out_ptrs[1] = inst->out_buf_r;

    inst->devices = scan_devices(module_dir);
    if (inst->devices.empty()) {
        host_log("rnbo: no devices found in %s/devices/", module_dir);
    } else {
        int dev_idx = 0;
        if (json_defaults && json_defaults[0]) {
            const char *di = strstr(json_defaults, "\"device_index\"");
            if (di) { di = strchr(di, ':'); if (di) { di++; while (*di == ' ') di++; dev_idx = atoi(di); } }
        }
        if (dev_idx >= 0 && dev_idx < (int)inst->devices.size())
            activate_device(inst, dev_idx);
    }
    return inst;
}

static void rnbo_destroy(void *instance) {
    auto *inst = (RnboInstance*)instance;
    if (!inst) return;
    if (inst->rnbo_dev && inst->device && inst->device->api_destroy)
        inst->device->api_destroy(inst->rnbo_dev);
    for (auto &dev : inst->devices) unload_device(dev);
    delete inst;
}

static void rnbo_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    auto *inst = (RnboInstance*)instance;
    if (!inst || !inst->rnbo_dev || len < 1) return;
    RnboInstance::MidiMsg m;
    m.len = (len > 3) ? 3 : len;
    m.data[0] = msg[0];
    m.data[1] = (len >= 2) ? msg[1] : 0;
    m.data[2] = (len >= 3) ? msg[2] : 0;
    inst->pending_midi.push_back(m);
}

static void rnbo_set_param(void *instance, const char *key, const char *val) {
    auto *inst = (RnboInstance*)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "volume") == 0) {
        inst->volume = std::max(0.0f, std::min(1.0f, (float)atof(val)));
        return;
    }
    if (strcmp(key, "device_index") == 0) {
        int idx = atoi(val);
        if (idx != inst->current_device_index && idx >= 0 && idx < (int)inst->devices.size())
            activate_device(inst, idx);
        return;
    }
    if (!inst->rnbo_dev || !inst->device) return;
    for (size_t i = 0; i < inst->device->params.size(); i++) {
        if (inst->device->params[i].paramId == key) {
            if (inst->device->api_set_param)
                inst->device->api_set_param(inst->rnbo_dev, inst->device->params[i].index, atof(val));
            inst->param_values[i] = atof(val);
            return;
        }
    }
}

static int rnbo_get_param(void *instance, const char *key, char *buf, int buf_len) {
    auto *inst = (RnboInstance*)instance;
    if (!inst || !key || !buf || buf_len < 1) return 0;

    if (strcmp(key, "volume") == 0) return snprintf(buf, buf_len, "%.2f", inst->volume);
    if (strcmp(key, "device_index") == 0) return snprintf(buf, buf_len, "%d", inst->current_device_index);
    if (strcmp(key, "device_list") == 0) return snprintf(buf, buf_len, "%s", inst->device_list_json.c_str());
    if (strcmp(key, "device_count") == 0) return snprintf(buf, buf_len, "%d", (int)inst->devices.size());
    if (strcmp(key, "chain_params") == 0) return snprintf(buf, buf_len, "%s", inst->chain_params_json.c_str());
    if (strcmp(key, "ui_hierarchy") == 0) return snprintf(buf, buf_len, "%s", inst->ui_hierarchy_json.c_str());
    if (strcmp(key, "component_type") == 0) {
        return snprintf(buf, buf_len, "%s", (inst->device && inst->device->is_effect) ? "audio_fx" : "sound_generator");
    }

    if (!inst->rnbo_dev || !inst->device) return 0;
    for (size_t i = 0; i < inst->device->params.size(); i++) {
        if (inst->device->params[i].paramId == key) {
            double v = inst->device->api_get_param ? inst->device->api_get_param(inst->rnbo_dev, inst->device->params[i].index) : 0.0;
            return snprintf(buf, buf_len, "%.6g", v);
        }
    }
    return 0;
}

static int rnbo_get_error(void *instance, char *buf, int buf_len) {
    auto *inst = (RnboInstance*)instance;
    if (!inst) return 0;
    if (inst->devices.empty()) return snprintf(buf, buf_len, "No RNBO devices found");
    return 0;
}

static void rnbo_render_block(void *instance, int16_t *out_lr, int frames) {
    auto *inst = (RnboInstance*)instance;
    if (!inst || !inst->rnbo_dev || !inst->device) {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Convert input for effects */
    if (inst->device->is_effect) {
        for (int i = 0; i < frames; i++) {
            inst->in_buf_l[i] = out_lr[i * 2 + 0] / 32768.0f;
            inst->in_buf_r[i] = out_lr[i * 2 + 1] / 32768.0f;
        }
    } else {
        memset(inst->in_buf_l, 0, frames * sizeof(float));
        memset(inst->in_buf_r, 0, frames * sizeof(float));
    }
    memset(inst->out_buf_l, 0, frames * sizeof(float));
    memset(inst->out_buf_r, 0, frames * sizeof(float));

    /* Inject MIDI */
    if (inst->device->api_midi_in) {
        for (auto &m : inst->pending_midi)
            inst->device->api_midi_in(inst->rnbo_dev, m.data, m.len);
    }
    inst->pending_midi.clear();

    /* Process audio via C API */
    int num_in = std::min(inst->device->num_input_channels, MAX_RNBO_CHANNELS);
    int num_out = std::min(inst->device->num_output_channels, MAX_RNBO_CHANNELS);
    inst->device->api_process(inst->rnbo_dev, inst->in_ptrs, num_in, inst->out_ptrs, num_out, frames);

    /* Convert to int16 */
    float vol = inst->volume;
    for (int i = 0; i < frames; i++) {
        float l = inst->out_buf_l[i] * vol;
        float r = (num_out > 1) ? inst->out_buf_r[i] * vol : l;
        l = std::max(-1.0f, std::min(1.0f, l));
        r = std::max(-1.0f, std::min(1.0f, r));
        out_lr[i * 2 + 0] = (int16_t)(l * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
}

/* ======================================================================
 * Entry point
 * ====================================================================== */

static plugin_api_v2_t g_api;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    g_api.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_api.create_instance = rnbo_create;
    g_api.destroy_instance = rnbo_destroy;
    g_api.on_midi = rnbo_on_midi;
    g_api.set_param = rnbo_set_param;
    g_api.get_param = rnbo_get_param;
    g_api.get_error = rnbo_get_error;
    g_api.render_block = rnbo_render_block;
    host_log("rnbo: plugin initialized");
    return &g_api;
}
