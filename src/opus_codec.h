#pragma once

#include <stdint.h>

/* Generous upper bound for any supported bitrate/frame combination. */
#define OPUS_MAX_FRAME_BYTES 256u

typedef struct opus_codec opus_codec_t;

/*
 * Create an Opus codec for narrowband speech (8 kHz mono SILK).
 *
 * bitrate_bps  — VBR target (e.g. 8000). Soft target; actual sizes vary.
 * frame_ms     — frame duration: 20, 40, or 60 ms.
 */
int  opus_codec_create(opus_codec_t **out, int bitrate_bps, int frame_ms);
void opus_codec_destroy(opus_codec_t *c);

/*
 * Encode one complete frame of 16-bit mono PCM at 8 kHz.
 * in_pcm must contain exactly frame_samples() samples.
 * out_opus must be at least OPUS_MAX_FRAME_BYTES.
 * Returns bytes written, or negative on error.
 */
int opus_codec_encode(opus_codec_t *c, const int16_t *in_pcm, uint8_t *out_opus);

/*
 * Decode one Opus frame.
 * out_pcm must hold at least frame_samples() samples.
 * Returns samples decoded, or negative on error.
 */
int opus_codec_decode(opus_codec_t *c, const uint8_t *in_opus, int in_len,
                      int16_t *out_pcm);

/* PCM samples per frame: frame_ms * 8 at 8 kHz. */
int opus_codec_frame_samples(const opus_codec_t *c);
