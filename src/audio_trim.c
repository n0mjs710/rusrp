#include "audio_trim.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    TRIM_IDLE,
    TRIM_ACTIVE,
} trim_state_t;

struct audio_trim {
    unsigned int leading_frames;
    unsigned int trailing_frames;

    /* Circular FIFO for trailing delay.
     * rd = oldest stored frame; wr = next write slot.
     * count distinguishes empty (0) from full (== trailing_frames). */
    int16_t      buf[AUDIO_TRIM_MAX_FRAMES][AUDIO_TRIM_FRAME_SAMPLES];
    unsigned int rd;
    unsigned int wr;
    unsigned int count;

    unsigned int  leading_remaining; /* counts down during TRIM_ACTIVE */
    trim_state_t  state;
};

int audio_trim_create(audio_trim_t **t,
                      unsigned int leading_frames,
                      unsigned int trailing_frames)
{
    audio_trim_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->leading_frames  = leading_frames;
    self->trailing_frames = trailing_frames;
    self->state           = TRIM_IDLE;
    *t = self;
    return 0;
}

void audio_trim_destroy(audio_trim_t *t)
{
    free(t);
}

void audio_trim_tx_start(audio_trim_t *t)
{
    t->state             = TRIM_ACTIVE;
    t->rd                = 0;
    t->wr                = 0;
    t->count             = 0;
    t->leading_remaining = t->leading_frames;
}

void audio_trim_tx_end(audio_trim_t *t)
{
    /* Discard the FIFO immediately — the last trailing_frames of audio are
     * dropped rather than forwarded.  The caller should stop sending downstream
     * on the same frame this is called. */
    t->state = TRIM_IDLE;
}

bool audio_trim_process(audio_trim_t *t, const int16_t *in, int16_t *out)
{
    static const int16_t silence[AUDIO_TRIM_FRAME_SAMPLES] = {0};

    if (t->state == TRIM_IDLE)
        return false;

    /* Fast path: no trailing delay — just apply the leading gate. */
    if (t->trailing_frames == 0) {
        if (t->leading_remaining > 0) {
            memcpy(out, silence, AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
            t->leading_remaining--;
        } else {
            memcpy(out, in, AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
        }
        return true;
    }

    /* Trailing FIFO path.
     *
     * Still filling: push frame, hold off output until the FIFO is full.
     * This creates the N-frame delay: audio reaching the output at time T
     * was captured at time T - trailing_frames*20ms. */
    if (t->count < t->trailing_frames) {
        memcpy(t->buf[t->wr], in, AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
        t->wr = (t->wr + 1) % t->trailing_frames;
        t->count++;
        return false;
    }

    /* FIFO full: read the oldest frame FIRST (rd == wr when full, so the
     * read must come before the write or we clobber the oldest frame). */
    if (t->leading_remaining > 0) {
        memcpy(out, silence, AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
        t->leading_remaining--;
    } else {
        memcpy(out, t->buf[t->rd], AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
    }
    /* Now overwrite the freed slot with the incoming frame. */
    memcpy(t->buf[t->wr], in, AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
    t->rd = (t->rd + 1) % t->trailing_frames;
    t->wr = (t->wr + 1) % t->trailing_frames;
    return true;
}

bool audio_trim_active(const audio_trim_t *t)
{
    return t->state != TRIM_IDLE;
}
