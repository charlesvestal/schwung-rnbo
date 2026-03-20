/*
 * RNBO Bridge — wraps a rnbopack patcher .so in the flat C device API.
 *
 * This bridge is compiled with the RNBO SDK matching the patcher version.
 * It dlopen's "patcher.so" from its own directory, calls GetPatcherFactoryFunction,
 * creates a CoreObject, and exports the rnbo_device_* flat C API that the host expects.
 *
 * Each device directory contains:
 *   device.so       — this bridge (same binary for all devices)
 *   patcher.so      — the rnbopack pre-compiled patcher
 *   description.json — parameter metadata
 */

#include <dlfcn.h>
#include <string.h>
#include <stdio.h>

#include <vector>

#define RNBO_NO_PATCHERFACTORY
#include "RNBO.h"
#include "rnbo_device_api.h"

/* Runtime version of RNBO::TAG() hash for dynamic tag strings */
static RNBO::MessageTag runtime_tag(const char *str) {
    RNBO::MessageTag hash = 0;
    while (*str) {
        hash = (RNBO::MessageTag)(*str) + (hash << 6) + (hash << 16) - hash;
        str++;
    }
    return hash;
}

/* Resolve our own .so path to find patcher.so next to us */
static const char* get_self_dir() {
    Dl_info info;
    if (dladdr((void*)get_self_dir, &info) && info.dli_fname) {
        static char dir[4096];
        strncpy(dir, info.dli_fname, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *last_slash = strrchr(dir, '/');
        if (last_slash) *last_slash = '\0';
        return dir;
    }
    return ".";
}

/* Globals for the patcher .so */
static void *g_patcher_dl = nullptr;
static RNBO::PatcherFactoryFunctionPtr g_factory = nullptr;

typedef RNBO::PatcherFactoryFunctionPtr (*GetFactoryFn)(void);

static bool ensure_factory() {
    if (g_factory) return true;
    if (g_patcher_dl) return false;

    char path[4096];
    snprintf(path, sizeof(path), "%s/patcher.so", get_self_dir());
    g_patcher_dl = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!g_patcher_dl) {
        fprintf(stderr, "rnbo_bridge: dlopen(%s) failed: %s\n", path, dlerror());
        return false;
    }

    auto fn = (GetFactoryFn)dlsym(g_patcher_dl, "GetPatcherFactoryFunction");
    if (!fn) {
        fprintf(stderr, "rnbo_bridge: no GetPatcherFactoryFunction\n");
        return false;
    }
    g_factory = fn();
    return g_factory != nullptr;
}

/* Wrapper that holds CoreObject + EventHandler for output capture */
struct BridgeDevice : public RNBO::EventHandler {
    RNBO::CoreObject *core;
    rnbo_message_callback_t msg_callback;
    void *msg_context;

    /* Captured MIDI output from last process call */
    rnbo_midi_event_t midi_out_buf[256];
    int midi_out_count;

    BridgeDevice(RNBO::CoreObject *c) : core(c), msg_callback(nullptr), msg_context(nullptr), midi_out_count(0) {}
    ~BridgeDevice() { delete core; }

    void eventsAvailable() override { /* don't auto-drain — let process() handle events */ }

    void handleMidiEvent(const RNBO::MidiEvent& event) override {
        if (midi_out_count < 256) {
            auto &out = midi_out_buf[midi_out_count];
            const uint8_t *d = event.getData();
            RNBO::Index len = event.getLength();
            out.len = (len > 3) ? 3 : (int)len;
            out.data[0] = (len > 0) ? d[0] : 0;
            out.data[1] = (len > 1) ? d[1] : 0;
            out.data[2] = (len > 2) ? d[2] : 0;
            midi_out_count++;
        }
    }

