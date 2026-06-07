#include "audio_alsa.h"
#include <stdlib.h>
#include <stdatomic.h>

struct audio_alsa {
    audio_capture_cb_t cap_cb;
    void              *userdata;
    atomic_uint_least64_t overruns;
    atomic_uint_least64_t underruns;
};

int audio_alsa_create(audio_alsa_t **a, const config_t *cfg,
                      audio_capture_cb_t cap_cb, void *userdata)
{
    (void)cfg;
    audio_alsa_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->cap_cb   = cap_cb;
    self->userdata = userdata;
    atomic_init(&self->overruns,  0);
    atomic_init(&self->underruns, 0);
    *a = self;
    return 0;
}

int audio_alsa_write(audio_alsa_t *a, const int16_t *samples, size_t count)
{
    (void)a; (void)samples; (void)count;
    return 0;
}

void audio_alsa_get_stats(const audio_alsa_t *a,
                          uint64_t *overruns_out, uint64_t *underruns_out)
{
    *overruns_out  = atomic_load(&a->overruns);
    *underruns_out = atomic_load(&a->underruns);
}

void audio_alsa_destroy(audio_alsa_t *a)
{
    free(a);
}
