#pragma once

#include <stdint.h>
#include <stdbool.h>

#define JITTER_BUF_MIN_MS   40u
#define JITTER_BUF_MAX_MS   250u
#define JITTER_BUF_DEFAULT  100u

typedef struct jitter_buffer jitter_buffer_t;

int  jitter_buffer_create(jitter_buffer_t **jb, unsigned int depth_ms);

/* Push a 160-sample voice frame by USRP sequence number.
 * Late arrivals (seq already past playout cursor) are dropped. */
void jitter_buffer_push(jitter_buffer_t *jb, uint32_t seq,
                        const int16_t *samples);

/* Pull the next 160-sample frame for playout.
 * Returns true if a real frame was available; false if silence was injected. */
bool jitter_buffer_pull(jitter_buffer_t *jb, int16_t *samples_out);

/* Drain all pending frames (call on UNKEY). */
void jitter_buffer_flush(jitter_buffer_t *jb);

/* Estimated current jitter in milliseconds. */
float jitter_buffer_estimate_ms(const jitter_buffer_t *jb);

/* Number of packets dropped as late/out-of-window since last call (resets counter). */
uint64_t jitter_buffer_late_count(jitter_buffer_t *jb);

/* Reset the live silence counter — call at the rising edge of output_active so
 * pre-transmission idle pulls don't inflate the per-transmission count. */
void jitter_buffer_reset_silence_count(jitter_buffer_t *jb);

/* Latch the current silence count into a stable snapshot for output-end logging,
 * and accumulate it into the heartbeat bucket.  Call at the falling edge of
 * output_active (in the playback thread) before telemetry polls. */
void jitter_buffer_latch_silence(jitter_buffer_t *jb);

/* Return (and reset) the latched per-transmission silence count.
 * Use for output-end log lines — reflects only mid-transmission misses. */
uint64_t jitter_buffer_latched_silence_count(jitter_buffer_t *jb);

/* Return (and reset) the accumulated mid-transmission silence since the last
 * heartbeat read.  Use for heartbeat log lines. */
uint64_t jitter_buffer_hb_silence_count(jitter_buffer_t *jb);

void jitter_buffer_destroy(jitter_buffer_t *jb);
