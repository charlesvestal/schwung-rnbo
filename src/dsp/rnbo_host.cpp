// RNBO Graph Host for Move Anything
//
// Loads rnbopack graphs: multiple interconnected RNBO patchers with
// audio, MIDI, and message routing. Each patcher is loaded via the
// bridge (flat C API), and the host orchestrates them per the set JSON.
//
// Falls back to single-device mode if no set JSON is found.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <dlfcn.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <queue>

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

/* Audio FX API v2 */
extern "C" {
#define AUDIO_FX_API_VERSION_2 2
typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;
}

/* RNBO device C API (resolved via dlsym from bridge .so) */
#include "rnbo_device_api.h"

/* Function pointer types for dlsym */
typedef rnbo_device_t (*fn_create)(void);
typedef void (*fn_destroy)(rnbo_device_t);
typedef void (*fn_get_info)(rnbo_device_t, rnbo_device_info_t*);
typedef void (*fn_prepare)(rnbo_device_t, double, int);
typedef void (*fn_process)(rnbo_device_t, float**, int, float**, int, int);
typedef int (*fn_process_midi)(rnbo_device_t, float**, int, float**, int, int, rnbo_midi_event_t*, int);
typedef void (*fn_midi_in)(rnbo_device_t, const uint8_t*, int);
typedef int (*fn_get_num_params)(rnbo_device_t);
typedef void (*fn_get_param_info)(rnbo_device_t, int, rnbo_param_info_t*);
typedef const char* (*fn_get_param_name)(rnbo_device_t, int);
typedef double (*fn_get_param)(rnbo_device_t, int);
typedef void (*fn_set_param)(rnbo_device_t, int, double);
typedef void (*fn_send_msg_num)(rnbo_device_t, const char*, double);
typedef void (*fn_send_msg_list)(rnbo_device_t, const char*, const double*, int);
typedef void (*fn_set_msg_cb)(rnbo_device_t, rnbo_message_callback_t, void*);
typedef void (*fn_drain_msgs)(rnbo_device_t);

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
 * Helpers
 * ====================================================================== */

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

static std::string escape_json(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

/* Simple JSON helpers for hand-rolled parsing of description.json */
static std::string json_get_string(const char *json, const char *key) {
    char needle[256]; snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return "";
    p = strchr(p + strlen(needle), ':'); if (!p) return "";
    p++; while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { p++; const char *end = strchr(p, '"'); if (!end) return ""; return std::string(p, end - p); }
    return "";
}

static int json_get_int(const char *json, const char *key, int fallback = 0) {
    char needle[256]; snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':'); if (!p) return fallback;
    p++; while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static double json_get_double(const char *json, const char *key, double fallback = 0.0) {
    char needle[256]; snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':'); if (!p) return fallback;
    p++; while (*p == ' ' || *p == '\t') p++;
    return atof(p);
}

static bool json_get_bool(const char *json, const char *key, bool fallback = false) {
    char needle[256]; snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return fallback;
    p = strchr(p + strlen(needle), ':'); if (!p) return fallback;
    p++; while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "true", 4) == 0;
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
        while (*p && depth > 0) { if (*p == '{') depth++; else if (*p == '}') depth--; p++; }
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
            if (ev) { ev = strchr(ev, '['); if (ev) { ev++;
                while (*ev) {
                    while (*ev == ' ' || *ev == '\t' || *ev == '\n' || *ev == ',') ev++;
                    if (*ev == ']') break;
                    if (*ev == '"') { ev++; const char *end = strchr(ev, '"'); if (!end) break;
                        pm.enumValues.push_back(std::string(ev, end - ev)); ev = end + 1;
                    } else break;
                }
            }}
        }

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
 * Device — a loaded RNBO bridge .so
 * ====================================================================== */

#define MAX_CH 2

struct RnboDevice {
    std::string name;
    std::string dir_path;
    std::string so_path;
    void *dl_handle;

    fn_create          api_create;
    fn_destroy         api_destroy;
    fn_get_info        api_get_info;
    fn_prepare         api_prepare;
    fn_process         api_process;
    fn_process_midi    api_process_midi;
    fn_midi_in         api_midi_in;
    fn_get_num_params  api_get_num_params;
    fn_get_param_info  api_get_param_info;
    fn_get_param_name  api_get_param_name;
    fn_get_param       api_get_param;
    fn_set_param       api_set_param;
    fn_send_msg_num    api_send_msg_num;
    fn_send_msg_list   api_send_msg_list;
    fn_set_msg_cb      api_set_msg_cb;
    fn_drain_msgs      api_drain_msgs;

    int num_input_channels;
    int num_output_channels;
    int num_midi_input_ports;
    int num_midi_output_ports;
    std::vector<ParamMeta> params;
    std::vector<std::string> inports;
    std::vector<std::string> outports;
    bool is_effect;
};

