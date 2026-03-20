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

/* ======================================================================
 * Extended API for graph support
 * ====================================================================== */

/* MIDI event for process_with_midi */
typedef struct {
    uint8_t data[3];
    int len;
} rnbo_midi_event_t;

/* Process audio and capture MIDI output.
 * midi_out: pre-allocated array to receive MIDI events
 * midi_out_capacity: max events to capture
 * Returns: number of MIDI events written to midi_out */
int rnbo_device_process_with_midi(rnbo_device_t dev,
                                   float **inputs, int num_inputs,
                                   float **outputs, int num_outputs,
                                   int frames,
                                   rnbo_midi_event_t *midi_out, int midi_out_capacity);

/* Send a number message to a named inport (e.g., "/rnbo/scl") */
void rnbo_device_send_message_number(rnbo_device_t dev, const char *tag, double value);

/* Send a list message to a named inport */
void rnbo_device_send_message_list(rnbo_device_t dev, const char *tag,
                                    const double *values, int num_values);

/* Message output callback — captures outport messages after drain */
typedef void (*rnbo_message_callback_t)(void *context, const char *tag,
                                         const double *values, int num_values);
void rnbo_device_set_message_callback(rnbo_device_t dev,
                                       rnbo_message_callback_t callback, void *context);

/* Drain pending output events (messages). Call AFTER process to capture outport messages. */
void rnbo_device_drain_messages(rnbo_device_t dev);

#ifdef __cplusplus
}
#endif

#endif /* RNBO_DEVICE_API_H */
