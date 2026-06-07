#include "telemetry.h"
#include "util.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <systemd/sd-journal.h>

struct telemetry {
    const config_t *cfg;
    uint64_t        last_log_ts;
    bool            prev_input_active;
    bool            prev_output_active;
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

static void log_status(int priority,
                       float in_peak,  float in_rms,
                       float out_peak, float out_rms,
                       bool  input_active, bool output_active,
                       float jitter,
                       uint64_t overruns, uint64_t underruns)
{
    sd_journal_print(priority,
        "status: in=%.1f/%.1f dBFS out=%.1f/%.1f dBFS "
        "input_active=%d output_active=%d jitter=%.1fms "
        "overruns=%llu underruns=%llu",
        in_peak,  in_rms,
        out_peak, out_rms,
        (int)input_active,
        (int)output_active,
        jitter,
        (unsigned long long)overruns,
        (unsigned long long)underruns);
}

void telemetry_log(telemetry_t *tel,
                   const audio_alsa_t    *alsa,
                   const audio_proc_t    *in_proc,
                   const audio_proc_t    *out_proc,
                   const logic_hid_t     *logic,
                   const jitter_buffer_t *jb)
{
    uint64_t now = monotonic_ms();

    uint64_t overruns = 0, underruns = 0;
    if (alsa) audio_alsa_get_stats(alsa, &overruns, &underruns);

    float in_peak = -96.0f, in_rms = -96.0f;
    float out_peak = -96.0f, out_rms = -96.0f;
    if (in_proc)  audio_proc_get_levels(in_proc,  &in_peak,  &in_rms);
    if (out_proc) audio_proc_get_levels(out_proc, &out_peak, &out_rms);

    bool input_active  = logic ? logic_hid_input_active(logic)  : false;
    bool output_active = logic ? logic_hid_output_active(logic) : false;
    float jitter       = jb   ? jitter_buffer_estimate_ms(jb)  : 0.0f;

    /* INFO: log once on the falling edge of each transmission direction. */
    if (tel->prev_input_active && !input_active)
        log_status(LOG_INFO,  in_peak, in_rms, out_peak, out_rms,
                   input_active, output_active, jitter, overruns, underruns);

    if (tel->prev_output_active && !output_active)
        log_status(LOG_INFO,  in_peak, in_rms, out_peak, out_rms,
                   input_active, output_active, jitter, overruns, underruns);

    tel->prev_input_active  = input_active;
    tel->prev_output_active = output_active;

    /* DEBUG: periodic heartbeat at the configured interval. */
    if (now - tel->last_log_ts >=
        (uint64_t)tel->cfg->logging.status_interval_sec * 1000u) {
        tel->last_log_ts = now;
        log_status(LOG_DEBUG, in_peak, in_rms, out_peak, out_rms,
                   input_active, output_active, jitter, overruns, underruns);
    }
}

void telemetry_destroy(telemetry_t *tel)
{
    free(tel);
}