    void handleMessageEvent(const RNBO::MessageEvent& event) override {
        if (!msg_callback) return;
        RNBO::MessageTagInfo info = core->resolveTag(event.getTag());
        if (!info) return;
        if (event.getType() == RNBO::MessageEvent::Number) {
            double val = event.getNumValue();
            msg_callback(msg_context, info, &val, 1);
        } else if (event.getType() == RNBO::MessageEvent::List) {
            auto listVal = event.getListValue();
            if (listVal) {
                /* Convert RNBO list to double array */
                std::vector<double> vals;
                for (RNBO::Index i = 0; i < (RNBO::Index)listVal->length; i++)
                    vals.push_back((*listVal)[i]);
                msg_callback(msg_context, info, vals.data(), (int)vals.size());
            }
        } else if (event.getType() == RNBO::MessageEvent::Bang) {
            msg_callback(msg_context, info, nullptr, 0);
        }
    }
};

extern "C" {

rnbo_device_t rnbo_device_create(void) {
    if (!ensure_factory()) return nullptr;
    RNBO::UniquePtr<RNBO::PatcherInterface> patcher(g_factory());
    if (!patcher) return nullptr;
    auto *core = new RNBO::CoreObject(std::move(patcher));
    auto *dev = new BridgeDevice(core);
    return (rnbo_device_t)dev;
}

void rnbo_device_destroy(rnbo_device_t dev) {
    if (dev) delete (BridgeDevice*)dev;
}

#define DEV(d) ((BridgeDevice*)(d))
#define CORE(d) (DEV(d)->core)

void rnbo_device_get_info(rnbo_device_t dev, rnbo_device_info_t *info) {
    if (!dev || !info) return;
    info->num_input_channels = (int)CORE(dev)->getNumInputChannels();
    info->num_output_channels = (int)CORE(dev)->getNumOutputChannels();
    info->num_parameters = (int)CORE(dev)->getNumParameters();
    info->num_midi_input_ports = (int)CORE(dev)->getNumMidiInputPorts();
    info->num_midi_output_ports = (int)CORE(dev)->getNumMidiOutputPorts();
}

void rnbo_device_prepare(rnbo_device_t dev, double sample_rate, int max_block_size) {
    if (!dev) return;
    CORE(dev)->prepareToProcess(sample_rate, max_block_size, true);
}

/* Internal: float<->double conversion + process */
static void bridge_process(BridgeDevice *bd, float **inputs, int num_inputs,
                           float **outputs, int num_outputs, int frames,
                           RNBO::MidiEventList *midi_out) {
    RNBO::SampleValue in_bufs[2][128], out_bufs[2][128];
    RNBO::SampleValue *in_ptrs[2] = { in_bufs[0], in_bufs[1] };
    RNBO::SampleValue *out_ptrs[2] = { out_bufs[0], out_bufs[1] };

    for (int ch = 0; ch < num_inputs && ch < 2; ch++)
        for (int i = 0; i < frames; i++)
            in_bufs[ch][i] = (RNBO::SampleValue)inputs[ch][i];

    for (int ch = 0; ch < num_outputs && ch < 2; ch++)
        memset(out_bufs[ch], 0, frames * sizeof(RNBO::SampleValue));

    bd->core->process(
        (const RNBO::SampleValue* const*)in_ptrs, (RNBO::Index)num_inputs,
        (RNBO::SampleValue* const*)out_ptrs, (RNBO::Index)num_outputs,
        (RNBO::Index)frames,
        nullptr, midi_out);

    for (int ch = 0; ch < num_outputs && ch < 2; ch++)
        for (int i = 0; i < frames; i++)
            outputs[ch][i] = (float)out_bufs[ch][i];
}

void rnbo_device_process(rnbo_device_t dev,
                         float **inputs, int num_inputs,
                         float **outputs, int num_outputs,
                         int frames) {
    if (!dev) return;
    bridge_process(DEV(dev), inputs, num_inputs, outputs, num_outputs, frames, nullptr);
}

int rnbo_device_process_with_midi(rnbo_device_t dev,
                                   float **inputs, int num_inputs,
                                   float **outputs, int num_outputs,
                                   int frames,
                                   rnbo_midi_event_t *midi_out, int midi_out_capacity) {
    if (!dev) return 0;
    auto *bd = DEV(dev);
    bd->midi_out_count = 0;

    RNBO::MidiEventList rnbo_midi_out;
    bridge_process(bd, inputs, num_inputs, outputs, num_outputs, frames, &rnbo_midi_out);

    /* Copy MIDI output from EventHandler capture */
    int count = 0;
    if (midi_out) {
        /* Events captured by the EventHandler during drainEvents */
        for (int i = 0; i < bd->midi_out_count && count < midi_out_capacity; i++) {
            midi_out[count++] = bd->midi_out_buf[i];
        }
        /* Events from the process() midiOutput list */
        for (auto it = rnbo_midi_out.begin(); it != rnbo_midi_out.end() && count < midi_out_capacity; ++it) {
            auto &evt = *it;
            const uint8_t *d = evt.getData();
            RNBO::Index len = evt.getLength();
            midi_out[count].len = (len > 3) ? 3 : (int)len;
            midi_out[count].data[0] = (len > 0) ? d[0] : 0;
            midi_out[count].data[1] = (len > 1) ? d[1] : 0;
            midi_out[count].data[2] = (len > 2) ? d[2] : 0;
            count++;
        }
    }
    return count;
}

void rnbo_device_midi_in(rnbo_device_t dev, const uint8_t *data, int len) {
    if (!dev || !data || len < 1) return;
    RNBO::MidiEvent evt(RNBO::RNBOTimeNow, 0, data, (RNBO::Index)len);
    CORE(dev)->scheduleEvent(evt);
}

void rnbo_device_send_message_number(rnbo_device_t dev, const char *tag, double value) {
    if (!dev || !tag) return;
    CORE(dev)->sendMessage(runtime_tag(tag), value, 0, RNBO::RNBOTimeNow);
}

void rnbo_device_send_message_list(rnbo_device_t dev, const char *tag,
                                    const double *values, int num_values) {
    if (!dev || !tag || !values || num_values < 1) return;
    auto listPtr = RNBO::make_unique<RNBO::list>();
    for (int i = 0; i < num_values; i++)
        listPtr->push(values[i]);
    CORE(dev)->sendMessage(runtime_tag(tag), std::move(listPtr), 0, RNBO::RNBOTimeNow);
}

void rnbo_device_set_message_callback(rnbo_device_t dev,
                                       rnbo_message_callback_t callback, void *context) {
    if (!dev) return;
    DEV(dev)->msg_callback = callback;
    DEV(dev)->msg_context = context;
}

int rnbo_device_get_num_params(rnbo_device_t dev) {
    if (!dev) return 0;
    return (int)CORE(dev)->getNumParameters();
}

void rnbo_device_get_param_info(rnbo_device_t dev, int index, rnbo_param_info_t *info) {
    if (!dev || !info) return;
    RNBO::ParameterInfo pi;
    CORE(dev)->getParameterInfo((RNBO::ParameterIndex)index, &pi);
    info->name = CORE(dev)->getParameterName((RNBO::ParameterIndex)index);
    info->display_name = pi.displayName;
    info->unit = pi.unit;
    info->min = pi.min;
    info->max = pi.max;
    info->initial_value = pi.initialValue;
    info->exponent = pi.exponent;
    info->steps = pi.steps;
    info->is_enum = (pi.enumValues != nullptr) ? 1 : 0;
    info->enum_values = pi.enumValues;
    info->num_enum_values = 0;
    if (pi.enumValues) {
        while (pi.enumValues[info->num_enum_values]) info->num_enum_values++;
    }
}

const char* rnbo_device_get_param_name(rnbo_device_t dev, int index) {
    if (!dev) return "";
    return CORE(dev)->getParameterName((RNBO::ParameterIndex)index);
}

double rnbo_device_get_param(rnbo_device_t dev, int index) {
    if (!dev) return 0.0;
    return (double)CORE(dev)->getParameterValue((RNBO::ParameterIndex)index);
}

void rnbo_device_set_param(rnbo_device_t dev, int index, double value) {
    if (!dev) return;
    CORE(dev)->setParameterValue((RNBO::ParameterIndex)index, (RNBO::ParameterValue)value);
}

#undef DEV
#undef CORE

} /* extern "C" */
