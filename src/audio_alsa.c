#include "audio_alsa.h"
#include "util.h"

#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <alsa/asoundlib.h>
#include <systemd/sd-journal.h>

#define SAMPLE_RATE    8000u
#define CHANNELS       1u
#define FRAME_SAMPLES  160u   /* 20 ms at 8 kHz */

struct audio_alsa {
    snd_pcm_t             *capture;
    snd_pcm_t             *playback;
    audio_capture_cb_t     cap_cb;
    void                  *userdata;
    pthread_t              capture_thread;
    bool                   capture_thread_started;
    atomic_bool            stop;
    atomic_uint_least64_t  overruns;
    atomic_uint_least64_t  underruns;
};

static int open_pcm(snd_pcm_t **handle, const char *device,
                    snd_pcm_stream_t stream)
{
    const char *dir = (stream == SND_PCM_STREAM_CAPTURE) ? "capture" : "playback";
    int err = snd_pcm_open(handle, device, stream, 0);
    if (err < 0) {
        sd_journal_print(LOG_ERR, "alsa: open %s (%s): %s",
                         device, dir, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(*handle, params);
    snd_pcm_hw_params_set_access(*handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(*handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(*handle, params, CHANNELS);

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(*handle, params, &rate, 0);
    if (rate != SAMPLE_RATE) {
        sd_journal_print(LOG_ERR,
            "alsa: %s (%s) only supports %u Hz, need %u Hz — "
            "use plughw: instead of hw: in alsa_device config",
            device, dir, rate, SAMPLE_RATE);
        snd_pcm_close(*handle);
        *handle = NULL;
        return -1;
    }

    snd_pcm_uframes_t period = FRAME_SAMPLES;
    snd_pcm_hw_params_set_period_size_near(*handle, params, &period, 0);

    err = snd_pcm_hw_params(*handle, params);
    if (err < 0) {
        sd_journal_print(LOG_ERR, "alsa: hw_params %s (%s): %s",
                         device, dir, snd_strerror(err));
        snd_pcm_close(*handle);
        *handle = NULL;
        return -1;
    }
    return 0;
}

static void *capture_thread_fn(void *arg)
{
    audio_alsa_t *a = arg;
    int16_t buf[FRAME_SAMPLES];

    while (!atomic_load_explicit(&a->stop, memory_order_relaxed)) {
        snd_pcm_sframes_t n = snd_pcm_readi(a->capture, buf, FRAME_SAMPLES);

        if (n == -EPIPE) {
            /* Buffer overrun — drain and continue. */
            atomic_fetch_add_explicit(&a->overruns, 1, memory_order_relaxed);
            snd_pcm_prepare(a->capture);
            continue;
        }
        if (n < 0) {
            int err = snd_pcm_recover(a->capture, (int)n, /*silent=*/1);
            if (err < 0) {
                sd_journal_print(LOG_ERR, "alsa: capture unrecoverable: %s",
                                 snd_strerror(err));
                break;
            }
            continue;
        }
        if ((size_t)n < FRAME_SAMPLES)
            continue; /* partial frame — shouldn't happen with blocking mode */

        a->cap_cb(buf, (size_t)n, a->userdata);
    }
    return NULL;
}

int audio_alsa_create(audio_alsa_t **out, const config_t *cfg,
                      audio_capture_cb_t cap_cb, void *userdata)
{
    audio_alsa_t *a = calloc(1, sizeof(*a));
    if (!a) return -1;

    a->cap_cb   = cap_cb;
    a->userdata = userdata;
    atomic_init(&a->stop,      false);
    atomic_init(&a->overruns,  0);
    atomic_init(&a->underruns, 0);

    if (open_pcm(&a->capture,  cfg->audio.alsa_device, SND_PCM_STREAM_CAPTURE)  < 0 ||
        open_pcm(&a->playback, cfg->audio.alsa_device, SND_PCM_STREAM_PLAYBACK) < 0) {
        audio_alsa_destroy(a);
        return -1;
    }

    if (pthread_create(&a->capture_thread, NULL, capture_thread_fn, a) != 0) {
        sd_journal_print(LOG_ERR, "alsa: pthread_create: %m");
        audio_alsa_destroy(a);
        return -1;
    }
    a->capture_thread_started = true;

    sd_journal_print(LOG_INFO, "alsa: opened %s (8 kHz mono s16le, 20 ms frames)",
                     cfg->audio.alsa_device);
    *out = a;
    return 0;
}

int audio_alsa_write(audio_alsa_t *a, const int16_t *samples, size_t count)
{
    snd_pcm_sframes_t n = snd_pcm_writei(a->playback, samples,
                                          (snd_pcm_uframes_t)count);
    if (n == -EPIPE) {
        /* Buffer underrun — recover silently, caller will retry next frame. */
        atomic_fetch_add_explicit(&a->underruns, 1, memory_order_relaxed);
        snd_pcm_prepare(a->playback);
        return 0;
    }
    if (n < 0) {
        int err = snd_pcm_recover(a->playback, (int)n, /*silent=*/1);
        return (err < 0) ? -1 : 0;
    }
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
    if (!a) return;
    atomic_store(&a->stop, true);
    if (a->capture_thread_started) {
        /* snd_pcm_drop() aborts the blocking snd_pcm_readi() in the capture
         * thread so it sees the stop flag within one error-recovery cycle. */
        if (a->capture)
            snd_pcm_drop(a->capture);
        pthread_join(a->capture_thread, NULL);
    }
    if (a->capture)
        snd_pcm_close(a->capture);
    if (a->playback)
        snd_pcm_close(a->playback);
    free(a);
}
