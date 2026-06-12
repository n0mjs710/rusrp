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
 * Trailing trim: audio is time-shifted through a FIFO of depth trailing_frames.
 * This adds trailing_frames * 20 ms of latency to every transmission, but when
 * audio_trim_tx_end() is called the FIFO is discarded immediately — the last
 * trailing_frames of audio never reach downstream.  On the input path this
 * removes squelch-tail noise before it reaches the network; on the output path
 * it stops ALSA output the moment PTT releases.
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

/* Call on transmission falling edge.  The FIFO is discarded immediately and
 * the module goes idle.  The caller should stop sending downstream on the
 * same frame (write silence / stop sending USRP frames). */
void audio_trim_tx_end(audio_trim_t *t);

/*
 * Process one 20 ms frame.
 *
 *   in  — real captured audio (must be valid while TRIM_ACTIVE)
 *   out — frame to send downstream
 *
 * Returns true if out contains a frame that should be forwarded.
 * Returns false when the module is idle or the trailing FIFO is still filling.
 */
bool audio_trim_process(audio_trim_t *t, const int16_t *in, int16_t *out);

/* True while the module is transmitting or draining (not yet fully idle). */
bool audio_trim_active(const audio_trim_t *t);
