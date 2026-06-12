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

/* Number of playout slots where no frame was available (silence injected) since last call (resets counter). */
uint64_t jitter_buffer_silence_count(jitter_buffer_t *jb);

void jitter_buffer_destroy(jitter_buffer_t *jb);
