#pragma once

#include "config.h"
#include <stdint.h>
#include <stddef.h>

/* Called by the capture thread with each 160-sample frame. */
typedef void (*audio_capture_cb_t)(const int16_t *samples, size_t count,
                                   void *userdata);

typedef struct audio_alsa audio_alsa_t;

int  audio_alsa_create(audio_alsa_t **a, const config_t *cfg,
                       audio_capture_cb_t cap_cb, void *userdata);

/* Write 160-sample frame to playback. Called from the playback thread.
 * If a drain was requested, drops the ALSA buffer before writing. */
int  audio_alsa_write(audio_alsa_t *a, const int16_t *samples, size_t count);

/* Request a playback drain: next audio_alsa_write call drops any queued
 * audio and restarts the stream. Thread-safe; call from any thread. */
void audio_alsa_request_drain(audio_alsa_t *a);

/* Read and reset per-call overrun/underrun counters. */
void audio_alsa_get_stats(audio_alsa_t *a,
                          uint64_t *overruns_out, uint64_t *underruns_out);

void audio_alsa_destroy(audio_alsa_t *a);
