#pragma once

#include "config.h"
#include "audio_alsa.h"
#include "audio_processing.h"
#include "logic_hid.h"
#include "jitter_buffer.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    /* audio_input */
    float    audio_input_peak_dbfs;
    float    audio_input_rms_dbfs;
    uint64_t audio_input_clip_count;

    /* audio_output */
    float    audio_output_peak_dbfs;
    float    audio_output_rms_dbfs;
    uint64_t audio_output_clip_count;

    /* ALSA */
    uint64_t alsa_capture_overruns;
    uint64_t alsa_playback_underruns;

    /* logic */
    bool     input_active_state;
    bool     output_active_state;

    /* network */
    uint64_t network_packets_received;
    uint64_t network_packets_sent;
    float    network_packet_loss_estimate;
    float    network_jitter_ms;
    uint64_t last_packet_age_ms;

    /* watchdog */
    uint64_t output_watchdog_events;
} telemetry_snapshot_t;

typedef struct telemetry telemetry_t;

int  telemetry_create(telemetry_t **tel, const config_t *cfg);
void telemetry_log(telemetry_t *tel,
                   const audio_alsa_t      *alsa,
                   const audio_proc_t      *in_proc,
                   const audio_proc_t      *out_proc,
                   const logic_hid_t       *logic,
                   const jitter_buffer_t   *jb);
void telemetry_destroy(telemetry_t *tel);
