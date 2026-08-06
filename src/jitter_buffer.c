#include "jitter_buffer.h"
#include "usrp_protocol.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

/* Hard array bound, independent of the configured (usually smaller)
 * overflow ceiling -- covers up to 320ms at 20ms/frame, comfortably above
 * JITTER_CAPACITY_MAX_MS (250ms). */
#define MAX_SLOTS 16u

typedef struct {
    int16_t samples[USRP_AUDIO_FRAMES];
} jb_slot_t;

struct jitter_buffer {
    pthread_mutex_t  lock;
    jb_slot_t        slots[MAX_SLOTS];
    unsigned int     read_idx;        /* next slot to drain */
    unsigned int     count;           /* frames currently buffered */
    unsigned int     capacity_frames; /* overflow ceiling */
    atomic_uint_least64_t silence_count;
    atomic_uint_least64_t latched_silence_count;
    atomic_uint_least64_t hb_silence_count;
    atomic_uint_least64_t overflow_count;
    /* jitter estimation (diagnostic only, nothing is gated on it) */
    uint64_t         last_push_ts;
    float             jitter_ms;
};

int jitter_buffer_create(jitter_buffer_t **jb, unsigned int capacity_ms)
{
    if (capacity_ms < JITTER_CAPACITY_MIN_MS) capacity_ms = JITTER_CAPACITY_MIN_MS;
    if (capacity_ms > JITTER_CAPACITY_MAX_MS) capacity_ms = JITTER_CAPACITY_MAX_MS;

    jitter_buffer_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;

    pthread_mutex_init(&self->lock, NULL);
    self->capacity_frames = (capacity_ms + 19u) / 20u;
    if (self->capacity_frames > MAX_SLOTS) self->capacity_frames = MAX_SLOTS;
    atomic_init(&self->silence_count, 0);
    atomic_init(&self->latched_silence_count, 0);
    atomic_init(&self->hb_silence_count, 0);
    atomic_init(&self->overflow_count, 0);
    *jb = self;
    return 0;
}

void jitter_buffer_push(jitter_buffer_t *jb, const int16_t *samples)
{
    uint64_t now = monotonic_ms();

    pthread_mutex_lock(&jb->lock);

    if (jb->last_push_ts > 0) {
        float gap = (float)(now - jb->last_push_ts);
        float dev = gap - 20.0f;
        if (dev < 0.0f) dev = -dev;
        jb->jitter_ms = jb->jitter_ms * 0.9f + dev * 0.1f;
    }
    jb->last_push_ts = now;

    if (jb->count >= jb->capacity_frames) {
        /* At capacity: drop the oldest frame to make room for this one --
         * prefer fresh audio over stale backlog. */
        jb->read_idx = (jb->read_idx + 1u) % MAX_SLOTS;
        jb->count--;
        atomic_fetch_add(&jb->overflow_count, 1);
    }

    unsigned int write_idx = (jb->read_idx + jb->count) % MAX_SLOTS;
    memcpy(jb->slots[write_idx].samples, samples, USRP_AUDIO_BYTES);
    jb->count++;

    pthread_mutex_unlock(&jb->lock);
}

bool jitter_buffer_pull(jitter_buffer_t *jb, int16_t *samples_out)
{
    pthread_mutex_lock(&jb->lock);

    bool real;
    if (jb->count > 0) {
        memcpy(samples_out, jb->slots[jb->read_idx].samples, USRP_AUDIO_BYTES);
        jb->read_idx = (jb->read_idx + 1u) % MAX_SLOTS;
        jb->count--;
        real = true;
    } else {
        memset(samples_out, 0, USRP_AUDIO_BYTES);
        atomic_fetch_add(&jb->silence_count, 1);
        real = false;
    }

    pthread_mutex_unlock(&jb->lock);
    return real;
}

void jitter_buffer_flush(jitter_buffer_t *jb)
{
    pthread_mutex_lock(&jb->lock);
    jb->read_idx = 0;
    jb->count    = 0;
    pthread_mutex_unlock(&jb->lock);
}

float jitter_buffer_estimate_ms(const jitter_buffer_t *jb)
{
    /* jitter_ms is updated under lock by push; read here without lock.
     * Benign race — a stale float read is acceptable for a diagnostic metric. */
    return jb->jitter_ms;
}

uint64_t jitter_buffer_overflow_count(jitter_buffer_t *jb)
{
    return atomic_exchange(&jb->overflow_count, 0);
}

void jitter_buffer_reset_silence_count(jitter_buffer_t *jb)
{
    atomic_store(&jb->silence_count, 0);
}

void jitter_buffer_latch_silence(jitter_buffer_t *jb)
{
    uint64_t v = atomic_exchange(&jb->silence_count, 0);
    atomic_store(&jb->latched_silence_count, v);
    atomic_fetch_add(&jb->hb_silence_count, v);
}

uint64_t jitter_buffer_latched_silence_count(jitter_buffer_t *jb)
{
    return atomic_exchange(&jb->latched_silence_count, 0);
}

uint64_t jitter_buffer_hb_silence_count(jitter_buffer_t *jb)
{
    return atomic_exchange(&jb->hb_silence_count, 0);
}

void jitter_buffer_destroy(jitter_buffer_t *jb)
{
    if (!jb) return;
    pthread_mutex_destroy(&jb->lock);
    free(jb);
}
