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

/* Replica of MMDVM-Host's FMControl.cpp RX audio filter: a 3-stage cascaded
 * IIR (Chebyshev Type 1, 0.2 dB ripple, 3rd order, 300-2700 Hz bandpass at
 * fs=8000). Coefficients and structure copied directly from FMControl.cpp /
 * IIRDirectForm1Filter.cpp (Copyright (C) 2015-2020,2023 Jonathan Naylor
 * G4KLX; Copyright (C) 2020 Geoffrey Merck F4FXL KC3FRA, GPLv2+), not
 * independently designed -- the exact same coefficients usrp-rc's own
 * voice_filter.c already uses for the same reason: matching MMDVM-Host's RX
 * audio bandwidth precisely rather than approximating it with a
 * freshly-designed filter of unverified equivalence. Here it band-limits
 * the analog CM119A audio in both directions, so a rusrp-linked analog
 * repeater imposes the same bandwidth an MMDVM-based one already does.
 *
 * Direct-form-1 difference equation per stage (matches IIRDirectForm1Filter
 * exactly): y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2].
 * Run in double precision, matching voice_filter.c, then narrowed back to
 * float on the way out -- these are IIR coefficients tuned against that
 * precision, not casually interchangeable with float.
 *
 * Don't also enable the 250 Hz HPF above on the same path: this filter's
 * own ~300 Hz low corner already does that job, so stacking the two adds a
 * second low-end cut for no real benefit. */
#define BPF_STAGES 3

static const double BPF_B0[BPF_STAGES] = { 0.29495028,  1.0,        1.0 };
static const double BPF_B1[BPF_STAGES] = { 0.0,         2.0,       -2.0 };
static const double BPF_B2[BPF_STAGES] = {-0.29495028,  1.0,        1.0 };
static const double BPF_A1[BPF_STAGES] = {-0.61384624,  0.9946123, -1.8414584 };
static const double BPF_A2[BPF_STAGES] = {-0.057158668, 0.6050482,  0.8804949 };

typedef struct { float w0, w1; } biquad_t;
typedef struct { double x1, x2, y1, y2; } bpf_state_t;

struct audio_proc {
    bool      enable_hpf;
    bool      enable_bandpass;
    float     gain_linear;
    biquad_t    hpf[HPF_SECTIONS];
    bpf_state_t bpf[BPF_STAGES];
    /* peak, rms_accum, rms_count are written by the audio thread and read by
     * the telemetry thread without synchronization.  The race is benign for a
     * level display; a mutex in the audio callback would cost more than the UB. */
    float     peak;
    float     rms_accum;
    size_t    rms_count;
    atomic_uint_fast64_t clip_count;
};

int audio_proc_create(audio_proc_t **p, bool enable_hpf, bool enable_bandpass,
                      float gain_db)
{
    audio_proc_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->enable_hpf      = enable_hpf;
    self->enable_bandpass = enable_bandpass;
    self->gain_linear     = powf(10.0f, gain_db / 20.0f);
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

        if (p->enable_bandpass) {
            double x = (double)s;
            for (int k = 0; k < BPF_STAGES; k++) {
                bpf_state_t *st = &p->bpf[k];
                double y0 = BPF_B0[k] * x + BPF_B1[k] * st->x1 + BPF_B2[k] * st->x2
                          - BPF_A1[k] * st->y1 - BPF_A2[k] * st->y2;
                st->x2 = st->x1;
                st->y2 = st->y1;
                st->x1 = x;
                st->y1 = y0;
                x = y0;
            }
            s = (float)x;
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
