#include "jitter_buffer.h"
#include "usrp_protocol.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_SLOTS 16u   /* covers up to 320ms at 20ms/frame */

typedef struct {
    uint32_t seq;
    bool     valid;
    int16_t  samples[USRP_AUDIO_FRAMES];
} jb_slot_t;

struct jitter_buffer {
    pthread_mutex_t  lock;
    jb_slot_t        slots[MAX_SLOTS];
    unsigned int     depth_frames;  /* jitter_buffer_ms / 20 */
    uint32_t         playout_seq;   /* next seq to deliver */
    bool             seeded;        /* true after first push */
    atomic_uint_least64_t late_count;
    atomic_uint_least64_t silence_count;
    atomic_uint_least64_t latched_silence_count;
    atomic_uint_least64_t hb_silence_count;
    /* jitter estimation */
    uint64_t         last_push_ts;
    float            jitter_ms;
};

int jitter_buffer_create(jitter_buffer_t **jb, unsigned int depth_ms)
{
    if (depth_ms < JITTER_BUF_MIN_MS) depth_ms = JITTER_BUF_MIN_MS;
    if (depth_ms > JITTER_BUF_MAX_MS) depth_ms = JITTER_BUF_MAX_MS;

    jitter_buffer_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;

    pthread_mutex_init(&self->lock, NULL);
    self->depth_frames = (depth_ms + 19u) / 20u;
    if (self->depth_frames > MAX_SLOTS) self->depth_frames = MAX_SLOTS;
    atomic_init(&self->late_count, 0);
    atomic_init(&self->silence_count, 0);
    atomic_init(&self->latched_silence_count, 0);
    atomic_init(&self->hb_silence_count, 0);
    *jb = self;
    return 0;
}

void jitter_buffer_push(jitter_buffer_t *jb, uint32_t seq,
                        const int16_t *samples)
{
    uint64_t now = monotonic_ms();

    pthread_mutex_lock(&jb->lock);

    if (!jb->seeded) {
        /* Seed playout cursor so first frame plays after depth_frames * 20ms. */
        jb->playout_seq = seq;
        jb->seeded      = true;
        jb->last_push_ts = now;
    }

    /* Jitter estimate: deviation from expected 20ms inter-packet arrival. */
    if (jb->last_push_ts > 0) {
        float gap = (float)(now - jb->last_push_ts);
        float dev = gap - 20.0f;
        if (dev < 0.0f) dev = -dev;
        jb->jitter_ms = jb->jitter_ms * 0.9f + dev * 0.1f;
    }
    jb->last_push_ts = now;

    /* Discard packets that have already passed the playout cursor. */
    if ((int32_t)(seq - jb->playout_seq) < 0) {
        pthread_mutex_unlock(&jb->lock);
        atomic_fetch_add(&jb->late_count, 1);
        return;
    }

    /* Discard packets too far ahead (outside buffer window). */
    if ((int32_t)(seq - jb->playout_seq) >= (int32_t)jb->depth_frames) {
        pthread_mutex_unlock(&jb->lock);
        atomic_fetch_add(&jb->late_count, 1);
        return;
    }

    unsigned int idx = seq % MAX_SLOTS;
    jb->slots[idx].seq   = seq;
    jb->slots[idx].valid = true;
    memcpy(jb->slots[idx].samples, samples, USRP_AUDIO_BYTES);

    pthread_mutex_unlock(&jb->lock);
}

bool jitter_buffer_pull(jitter_buffer_t *jb, int16_t *samples_out)
{
    pthread_mutex_lock(&jb->lock);

    unsigned int idx = jb->playout_seq % MAX_SLOTS;
    bool real = false;

    if (jb->slots[idx].valid && jb->slots[idx].seq == jb->playout_seq) {
        memcpy(samples_out, jb->slots[idx].samples, USRP_AUDIO_BYTES);
        jb->slots[idx].valid = false;
        real = true;
    } else {
        memset(samples_out, 0, USRP_AUDIO_BYTES);
        atomic_fetch_add(&jb->silence_count, 1);
    }

    jb->playout_seq++;
    pthread_mutex_unlock(&jb->lock);
    return real;
}

void jitter_buffer_flush(jitter_buffer_t *jb)
{
    pthread_mutex_lock(&jb->lock);
    memset(jb->slots, 0, sizeof(jb->slots));
    jb->seeded = false;
    pthread_mutex_unlock(&jb->lock);
}

float jitter_buffer_estimate_ms(const jitter_buffer_t *jb)
{
    /* jitter_ms is updated under lock by push; read here without lock.
     * Benign race — a stale float read is acceptable for a diagnostic metric. */
    return jb->jitter_ms;
}

uint64_t jitter_buffer_late_count(jitter_buffer_t *jb)
{
    return atomic_exchange(&jb->late_count, 0);
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
