#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct audio_proc audio_proc_t;

int  audio_proc_create(audio_proc_t **p, bool enable_hpf, float gain_db);

/* Process samples in-place. Updates *clip_count_out with new clip events. */
void audio_proc_run(audio_proc_t *p, int16_t *samples, size_t count,
                    uint64_t *clip_count_out);

/* Peak and RMS since last call (rolling window). Values in dBFS. */
void audio_proc_get_levels(const audio_proc_t *p,
                           float *peak_dbfs_out, float *rms_dbfs_out);

void audio_proc_destroy(audio_proc_t *p);
