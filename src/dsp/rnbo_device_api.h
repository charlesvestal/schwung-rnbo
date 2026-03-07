/*
 * RNBO Device C API
 *
 * Thin C interface exported by each compiled RNBO device .so.
 * Avoids vtable/ABI issues between host and device shared libraries.
 */

#ifndef RNBO_DEVICE_API_H
#define RNBO_DEVICE_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to an RNBO patcher instance */
typedef void* rnbo_device_t;

/* Device info (filled by rnbo_device_get_info) */
typedef struct {
    int num_input_channels;
    int num_output_channels;
    int num_parameters;
    int num_midi_input_ports;
    int num_midi_output_ports;
} rnbo_device_info_t;

/* Parameter info */
typedef struct {
    const char *name;
    const char *display_name;
    const char *unit;
    double min;
    double max;
    double initial_value;
    double exponent;
    int steps;          /* 0=continuous, 1=toggle, 2+=stepped */
    int is_enum;
    const char **enum_values;
    int num_enum_values;
} rnbo_param_info_t;

/* Create a new patcher instance */
rnbo_device_t rnbo_device_create(void);

/* Destroy a patcher instance */
void rnbo_device_destroy(rnbo_device_t dev);

/* Get device info */
void rnbo_device_get_info(rnbo_device_t dev, rnbo_device_info_t *info);

/* Prepare for processing */
void rnbo_device_prepare(rnbo_device_t dev, double sample_rate, int max_block_size);

/* Process audio (non-interleaved float buffers) */
void rnbo_device_process(rnbo_device_t dev,
                         float **inputs, int num_inputs,
                         float **outputs, int num_outputs,
                         int frames);

/* Send MIDI event */
void rnbo_device_midi_in(rnbo_device_t dev, const uint8_t *data, int len);

/* Get number of parameters */
int rnbo_device_get_num_params(rnbo_device_t dev);

/* Get parameter info */
void rnbo_device_get_param_info(rnbo_device_t dev, int index, rnbo_param_info_t *info);

/* Get parameter name */
const char* rnbo_device_get_param_name(rnbo_device_t dev, int index);

/* Get parameter value */
double rnbo_device_get_param(rnbo_device_t dev, int index);

/* Set parameter value */
void rnbo_device_set_param(rnbo_device_t dev, int index, double value);

#ifdef __cplusplus
}
#endif

#endif /* RNBO_DEVICE_API_H */
