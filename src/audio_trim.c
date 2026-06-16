#include "audio_trim.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    TRIM_IDLE,
    TRIM_ACTIVE,
} trim_state_t;

struct audio_trim {
    unsigned int leading_frames;
    unsigned int leading_remaining;
    trim_state_t state;
};

int audio_trim_create(audio_trim_t **t, unsigned int leading_frames)
{
    audio_trim_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->leading_frames = leading_frames;
    self->state          = TRIM_IDLE;
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
    t->leading_remaining = t->leading_frames;
}

void audio_trim_tx_end(audio_trim_t *t)
{
    t->state = TRIM_IDLE;
}

bool audio_trim_process(audio_trim_t *t, const int16_t *in, int16_t *out)
{
    static const int16_t silence[AUDIO_TRIM_FRAME_SAMPLES] = {0};

    if (t->state == TRIM_IDLE)
        return false;

    if (t->leading_remaining > 0) {
        memcpy(out, silence, AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
        t->leading_remaining--;
    } else {
        memcpy(out, in, AUDIO_TRIM_FRAME_SAMPLES * sizeof(int16_t));
    }
    return true;
}

bool audio_trim_active(const audio_trim_t *t)
{
    return t->state != TRIM_IDLE;
}