static bool load_device(RnboDevice &dev) {
    std::string desc_path = dev.dir_path + "/description.json";
    std::string desc = read_file(desc_path.c_str());
    if (desc.empty()) { host_log("rnbo: no description.json in %s", dev.dir_path.c_str()); return false; }

    dev.num_input_channels = json_get_int(desc.c_str(), "numInputChannels", 0);
    dev.num_output_channels = json_get_int(desc.c_str(), "numOutputChannels", 2);
    dev.num_midi_input_ports = json_get_int(desc.c_str(), "numMidiInputPorts", 0);
    dev.num_midi_output_ports = json_get_int(desc.c_str(), "numMidiOutputPorts", 0);
    dev.is_effect = (dev.num_input_channels > 0);
    dev.params = parse_params_from_description(desc.c_str());

    /* Parse inports/outports */
    const char *ip = strstr(desc.c_str(), "\"inports\"");
    if (ip) { ip = strchr(ip, '['); if (ip) { ip++;
        while (*ip) {
            const char *tag_start = strstr(ip, "\"tag\"");
            if (!tag_start) break;
            std::string tag = json_get_string(tag_start - 1, "tag");
            if (!tag.empty()) dev.inports.push_back(tag);
            ip = tag_start + 5;
            const char *next_brace = strchr(ip, '}');
            if (!next_brace) break;
            ip = next_brace + 1;
        }
    }}
    const char *op = strstr(desc.c_str(), "\"outports\"");
    if (op) { op = strchr(op, '['); if (op) { op++;
        while (*op) {
            const char *tag_start = strstr(op, "\"tag\"");
            if (!tag_start) break;
            std::string tag = json_get_string(tag_start - 1, "tag");
            if (!tag.empty()) dev.outports.push_back(tag);
            op = tag_start + 5;
            const char *next_brace = strchr(op, '}');
            if (!next_brace) break;
            op = next_brace + 1;
        }
    }}

    dev.dl_handle = dlopen(dev.so_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!dev.dl_handle) { host_log("rnbo: dlopen failed: %s", dlerror()); return false; }

    #define RESOLVE(name) dev.api_##name = (fn_##name)dlsym(dev.dl_handle, "rnbo_device_" #name)
    RESOLVE(create); RESOLVE(destroy); RESOLVE(get_info); RESOLVE(prepare);
    RESOLVE(process); RESOLVE(midi_in); RESOLVE(get_num_params);
    RESOLVE(get_param_info); RESOLVE(get_param_name); RESOLVE(get_param); RESOLVE(set_param);
    #undef RESOLVE
    /* Extended API */
    dev.api_process_midi = (fn_process_midi)dlsym(dev.dl_handle, "rnbo_device_process_with_midi");
    dev.api_send_msg_num = (fn_send_msg_num)dlsym(dev.dl_handle, "rnbo_device_send_message_number");
    dev.api_send_msg_list = (fn_send_msg_list)dlsym(dev.dl_handle, "rnbo_device_send_message_list");
    dev.api_set_msg_cb = (fn_set_msg_cb)dlsym(dev.dl_handle, "rnbo_device_set_message_callback");
    dev.api_drain_msgs = (fn_drain_msgs)dlsym(dev.dl_handle, "rnbo_device_drain_messages");

    if (!dev.api_create || !dev.api_destroy || !dev.api_prepare || !dev.api_process) {
        host_log("rnbo: missing C API in %s", dev.so_path.c_str());
        dlclose(dev.dl_handle); dev.dl_handle = nullptr; return false;
    }

    host_log("rnbo: loaded device '%s' (%din/%dout, %d params, %d/%d ports)",
             dev.name.c_str(), dev.num_input_channels, dev.num_output_channels,
             (int)dev.params.size(), (int)dev.inports.size(), (int)dev.outports.size());
    return true;
}

static void unload_device(RnboDevice &dev) {
    if (dev.dl_handle) { dlclose(dev.dl_handle); dev.dl_handle = nullptr; }
}

