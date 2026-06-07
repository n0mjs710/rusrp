#include "telemetry.h"
#include "util.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <systemd/sd-journal.h>

struct telemetry {
    const config_t *cfg;
    uint64_t        last_log_ts;
    atomic_uint_least64_t packets_sent;
    atomic_uint_least64_t packets_received;
};

int telemetry_create(telemetry_t **tel, const config_t *cfg)
{
    telemetry_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->cfg         = cfg;
    self->last_log_ts = monotonic_ms();
    atomic_init(&self->packets_sent,     0);
    atomic_init(&self->packets_received, 0);
    *tel = self;
    return 0;
}

void telemetry_log(telemetry_t *tel,
                   const audio_alsa_t    *alsa,
                   const audio_proc_t    *in_proc,
                   const audio_proc_t    *out_proc,
                   const logic_hid_t     *logic,
                   const jitter_buffer_t *jb)
{
    uint64_t now = monotonic_ms();
    if (now - tel->last_log_ts <
        (uint64_t)tel->cfg->logging.status_interval_sec * 1000u)
        return;
    tel->last_log_ts = now;

    uint64_t overruns = 0, underruns = 0;
    if (alsa)
        audio_alsa_get_stats(alsa, &overruns, &underruns);

    float in_peak = -96.0f, in_rms = -96.0f;
    float out_peak = -96.0f, out_rms = -96.0f;
    if (in_proc)  audio_proc_get_levels(in_proc,  &in_peak,  &in_rms);
    if (out_proc) audio_proc_get_levels(out_proc, &out_peak, &out_rms);

    bool input_state  = logic ? logic_hid_input_active(logic) : false;
    float jitter      = jb    ? jitter_buffer_estimate_ms(jb) : 0.0f;

    sd_journal_print(LOG_INFO,
        "status: in=%.1f/%.1f dBFS out=%.1f/%.1f dBFS "
        "input_active=%d output_active=%d jitter=%.1fms "
        "overruns=%llu underruns=%llu",
        in_peak,  in_rms,
        out_peak, out_rms,
        (int)input_state,
        (int)(logic ? !logic_hid_input_active(logic) : 0), /* placeholder */
        jitter,
        (unsigned long long)overruns,
        (unsigned long long)underruns);
}

void telemetry_destroy(telemetry_t *tel)
{
    free(tel);
}
