/*
 * RNBO Device Wrapper
 *
 * This file is appended to the build wrapper after RNBO.cpp and the patcher.
 * It exports a flat C API so the host module can call into the device
 * without cross-SO vtable issues.
 *
 * Uses RNBO::CoreObject to properly initialize the patcher with an Engine.
 */

#include "rnbo_device_api.h"

/* RNBO headers are already included via RNBO.cpp above */

extern "C" RNBO::PatcherFactoryFunctionPtr GetPatcherFactoryFunction(RNBO::PlatformInterface*);

extern "C" {

rnbo_device_t rnbo_device_create(void) {
    auto factory = GetPatcherFactoryFunction(RNBO::Platform::get());
    if (!factory) return nullptr;
    RNBO::UniquePtr<RNBO::PatcherInterface> patcher(factory());
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
    auto *obj = (RNBO::CoreObject*)dev;
    obj->prepareToProcess(sample_rate, max_block_size, true);
}

void rnbo_device_process(rnbo_device_t dev,
                         float **inputs, int num_inputs,
                         float **outputs, int num_outputs,
                         int frames) {
    if (!dev) return;
    auto *obj = (RNBO::CoreObject*)dev;
    obj->process((RNBO::SampleValue**)inputs, (RNBO::Index)num_inputs,
                 (RNBO::SampleValue**)outputs, (RNBO::Index)num_outputs,
                 (RNBO::Index)frames);
}

void rnbo_device_midi_in(rnbo_device_t dev, const uint8_t *data, int len) {
    if (!dev || !data || len < 1) return;
    auto *obj = (RNBO::CoreObject*)dev;
    RNBO::MidiEvent evt(RNBO::RNBOTimeNow, 0, data, (RNBO::Index)len);
    obj->scheduleEvent(evt);
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
