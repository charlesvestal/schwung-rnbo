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

#define RNBO_NO_PATCHERFACTORY
#include "RNBO.h"
#include "rnbo_device_api.h"

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

extern "C" {

rnbo_device_t rnbo_device_create(void) {
    if (!ensure_factory()) return nullptr;
    RNBO::UniquePtr<RNBO::PatcherInterface> patcher(g_factory());
    if (!patcher) return nullptr;
    auto *obj = new RNBO::CoreObject(std::move(patcher));
    return (rnbo_device_t)obj;
}

void rnbo_device_destroy(rnbo_device_t dev) {
    if (dev) delete (RNBO::CoreObject*)dev;
}

void rnbo_device_get_info(rnbo_device_t dev, rnbo_device_info_t *info) {
    if (!dev || !info) return;
    auto *obj = (RNBO::CoreObject*)dev;
    info->num_input_channels = (int)obj->getNumInputChannels();
    info->num_output_channels = (int)obj->getNumOutputChannels();
    info->num_parameters = (int)obj->getNumParameters();
    info->num_midi_input_ports = (int)obj->getNumMidiInputPorts();
    info->num_midi_output_ports = (int)obj->getNumMidiOutputPorts();
}

void rnbo_device_prepare(rnbo_device_t dev, double sample_rate, int max_block_size) {
    if (!dev) return;
    ((RNBO::CoreObject*)dev)->prepareToProcess(sample_rate, max_block_size, true);
}

void rnbo_device_process(rnbo_device_t dev,
                         float **inputs, int num_inputs,
                         float **outputs, int num_outputs,
                         int frames) {
    if (!dev) return;
    /* CoreObject uses double (SampleValue), host uses float — convert */
    RNBO::SampleValue in_bufs[2][128], out_bufs[2][128];
    RNBO::SampleValue *in_ptrs[2] = { in_bufs[0], in_bufs[1] };
    RNBO::SampleValue *out_ptrs[2] = { out_bufs[0], out_bufs[1] };

    for (int ch = 0; ch < num_inputs && ch < 2; ch++)
        for (int i = 0; i < frames; i++)
            in_bufs[ch][i] = (RNBO::SampleValue)inputs[ch][i];

    for (int ch = 0; ch < num_outputs && ch < 2; ch++)
        memset(out_bufs[ch], 0, frames * sizeof(RNBO::SampleValue));

    ((RNBO::CoreObject*)dev)->process(
        (const RNBO::SampleValue* const*)in_ptrs, (RNBO::Index)num_inputs,
        (RNBO::SampleValue* const*)out_ptrs, (RNBO::Index)num_outputs,
        (RNBO::Index)frames);

    for (int ch = 0; ch < num_outputs && ch < 2; ch++)
        for (int i = 0; i < frames; i++)
            outputs[ch][i] = (float)out_bufs[ch][i];
}

void rnbo_device_midi_in(rnbo_device_t dev, const uint8_t *data, int len) {
    if (!dev || !data || len < 1) return;
    RNBO::MidiEvent evt(RNBO::RNBOTimeNow, 0, data, (RNBO::Index)len);
    ((RNBO::CoreObject*)dev)->scheduleEvent(evt);
}

int rnbo_device_get_num_params(rnbo_device_t dev) {
    if (!dev) return 0;
    return (int)((RNBO::CoreObject*)dev)->getNumParameters();
}

void rnbo_device_get_param_info(rnbo_device_t dev, int index, rnbo_param_info_t *info) {
    if (!dev || !info) return;
    auto *obj = (RNBO::CoreObject*)dev;
    RNBO::ParameterInfo pi;
    obj->getParameterInfo((RNBO::ParameterIndex)index, &pi);
    info->name = obj->getParameterName((RNBO::ParameterIndex)index);
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
    return ((RNBO::CoreObject*)dev)->getParameterName((RNBO::ParameterIndex)index);
}

double rnbo_device_get_param(rnbo_device_t dev, int index) {
    if (!dev) return 0.0;
    return (double)((RNBO::CoreObject*)dev)->getParameterValue((RNBO::ParameterIndex)index);
}

void rnbo_device_set_param(rnbo_device_t dev, int index, double value) {
    if (!dev) return;
    ((RNBO::CoreObject*)dev)->setParameterValue((RNBO::ParameterIndex)index, (RNBO::ParameterValue)value);
}

} /* extern "C" */
