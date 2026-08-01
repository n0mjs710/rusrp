#include "opus_codec.h"

#include <opus/opus.h>
#include <stdlib.h>
#include <systemd/sd-journal.h>

struct opus_codec {
    OpusEncoder *enc;
    OpusDecoder *dec;
    int          frame_samples;   /* 160, 320, or 480 for 20/40/60 ms at 8 kHz */
};

int opus_codec_create(opus_codec_t **out, int bitrate_bps, int frame_ms)
{
    if (frame_ms != 20 && frame_ms != 40 && frame_ms != 60) {
        sd_journal_print(LOG_ERR, "opus: frame_ms must be 20, 40, or 60; got %d", frame_ms);
        return -1;
    }

    opus_codec_t *c = calloc(1, sizeof(*c));
    if (!c) return -1;

    c->frame_samples = frame_ms * 8;  /* 8 kHz sample rate */

    int err;
    c->enc = opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        sd_journal_print(LOG_ERR, "opus: encoder create: %s", opus_strerror(err));
        free(c);
        return -1;
    }

    opus_encoder_ctl(c->enc, OPUS_SET_BITRATE(bitrate_bps));
    opus_encoder_ctl(c->enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(c->enc, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(c->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    /* Force narrowband — matches the analog channel's 300–3000 Hz passband. */
    opus_encoder_ctl(c->enc, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
    opus_encoder_ctl(c->enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));

    c->dec = opus_decoder_create(8000, 1, &err);
    if (err != OPUS_OK) {
        sd_journal_print(LOG_ERR, "opus: decoder create: %s", opus_strerror(err));
        opus_encoder_destroy(c->enc);
        free(c);
        return -1;
    }

    sd_journal_print(LOG_INFO, "opus: %d ms frames, %d bps VBR narrowband SILK",
                     frame_ms, bitrate_bps);
    *out = c;
    return 0;
}

void opus_codec_destroy(opus_codec_t *c)
{
    if (!c) return;
    opus_encoder_destroy(c->enc);
    opus_decoder_destroy(c->dec);
    free(c);
}

int opus_codec_encode(opus_codec_t *c, const int16_t *in_pcm, uint8_t *out_opus)
{
    int n = opus_encode(c->enc, in_pcm, c->frame_samples,
                        out_opus, (opus_int32)OPUS_MAX_FRAME_BYTES);
    if (n < 0)
        sd_journal_print(LOG_ERR, "opus: encode: %s", opus_strerror(n));
    return n;
}

int opus_codec_decode(opus_codec_t *c, const uint8_t *in_opus, int in_len,
                      int16_t *out_pcm)
{
    int n = opus_decode(c->dec, in_opus, in_len, out_pcm, c->frame_samples, 0);
    if (n < 0)
        sd_journal_print(LOG_ERR, "opus: decode: %s", opus_strerror(n));
    return n;
}

int opus_codec_frame_samples(const opus_codec_t *c)
{
    return c->frame_samples;
}
