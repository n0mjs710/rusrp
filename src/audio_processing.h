#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct audio_proc audio_proc_t;

int  audio_proc_create(audio_proc_t **p, bool enable_hpf, float gain_db);

/* Process samples in-place.  Clips are counted internally; read with
 * audio_proc_clip_count(). */
void audio_proc_run(audio_proc_t *p, int16_t *samples, size_t count);

/* Peak and RMS since last reset. Values in dBFS. */
void audio_proc_get_levels(const audio_proc_t *p,
                           float *peak_dbfs_out, float *rms_dbfs_out);

/* Reset peak and RMS accumulators (call after reading to get per-transmission stats). */
void audio_proc_reset_levels(audio_proc_t *p);

/* Return and reset the clip counter (samples clamped since last call). */
uint64_t audio_proc_clip_count(audio_proc_t *p);

void audio_proc_destroy(audio_proc_t *p);