static std::vector<RnboDevice> scan_devices(const char *devices_dir) {
    std::vector<RnboDevice> devices;
    DIR *dir = opendir(devices_dir);
    if (!dir) return devices;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string dev_dir = std::string(devices_dir) + "/" + entry->d_name;
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
 * Graph structures
 * ====================================================================== */

enum ConnType { CONN_AUDIO, CONN_MIDI, CONN_MESSAGE };

struct GraphNode {
    int instance_index;
    std::string patcher_name;
    int device_index;     /* index into devices vector */
    rnbo_device_t handle; /* live device handle */

    /* Audio buffers */
    float in_buf[MAX_CH][MOVE_FRAMES_PER_BLOCK];
    float out_buf[MAX_CH][MOVE_FRAMES_PER_BLOCK];
    float *in_ptrs[MAX_CH];
    float *out_ptrs[MAX_CH];

    /* Pending MIDI for this node */
    std::vector<rnbo_midi_event_t> pending_midi;
};

struct Connection {
    ConnType type;
    int source_node;  /* -1 = system */
    int sink_node;    /* -1 = system */
    std::string source_port;
    std::string sink_port;
};

struct ViewParam {
    int node_index;
    int param_index;   /* index into the device's params vector */
    std::string key;   /* "inst_paramId" for chain UI */
    std::string label;
};

/* Message captured from an outport — stored per-node per-block */
struct CapturedMessage {
    std::string tag;
    std::vector<double> values;
};

struct RnboGraph {
    std::string name;
    std::vector<GraphNode> nodes;
    std::vector<Connection> connections;
    std::vector<int> process_order;
    int audio_output_node;
    std::vector<int> midi_input_nodes;
    std::vector<ViewParam> view_params;
    bool is_effect;
};

/* ======================================================================
 * Graph building from set JSON
 * ====================================================================== */

static ConnType classify_port(const std::string &port) {
    if (port.find("out1") != std::string::npos || port.find("out2") != std::string::npos ||
        port.find("in1") != std::string::npos || port.find("in2") != std::string::npos)
        return CONN_AUDIO;
    if (port.find("midi") != std::string::npos)
        return CONN_MIDI;
    return CONN_MESSAGE;
}

static bool build_graph(const std::string &set_json_str, std::vector<RnboDevice> &devices, RnboGraph &graph) {
    /* Parse instances */
    const char *json = set_json_str.c_str();
    graph.name = json_get_string(json, "name");
    if (graph.name.empty()) graph.name = "RNBO";

    /* Build name→device_index map */
    std::map<std::string, int> name_to_dev;
    for (size_t i = 0; i < devices.size(); i++)
        name_to_dev[devices[i].name] = (int)i;

    /* Parse instances array */
    const char *inst_arr = strstr(json, "\"instances\"");
    if (!inst_arr) return false;
    inst_arr = strchr(inst_arr, '[');
    if (!inst_arr) return false;

    std::map<int, int> inst_to_node; /* instance_index → graph node index */

    const char *p = inst_arr + 1;
    while (*p) {
        const char *obj_start = strchr(p, '{');
        if (!obj_start) break;
        const char *scan = obj_start + 1;
        int depth = 1;
        while (*scan && depth > 0) { if (*scan == '{') depth++; else if (*scan == '}') depth--; scan++; }
        if (depth != 0) break;
        std::string obj(obj_start, scan - obj_start);

        int inst_idx = json_get_int(obj.c_str(), "instance_index", -1);
        std::string patcher_name = json_get_string(obj.c_str(), "patcher_name");

        /* Match to a device by converting name to dir-name format */
        std::string dir_name;
        for (char c : patcher_name) {
            if (c == ' ' || c == '_') dir_name += '-';
            else if (c >= 'A' && c <= 'Z') dir_name += (c + 32);
            else dir_name += c;
        }

        auto it = name_to_dev.find(dir_name);
        if (it == name_to_dev.end()) {
            host_log("rnbo: graph instance '%s' (dir '%s') not found in devices", patcher_name.c_str(), dir_name.c_str());
            p = scan; continue;
        }

        GraphNode node;
        node.instance_index = inst_idx;
        node.patcher_name = patcher_name;
        node.device_index = it->second;
        node.handle = nullptr;
        for (int ch = 0; ch < MAX_CH; ch++) {
            node.in_ptrs[ch] = node.in_buf[ch];
            node.out_ptrs[ch] = node.out_buf[ch];
        }

        host_log("rnbo: graph node [%d] '%s' -> device %d '%s'",
                 inst_idx, patcher_name.c_str(), it->second, dir_name.c_str());

        int ni = (int)graph.nodes.size();
        inst_to_node[inst_idx] = ni;
        graph.nodes.push_back(std::move(node));

        p = scan;
        if (*p == ',') p++;
        while (*p == ' ' || *p == '\n' || *p == '\t') p++;
        if (*p == ']') break;
    }

    /* Parse connections */
    graph.audio_output_node = -1;
    graph.midi_input_nodes.clear();

    const char *conn_arr = strstr(json, "\"connections\"");
    if (conn_arr) {
        conn_arr = strchr(conn_arr, '[');
        if (conn_arr) {
            p = conn_arr + 1;
            while (*p) {
                const char *obj_start = strchr(p, '{');
                if (!obj_start) break;
                const char *scan = obj_start + 1;
                int depth = 1;
                while (*scan && depth > 0) { if (*scan == '{') depth++; else if (*scan == '}') depth--; scan++; }
                if (depth != 0) break;
                std::string obj(obj_start, scan - obj_start);

                int src_inst = json_get_int(obj.c_str(), "source_instance_index", -1);
                int snk_inst = json_get_int(obj.c_str(), "sink_instance_index", -1);
                std::string src_port = json_get_string(obj.c_str(), "source_port_name");
                std::string snk_port = json_get_string(obj.c_str(), "sink_port_name");
                std::string src_name = json_get_string(obj.c_str(), "source_name");
                std::string snk_name = json_get_string(obj.c_str(), "sink_name");

                /* Audio output to system */
                if (snk_name == "move-volume" && src_inst >= 0) {
                    auto it = inst_to_node.find(src_inst);
                    if (it != inst_to_node.end()) graph.audio_output_node = it->second;
                    p = scan; continue;
                }

                /* Skip record/system sinks */
                if (snk_name == "rnbo-record" || snk_name == "system") { p = scan; continue; }

                /* MIDI from pads */
                if (src_name == "move-control" && snk_inst >= 0) {
                    auto it = inst_to_node.find(snk_inst);
                    if (it != inst_to_node.end()) {
                        auto &v = graph.midi_input_nodes;
                        if (std::find(v.begin(), v.end(), it->second) == v.end())
                            v.push_back(it->second);
                    }
                    p = scan; continue;
                }

                /* Skip other system connections */
                if (src_inst < 0 || snk_inst < 0) { p = scan; continue; }

                auto src_it = inst_to_node.find(src_inst);
                auto snk_it = inst_to_node.find(snk_inst);
                if (src_it == inst_to_node.end() || snk_it == inst_to_node.end()) { p = scan; continue; }

                Connection c;
                c.source_node = src_it->second;
                c.sink_node = snk_it->second;
                c.source_port = src_port;
                c.sink_port = snk_port;
                c.type = classify_port(src_port);
                graph.connections.push_back(c);

                p = scan;
                if (*p == ',') p++;
                while (*p == ' ' || *p == '\n' || *p == '\t') p++;
                if (*p == ']') break;
            }
        }
    }

    /* Fix up buffer pointers — they were invalidated by vector moves */
    for (auto &node : graph.nodes) {
        for (int ch = 0; ch < MAX_CH; ch++) {
            node.in_ptrs[ch] = node.in_buf[ch];
            node.out_ptrs[ch] = node.out_buf[ch];
        }
    }

    /* Also route host MIDI to any node with MIDI input + audio output (synths).
     * The set JSON only wires move-control to MIDI processors like Pad Scale,
     * which may not forward MIDI. Synths need MIDI directly. */
    for (size_t ni = 0; ni < graph.nodes.size(); ni++) {
        auto &node_dev = devices[graph.nodes[ni].device_index];
        if (node_dev.num_midi_input_ports > 0 && node_dev.num_output_channels > 0) {
            auto &v = graph.midi_input_nodes;
            if (std::find(v.begin(), v.end(), (int)ni) == v.end()) {
                v.push_back((int)ni);
                host_log("rnbo: also routing host MIDI to '%s' (has midi+audio)",
                         graph.nodes[ni].patcher_name.c_str());
            }
        }
    }

    /* Parse views — use first view with params */
    const char *views_arr = strstr(json, "\"views\"");
    if (views_arr) {
        views_arr = strchr(views_arr, '[');
        if (views_arr) {
            p = views_arr + 1;
            while (*p) {
                const char *obj_start = strchr(p, '{');
                if (!obj_start) break;
                const char *scan = obj_start + 1;
                int depth = 1;
                while (*scan && depth > 0) { if (*scan == '{') depth++; else if (*scan == '}') depth--; scan++; }
                if (depth != 0) break;
                std::string obj(obj_start, scan - obj_start);

                /* Find params array in this view */
                const char *params_arr = strstr(obj.c_str(), "\"params\"");
                if (params_arr) {
                    params_arr = strchr(params_arr, '[');
                    if (params_arr) {
                        params_arr++;
                        while (*params_arr) {
                            while (*params_arr == ' ' || *params_arr == ',' || *params_arr == '\n') params_arr++;
                            if (*params_arr == ']') break;
                            if (*params_arr == '"') {
                                params_arr++;
                                const char *end = strchr(params_arr, '"');
                                if (!end) break;
                                std::string param_ref(params_arr, end - params_arr);
                                params_arr = end + 1;

                                /* Parse "instance_index:paramId" */
                                size_t colon = param_ref.find(':');
                                if (colon == std::string::npos) continue;
                                int ref_inst = atoi(param_ref.substr(0, colon).c_str());
                                std::string param_id = param_ref.substr(colon + 1);

                                auto it = inst_to_node.find(ref_inst);
                                if (it == inst_to_node.end()) continue;
                                int ni = it->second;
                                auto &dev = devices[graph.nodes[ni].device_index];

                                for (size_t pi = 0; pi < dev.params.size(); pi++) {
                                    if (dev.params[pi].paramId == param_id) {
                                        ViewParam vp;
                                        vp.node_index = ni;
                                        vp.param_index = (int)pi;
                                        /* Safe key: replace / with - */
                                        std::string safe_id;
                                        for (char c : param_id) safe_id += (c == '/') ? '-' : c;
                                        vp.key = std::to_string(ref_inst) + "_" + safe_id;
                                        vp.label = dev.params[pi].displayName.empty() ? dev.params[pi].name : dev.params[pi].displayName;
                                        graph.view_params.push_back(vp);
                                        break;
                                    }
                                }
                            } else break;
                        }
                    }
                }

                if (!graph.view_params.empty()) break; /* use first view with params */
                p = scan;
                if (*p == ',') p++;
                while (*p == ' ' || *p == '\n' || *p == '\t') p++;
                if (*p == ']') break;
            }
        }
    }

    /* Topological sort */
    int n = (int)graph.nodes.size();
    std::vector<std::vector<int>> adj(n);
    for (auto &c : graph.connections) {
        if (c.source_node >= 0 && c.sink_node >= 0)
            adj[c.source_node].push_back(c.sink_node);
    }
    for (int i = 0; i < n; i++) {
        std::sort(adj[i].begin(), adj[i].end());
        adj[i].erase(std::unique(adj[i].begin(), adj[i].end()), adj[i].end());
    }
    std::vector<int> in_deg(n, 0);
    for (int i = 0; i < n; i++) for (int j : adj[i]) in_deg[j]++;
    std::queue<int> q;
    for (int i = 0; i < n; i++) if (in_deg[i] == 0) q.push(i);
    graph.process_order.clear();
    while (!q.empty()) {
        int u = q.front(); q.pop();
        graph.process_order.push_back(u);
        for (int v : adj[u]) if (--in_deg[v] == 0) q.push(v);
    }
    for (int i = 0; i < n; i++)
        if (std::find(graph.process_order.begin(), graph.process_order.end(), i) == graph.process_order.end())
            graph.process_order.push_back(i);

    graph.is_effect = false;

    host_log("rnbo: graph '%s': %d nodes, %d connections, audio_out=%d, %d midi_in, %d view_params",
             graph.name.c_str(), n, (int)graph.connections.size(),
             graph.audio_output_node, (int)graph.midi_input_nodes.size(), (int)graph.view_params.size());
    return true;
}

/* ======================================================================
 * Message callback for capturing outport messages during process
 * ====================================================================== */

struct MsgCaptureCtx {
    std::vector<CapturedMessage> messages;
};

static void msg_capture_callback(void *context, const char *tag, const double *values, int num_values) {
    auto *ctx = (MsgCaptureCtx*)context;
    CapturedMessage msg;
    msg.tag = tag ? tag : "";
    for (int i = 0; i < num_values; i++) msg.values.push_back(values[i]);
    ctx->messages.push_back(std::move(msg));
}

/* ======================================================================
 * Instance
 * ====================================================================== */

struct RnboInstance {
    std::vector<RnboDevice> devices;
    RnboGraph graph;
    bool graph_mode;

    /* Per-node message capture contexts */
    std::vector<MsgCaptureCtx> node_msg_ctx;

    float volume;
    std::vector<rnbo_midi_event_t> pending_midi;

    std::string module_dir;
    std::string chain_params_json;
    std::string ui_hierarchy_json;
};

/* ======================================================================
 * JSON generation for chain UI
 * ====================================================================== */

static void build_param_json(std::string &j, const ParamMeta &pm, const std::string &key) {
    j += ",{\"key\":\"" + escape_json(key) + "\"";
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

static void build_graph_ui(RnboInstance *inst) {
    auto &g = inst->graph;

    /* If no view params, generate from all nodes' params */
    std::vector<ViewParam> *params = &g.view_params;
    std::vector<ViewParam> all_params;
    if (g.view_params.empty()) {
        for (size_t ni = 0; ni < g.nodes.size(); ni++) {
            auto &dev = inst->devices[g.nodes[ni].device_index];
            for (size_t pi = 0; pi < dev.params.size(); pi++) {
                auto &pm = dev.params[pi];
                ViewParam vp;
                vp.node_index = (int)ni;
                vp.param_index = (int)pi;
                std::string safe_id;
                for (char c : pm.paramId) safe_id += (c == '/') ? '-' : c;
                vp.key = std::to_string(g.nodes[ni].instance_index) + "_" + safe_id;
                vp.label = pm.displayName.empty() ? pm.name : pm.displayName;
                all_params.push_back(vp);
            }
        }
        params = &all_params;
    }

    /* chain_params — flat list of all params for chain registration */
    std::string cp = "[{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02,\"default\":0.7}";
    for (auto &vp : *params) {
        auto &pm = inst->devices[g.nodes[vp.node_index].device_index].params[vp.param_index];
        build_param_json(cp, pm, vp.key);
    }
    cp += "]";
    inst->chain_params_json = cp;

    /* ui_hierarchy — tree: root has volume + sublevels per patcher */
    std::string ui = "{\"levels\":{\"root\":{\"label\":\"" + escape_json(g.name) + "\",\"params\":[{\"key\":\"volume\",\"label\":\"Volume\"}";

    /* Collect which nodes have params */
    std::map<int, std::vector<const ViewParam*>> node_params;
    for (auto &vp : *params)
        node_params[vp.node_index].push_back(&vp);

    /* Add sublevel entry for each patcher with params */
    for (auto &[ni, vps] : node_params) {
        std::string level_id = "node_" + std::to_string(ni);
        std::string label = g.nodes[ni].patcher_name;
        ui += ",{\"level\":\"" + escape_json(level_id) + "\",\"label\":\"" + escape_json(label) + "\"}";
    }

    /* Knobs: volume + first params from view (or first node) */
    ui += "],\"knobs\":[\"volume\"";
    int k = 0;
    for (auto &vp : *params) {
        if (k++ >= 7) break;
        ui += ",\"" + escape_json(vp.key) + "\"";
    }
    ui += "]}";

    /* Define each patcher sublevel */
    for (auto &[ni, vps] : node_params) {
        std::string level_id = "node_" + std::to_string(ni);
        std::string label = g.nodes[ni].patcher_name;
        ui += ",\"" + escape_json(level_id) + "\":{\"label\":\"" + escape_json(label) + "\",\"params\":[";
        bool first = true;
        for (auto *vp : vps) {
            if (!first) ui += ",";
            first = false;
            ui += "{\"key\":\"" + escape_json(vp->key) + "\",\"label\":\"" + escape_json(vp->label) + "\"}";
        }
        ui += "],\"knobs\":[";
        int kn = 0;
        for (auto *vp : vps) {
            if (kn++ >= 8) break;
            if (kn > 1) ui += ",";
            ui += "\"" + escape_json(vp->key) + "\"";
        }
        ui += "]}";
    }

    ui += "}}";
    inst->ui_hierarchy_json = ui;
}

/* ======================================================================
 * Graph processing
 * ====================================================================== */

static void process_graph(RnboInstance *inst, int16_t *out_lr, int frames) {
    auto &g = inst->graph;

    /* Clear node buffers and pending MIDI */
    for (auto &node : g.nodes) {
        for (int ch = 0; ch < MAX_CH; ch++) {
            memset(node.in_buf[ch], 0, frames * sizeof(float));
            memset(node.out_buf[ch], 0, frames * sizeof(float));
        }
        node.pending_midi.clear();
    }

    /* Clear message capture contexts */
    for (auto &ctx : inst->node_msg_ctx) ctx.messages.clear();

    /* Inject host MIDI into designated nodes */
    if (!inst->pending_midi.empty()) {
        static int midi_dbg = 0;
        if (++midi_dbg <= 10)
            host_log("rnbo: injecting %d MIDI to %d nodes", (int)inst->pending_midi.size(), (int)g.midi_input_nodes.size());
    }
    for (auto &m : inst->pending_midi)
        for (int ni : g.midi_input_nodes)
            g.nodes[ni].pending_midi.push_back(m);
    inst->pending_midi.clear();

    /* Process nodes in topological order */
    for (int ni : g.process_order) {
        auto &node = g.nodes[ni];
        if (!node.handle) continue;
        auto &dev = inst->devices[node.device_index];

        /* Inject MIDI */
        if (dev.api_midi_in) {
            for (auto &m : node.pending_midi)
                dev.api_midi_in(node.handle, m.data, m.len);
        }

        /* Process audio with MIDI output capture */
        int nin = std::min(dev.num_input_channels, MAX_CH);
        int nout = std::min(dev.num_output_channels, MAX_CH);
        rnbo_midi_event_t midi_out[256];
        int midi_out_count = 0;

        if (dev.api_process_midi) {
            midi_out_count = dev.api_process_midi(node.handle,
                node.in_ptrs, nin, node.out_ptrs, nout, frames,
                midi_out, 256);
        } else {
            dev.api_process(node.handle, node.in_ptrs, nin, node.out_ptrs, nout, frames);
        }

        /* Drain outgoing messages (delivered to callback) */
        if (dev.api_drain_msgs)
            dev.api_drain_msgs(node.handle);

        /* Route audio and MIDI via explicit connections */
        for (auto &conn : g.connections) {
            if (conn.source_node != ni) continue;
            if (conn.sink_node < 0 || conn.sink_node >= (int)g.nodes.size()) continue;
            auto &sink = g.nodes[conn.sink_node];

            if (conn.type == CONN_AUDIO) {
                int src_ch = (conn.source_port.find("2") != std::string::npos) ? 1 : 0;
                int snk_ch = (conn.sink_port.find("2") != std::string::npos) ? 1 : 0;
                if (src_ch < nout && snk_ch < MAX_CH)
                    for (int i = 0; i < frames; i++)
                        sink.in_buf[snk_ch][i] += node.out_buf[src_ch][i];
            } else if (conn.type == CONN_MIDI) {
                for (int i = 0; i < midi_out_count; i++)
                    sink.pending_midi.push_back(midi_out[i]);
            }
        }

        /* Route messages via global bus: outport tag → all nodes with matching inport */
        auto &msgs = inst->node_msg_ctx[ni].messages;
        if (!msgs.empty()) {
            static int msg_dbg = 0;
            if (++msg_dbg <= 20)
                host_log("rnbo: node[%d] '%s' emitted %d messages", ni, node.patcher_name.c_str(), (int)msgs.size());
            for (auto &msg : msgs) {
                if (msg_dbg <= 20)
                    host_log("rnbo:   tag='%s' values=%d", msg.tag.c_str(), (int)msg.values.size());
            }
            for (auto &other_node : g.nodes) {
                if (!other_node.handle) continue;
                auto &other_dev = inst->devices[other_node.device_index];
                for (auto &msg : msgs) {
                    for (auto &inport : other_dev.inports) {
                        if (inport == msg.tag) {
                            if (msg.values.empty() && other_dev.api_send_msg_num)
                                other_dev.api_send_msg_num(other_node.handle, msg.tag.c_str(), 0);
                            else if (msg.values.size() == 1 && other_dev.api_send_msg_num)
                                other_dev.api_send_msg_num(other_node.handle, msg.tag.c_str(), msg.values[0]);
                            else if (msg.values.size() > 1 && other_dev.api_send_msg_list)
                                other_dev.api_send_msg_list(other_node.handle, msg.tag.c_str(), msg.values.data(), (int)msg.values.size());
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Debug: check audio output */
    static int audio_dbg = 0;
    if (++audio_dbg % 1000 == 1 && g.audio_output_node >= 0) {
        auto &out = g.nodes[g.audio_output_node];
        auto &dev = inst->devices[out.device_index];
        int nout = std::min(dev.num_output_channels, MAX_CH);
        float peak = 0;
        for (int ch = 0; ch < nout; ch++)
            for (int i = 0; i < frames; i++)
                if (fabsf(out.out_buf[ch][i]) > peak) peak = fabsf(out.out_buf[ch][i]);
        host_log("rnbo: audio_out node=%d peak=%.6f midi_pending=%d", g.audio_output_node, peak, (int)out.pending_midi.size());
    }

    /* Read audio from output node */
    memset(out_lr, 0, frames * 2 * sizeof(int16_t));
    if (g.audio_output_node >= 0 && g.audio_output_node < (int)g.nodes.size()) {
        auto &out = g.nodes[g.audio_output_node];
        auto &dev = inst->devices[out.device_index];
        int nout = std::min(dev.num_output_channels, MAX_CH);
        float vol = inst->volume;
        for (int i = 0; i < frames; i++) {
            float l = (nout > 0) ? out.out_buf[0][i] * vol : 0;
            float r = (nout > 1) ? out.out_buf[1][i] * vol : l;
            l = std::max(-1.0f, std::min(1.0f, l));
            r = std::max(-1.0f, std::min(1.0f, r));
            out_lr[i * 2 + 0] = (int16_t)(l * 32767.0f);
            out_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
        }
    }
}

/* ======================================================================
 * Plugin API v2
 * ====================================================================== */

static void* rnbo_create(const char *module_dir, const char *json_defaults) {
    auto *inst = new RnboInstance();
    inst->module_dir = module_dir;
    inst->volume = 0.7f;
    inst->graph_mode = false;

    std::string devices_dir = std::string(module_dir) + "/devices";
    inst->devices = scan_devices(devices_dir.c_str());

    /* Try to find and load set JSON for graph mode */
    /* Look in pack/ directory first, then module_dir */
    std::string set_json_str;
    std::string pack_dir = std::string(module_dir) + "/pack";
    DIR *pack = opendir(pack_dir.c_str());
    if (pack) {
        closedir(pack);
        /* Read info.json to find set path */
        std::string info_str = read_file((pack_dir + "/info.json").c_str());
        if (!info_str.empty()) {
            /* Find first set location */
            const char *sets = strstr(info_str.c_str(), "\"sets\"");
            if (sets) {
                sets = strchr(sets, '[');
                if (sets) {
                    std::string loc = json_get_string(sets, "location");
                    if (!loc.empty()) {
                        set_json_str = read_file((pack_dir + "/" + loc).c_str());
                    }
                }
            }
        }
    }

    if (!set_json_str.empty() && !inst->devices.empty()) {
        if (build_graph(set_json_str, inst->devices, inst->graph)) {
            inst->graph_mode = true;

            /* Create and prepare all node handles */
            inst->node_msg_ctx.resize(inst->graph.nodes.size());
            for (size_t ni = 0; ni < inst->graph.nodes.size(); ni++) {
                auto &node = inst->graph.nodes[ni];
                auto &dev = inst->devices[node.device_index];

                node.handle = dev.api_create();
                if (!node.handle) {
                    host_log("rnbo: create failed for node '%s'", node.patcher_name.c_str());
                    continue;
                }
                dev.api_prepare(node.handle, MOVE_SAMPLE_RATE, MOVE_FRAMES_PER_BLOCK);

                /* Set up message callback */
                if (dev.api_set_msg_cb)
                    dev.api_set_msg_cb(node.handle, msg_capture_callback, &inst->node_msg_ctx[ni]);

                /* Set midichan to omni */
                if (dev.api_set_param) {
                    for (auto &pm : dev.params) {
                        if (pm.paramId == "midichan")
                            dev.api_set_param(node.handle, pm.index, 0);
                    }
                }

                host_log("rnbo: graph node [%d] '%s' ready", node.instance_index, node.patcher_name.c_str());
            }

            build_graph_ui(inst);
            host_log("rnbo: graph '%s' ready (%d nodes)", inst->graph.name.c_str(), (int)inst->graph.nodes.size());
        }
    }

    if (!inst->graph_mode) {
        host_log("rnbo: no graph found, running in single-device mode");
        /* Fall back: if we have devices, just use the first audio-producing one */
        /* (backwards compat with non-graph rnbopack installs) */
    }

    return inst;
}

static void rnbo_destroy(void *instance) {
    auto *inst = (RnboInstance*)instance;
    if (!inst) return;
    for (auto &node : inst->graph.nodes) {
        if (node.handle) {
            auto &dev = inst->devices[node.device_index];
            if (dev.api_destroy) dev.api_destroy(node.handle);
        }
    }
    for (auto &dev : inst->devices) unload_device(dev);
    delete inst;
}

static void rnbo_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    auto *inst = (RnboInstance*)instance;
    if (!inst || len < 1) return;
    rnbo_midi_event_t m;
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

    /* Parse "instIdx_paramId" where paramId has / replaced with - */
    const char *sep = key;
    while (*sep >= '0' && *sep <= '9') sep++;
    if (*sep != '_') return;
    int target_inst = atoi(key);
    std::string safe_id(sep + 1);
    /* Convert back: - to / */
    std::string param_id;
    for (char c : safe_id) param_id += (c == '-') ? '/' : c;

    for (auto &node : inst->graph.nodes) {
        if (node.instance_index != target_inst || !node.handle) continue;
        auto &dev = inst->devices[node.device_index];
        for (auto &pm : dev.params) {
            if (pm.paramId == param_id) {
                if (dev.api_set_param)
                    dev.api_set_param(node.handle, pm.index, atof(val));
                return;
            }
        }
        break;
    }
}

static int rnbo_get_param(void *instance, const char *key, char *buf, int buf_len) {
    auto *inst = (RnboInstance*)instance;
    if (!inst || !key || !buf || buf_len < 1) return 0;

    if (strcmp(key, "volume") == 0) return snprintf(buf, buf_len, "%.2f", inst->volume);
    if (strcmp(key, "chain_params") == 0) return snprintf(buf, buf_len, "%s", inst->chain_params_json.c_str());
    if (strcmp(key, "ui_hierarchy") == 0) return snprintf(buf, buf_len, "%s", inst->ui_hierarchy_json.c_str());
    if (strcmp(key, "component_type") == 0) {
        return snprintf(buf, buf_len, "%s", inst->graph.is_effect ? "audio_fx" : "sound_generator");
    }

    /* Parse "instIdx_paramId" */
    const char *sep = key;
    while (*sep >= '0' && *sep <= '9') sep++;
    if (*sep != '_') return 0;
    int target_inst = atoi(key);
    std::string safe_id(sep + 1);
    std::string param_id;
    for (char c : safe_id) param_id += (c == '-') ? '/' : c;

    for (auto &node : inst->graph.nodes) {
        if (node.instance_index != target_inst || !node.handle) continue;
        auto &dev = inst->devices[node.device_index];
        for (auto &pm : dev.params) {
            if (pm.paramId == param_id) {
                double v = dev.api_get_param ? dev.api_get_param(node.handle, pm.index) : 0.0;
                return snprintf(buf, buf_len, "%.6g", v);
            }
        }
        break;
    }
    return 0;
}

static int rnbo_get_error(void *instance, char *buf, int buf_len) {
    auto *inst = (RnboInstance*)instance;
    if (!inst) return 0;
    if (inst->graph.nodes.empty() && inst->devices.empty())
        return snprintf(buf, buf_len, "No RNBO devices found");
    return 0;
}

static void rnbo_render_block(void *instance, int16_t *out_lr, int frames) {
    auto *inst = (RnboInstance*)instance;
    if (!inst) { memset(out_lr, 0, frames * 2 * sizeof(int16_t)); return; }

    if (inst->graph_mode) {
        process_graph(inst, out_lr, frames);
    } else {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
    }
}

/* ======================================================================
 * Entry points
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
    host_log("rnbo: graph host initialized");
    return &g_api;
}

static audio_fx_api_v2_t g_fx_api;

extern "C" audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    g_fx_api.api_version = AUDIO_FX_API_VERSION_2;
    g_fx_api.create_instance = rnbo_create;
    g_fx_api.destroy_instance = rnbo_destroy;
    g_fx_api.process_block = rnbo_render_block;
    g_fx_api.set_param = rnbo_set_param;
    g_fx_api.get_param = rnbo_get_param;
    g_fx_api.on_midi = rnbo_on_midi;
    host_log("rnbo: graph host (audio FX) initialized");
    return &g_fx_api;
}
