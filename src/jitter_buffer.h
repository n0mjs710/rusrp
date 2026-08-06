#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Plain arrival-order FIFO for USRP-RX audio, not a sequence-number-windowed
 * de-jitter buffer. The previous design policed incoming USRP sequence
 * numbers against a fixed playout window; the moment ASL3's numbering did
 * anything unexpected, that window could not resync itself -- every real
 * packet arriving outside it got discarded as "late" and every pull
 * returned silence, for the rest of a transmission (same failure mode
 * diagnosed and fixed in the sibling usrp-rc project's jitter_buffer.c on
 * 2026-08-05; ported here for the same reason).
 *
 * No priming ramp is needed here (unlike usrp-rc's version of this fix):
 * watchdog.c already defers asserting output_active (PTT) until
 * jitter_buffer_ms has elapsed after network keyup (see ptt_ready_ts in
 * watchdog.c), which serves the same "don't start the radio until there's
 * a little audio cushion" purpose at the output-gating layer. Adding a
 * second internal delay here would stack on top of that one for no benefit.
 *
 * capacity_ms still sizes something real: the overflow ceiling. With no
 * sequence gating, the only discard path left is a genuine burst -- pushes
 * arriving faster than the 20ms real-time drain rate for a sustained
 * stretch. A deeper capacity tolerates more of that before dropping
 * anything, at the cost of letting latency grow further first; same
 * latency/reliability trade the config comment has always described, just
 * via a different mechanism than before. */

#define JITTER_CAPACITY_MIN_MS      40u
#define JITTER_CAPACITY_MAX_MS      250u
#define JITTER_CAPACITY_DEFAULT_MS  100u

typedef struct jitter_buffer jitter_buffer_t;

int  jitter_buffer_create(jitter_buffer_t **jb, unsigned int capacity_ms);

/* Push a 160-sample voice frame in arrival order. No sequence number is
 * consulted -- this is a FIFO, not a reorder window. If the buffer is at
 * capacity (a sustained burst outpaced the 20ms drain rate), the oldest
 * buffered frame is dropped to make room, favoring fresh audio over stale
 * backlog; jitter_buffer_overflow_count() reflects this. */
void jitter_buffer_push(jitter_buffer_t *jb, const int16_t *samples);

/* Pull the next 160-sample frame for playout.
 * Returns true if a real frame was available; false if silence was injected
 * because the buffer was genuinely empty. */
bool jitter_buffer_pull(jitter_buffer_t *jb, int16_t *samples_out);

/* Drain all pending frames (call on UNKEY, matching existing call sites). */
void jitter_buffer_flush(jitter_buffer_t *jb);

/* Estimated current jitter in milliseconds, based on inter-arrival timing
 * deviation from the nominal 20ms cadence. Diagnostic only. */
float jitter_buffer_estimate_ms(const jitter_buffer_t *jb);

/* Number of frames dropped since last call because the buffer was at
 * capacity when a push arrived (resets counter). Expected to be rare --
 * unlike the old "late" counter, this is not the discard path for ordinary
 * arrival timing or sequence-number irregularities, only a genuine
 * sustained burst. */
uint64_t jitter_buffer_overflow_count(jitter_buffer_t *jb);

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
