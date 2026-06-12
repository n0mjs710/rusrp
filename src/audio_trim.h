#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * audio_trim — per-path leading and trailing edge trim for audio transmissions.
 *
 * Leading trim: after the transmission rising edge, the first leading_frames
 * of audio sent downstream are replaced with silence.  The KEY/output_active
 * signal is NOT delayed — it asserts immediately so the channel is captured
 * and any CTCSS decoder on the far end has time to open before voice arrives.
 *
 * Trailing trim: audio is delayed by trailing_frames through a FIFO.  When
 * the transmission falling edge is detected the FIFO is drained (its content
 * is the clean audio from just before the edge) and then the path goes idle.
 * The last trailing_frames worth of newly-captured audio — which may contain
 * squelch-tail noise or other artifacts — are never sent downstream.
 *
 * Both values are expressed in 20 ms frames (one USRP frame).  Pass 0 for
 * either to disable that function; the module then adds no latency.
 */

/* Maximum trim depth: 260 ms / 20 ms = 13 frames. */
#define AUDIO_TRIM_MAX_FRAMES  13u
#define AUDIO_TRIM_FRAME_SAMPLES 160u   /* must match USRP_AUDIO_FRAMES */

typedef struct audio_trim audio_trim_t;

int  audio_trim_create(audio_trim_t **t,
                       unsigned int leading_frames,
                       unsigned int trailing_frames);
void audio_trim_destroy(audio_trim_t *t);

/* Call on transmission rising edge.  Resets state; safe to call repeatedly. */
void audio_trim_tx_start(audio_trim_t *t);

/*
 * Call on transmission falling edge.  If trailing_frames > 0 the module
 * enters drain mode: subsequent calls to audio_trim_process will emit the
 * buffered pre-edge frames before returning false.  If trailing_frames == 0
 * the module goes idle immediately.
 */
void audio_trim_tx_end(audio_trim_t *t);

/*
 * Process one 20 ms frame.
 *
 *   in  — real captured audio (ignored while draining; may be NULL then)
 *   out — frame to send downstream
 *
 * Returns true if out contains a frame that should be forwarded.
 * Returns false when the module is idle or the trailing FIFO is still filling.
 *
 * Callers should send UNKEY / deassert output_active on the first call that
 * returns false after audio_trim_tx_end() was called (i.e. the falling edge
 * of audio_trim_active()).
 */
bool audio_trim_process(audio_trim_t *t, const int16_t *in, int16_t *out);

/* True while the module is transmitting or draining (not yet fully idle). */
bool audio_trim_active(const audio_trim_t *t);
