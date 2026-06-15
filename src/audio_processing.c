#include "audio_processing.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>

/* 4th-order Butterworth HPF at 250 Hz, fs = 8000 Hz.
 * Two cascaded Direct Form II Transposed biquad sections.
 * 250 Hz cutoff keeps the transition band away from the voice passband
 * while still attenuating CTCSS (67–203 Hz) by 8–42 dB and DCS (134 Hz)
 * by ~28 dB. This is a safety filter; sub-audible signalling should
 * already have been stripped by the radio or controller.
 *
 * Coefficients derived via RBJ audio EQ cookbook (bilinear transform):
 *   Q1=0.5412 (lower-Q section first — better numerical conditioning)
 *   Q2=1.3066
 * Rejection: -42 dB @ 67 Hz, -32 dB @ 100 Hz, -8 dB @ 200 Hz. */
#define HPF_SECTIONS 2

static const float HPF_B0[HPF_SECTIONS] = { 0.83914f,  0.92156f };
static const float HPF_B1[HPF_SECTIONS] = {-1.67828f, -1.84312f };
/* B2 == B0 for every HPF biquad (symmetric numerator). */
static const float HPF_A1[HPF_SECTIONS] = {-1.66202f, -1.82527f };
static const float HPF_A2[HPF_SECTIONS] = { 0.69455f,  0.86103f };

typedef struct { float w0, w1; } biquad_t;

struct audio_proc {
    bool      enable_hpf;
    float     gain_linear;
    biquad_t  hpf[HPF_SECTIONS];
    /* peak, rms_accum, rms_count are written by the audio thread and read by
     * the telemetry thread without synchronization.  The race is benign for a
     * level display; a mutex in the audio callback would cost more than the UB. */
    float     peak;
    float     rms_accum;
    size_t    rms_count;
    atomic_uint_fast64_t clip_count;
};

int audio_proc_create(audio_proc_t **p, bool enable_hpf, float gain_db)
{
    audio_proc_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->enable_hpf  = enable_hpf;
    self->gain_linear = powf(10.0f, gain_db / 20.0f);
    atomic_init(&self->clip_count, 0);
    *p = self;
    return 0;
}

void audio_proc_run(audio_proc_t *p, int16_t *samples, size_t count)
{
    uint64_t clips = 0;

    for (size_t i = 0; i < count; i++) {
        float s = (float)samples[i];

        if (p->enable_hpf) {
            for (int k = 0; k < HPF_SECTIONS; k++) {
                biquad_t *st = &p->hpf[k];
                float y  = HPF_B0[k] * s + st->w0;
                st->w0   = HPF_B1[k] * s - HPF_A1[k] * y + st->w1;
                st->w1   = HPF_B0[k] * s - HPF_A2[k] * y; /* B2 == B0 */
                s = y;
            }
        }

        s *= p->gain_linear;

        if      (s >  32767.0f) { s =  32767.0f; clips++; }
        else if (s < -32768.0f) { s = -32768.0f; clips++; }

        samples[i] = (int16_t)s;

        float abs_s = fabsf(s);
        if (abs_s > p->peak) p->peak = abs_s;
        p->rms_accum += s * s;
        p->rms_count++;
    }

    if (clips)
        atomic_fetch_add_explicit(&p->clip_count, clips, memory_order_relaxed);
}

void audio_proc_get_levels(const audio_proc_t *p,
                           float *peak_dbfs_out, float *rms_dbfs_out)
{
    *peak_dbfs_out = (p->peak > 0.0f)
        ? 20.0f * log10f(p->peak / 32768.0f)
        : -96.0f;

    if (p->rms_count > 0) {
        float rms = sqrtf(p->rms_accum / (float)p->rms_count);
        *rms_dbfs_out = (rms > 0.0f) ? 20.0f * log10f(rms / 32768.0f) : -96.0f;
    } else {
        *rms_dbfs_out = -96.0f;
    }
}

void audio_proc_reset_levels(audio_proc_t *p)
{
    p->peak      = 0.0f;
    p->rms_accum = 0.0f;
    p->rms_count = 0;
}

uint64_t audio_proc_clip_count(audio_proc_t *p)
{
    return atomic_exchange(&p->clip_count, 0);
}

void audio_proc_destroy(audio_proc_t *p)
{
    free(p);
}
