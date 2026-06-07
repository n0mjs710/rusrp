#include "audio_processing.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>

struct audio_proc {
    bool  enable_hpf;
    float gain_linear;
    float hpf_prev_in;
    float hpf_prev_out;
    float hpf_alpha;
    float peak;
    float rms_accum;
    size_t rms_count;
};

/* First-order Butterworth HPF at 300 Hz, fs = 8000 Hz.
 * RC = 1/(2π*300) ≈ 530.5 µs; dt = 1/8000 = 125 µs
 * α = RC/(RC+dt) = 530.5/655.5 ≈ 0.8093 */
#define HPF_ALPHA 0.8093f

int audio_proc_create(audio_proc_t **p, bool enable_hpf, float gain_db)
{
    audio_proc_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->enable_hpf  = enable_hpf;
    self->gain_linear = powf(10.0f, gain_db / 20.0f);
    self->hpf_alpha   = HPF_ALPHA;
    *p = self;
    return 0;
}

void audio_proc_run(audio_proc_t *p, int16_t *samples, size_t count,
                    uint64_t *clip_count_out)
{
    uint64_t clips = 0;

    for (size_t i = 0; i < count; i++) {
        float s = (float)samples[i];

        if (p->enable_hpf) {
            float out = p->hpf_alpha * (p->hpf_prev_out + s - p->hpf_prev_in);
            p->hpf_prev_in  = s;
            p->hpf_prev_out = out;
            s = out;
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

    *clip_count_out += clips;
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

void audio_proc_destroy(audio_proc_t *p)
{
    free(p);
}
