#pragma once

#include "config.h"
#include "audio_alsa.h"
#include "audio_processing.h"
#include "logic_hid.h"
#include "jitter_buffer.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct telemetry telemetry_t;

int  telemetry_create(telemetry_t **tel, const config_t *cfg);

/* Call on the rising edge of input_active (capture thread). */
void telemetry_input_start(telemetry_t *tel);

/* Call on the falling edge of input_active (capture thread). */
void telemetry_input_end(telemetry_t *tel, audio_alsa_t *alsa, audio_proc_t *in_proc);

/* Call on the rising edge of output_active (playback thread). */
void telemetry_output_start(telemetry_t *tel);

/* Call on the falling edge of output_active (playback thread).
 * jitter_buffer_latch_silence() must be called before this. */
void telemetry_output_end(telemetry_t *tel, audio_alsa_t *alsa,
                          audio_proc_t *out_proc, jitter_buffer_t *jb);

/* Periodic heartbeat — call from the main loop. */
void telemetry_log(telemetry_t *tel,
                   audio_alsa_t          *alsa,
                   audio_proc_t          *in_proc,
                   audio_proc_t          *out_proc,
                   const logic_hid_t     *logic,
                   jitter_buffer_t       *jb);

void telemetry_destroy(telemetry_t *tel);
