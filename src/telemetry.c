#include "telemetry.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-journal.h>

struct telemetry {
    const config_t *cfg;
    uint64_t        last_log_ts;
    bool            prev_input_active;
    bool            prev_output_active;
};

int telemetry_create(telemetry_t **tel, const config_t *cfg)
{
    telemetry_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;
    self->cfg         = cfg;
    self->last_log_ts = monotonic_ms();
    *tel = self;
    return 0;
}

static void log_status(int priority, const char *label,
                       float in_peak,  float in_rms,
                       float out_peak, float out_rms,
                       bool show_input, bool show_output,
                       bool  input_active, bool output_active,
                       float jitter,
                       uint64_t late_packets,
                       uint64_t watchdog_events,
                       uint64_t overruns, uint64_t underruns)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s:", label);
    if (show_input)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " in=%.1fpk/%.1frms dBFS", in_peak, in_rms);
    if (show_output)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " out=%.1fpk/%.1frms dBFS", out_peak, out_rms);
    snprintf(buf + n, sizeof(buf) - n,
             " input_active=%d output_active=%d jitter=%.1fms"
             " late=%llu wd_events=%llu overruns=%llu underruns=%llu",
             (int)input_active, (int)output_active, jitter,
             (unsigned long long)late_packets,
             (unsigned long long)watchdog_events,
             (unsigned long long)overruns, (unsigned long long)underruns);
    sd_journal_print(priority, "%s", buf);
}

void telemetry_log(telemetry_t *tel,
                   audio_alsa_t          *alsa,
                   audio_proc_t          *in_proc,
                   audio_proc_t          *out_proc,
                   const logic_hid_t     *logic,
                   jitter_buffer_t       *jb,
                   const watchdog_t      *wd)
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
    uint64_t late      = jb   ? jitter_buffer_late_count(jb)   : 0;
    uint64_t wd_events = wd   ? watchdog_event_count(wd)       : 0;

    /* INFO: one summary line on the falling edge of each transmission. */
    bool log_now = false;

    if (tel->prev_input_active && !input_active) {
        log_status(LOG_INFO, "input-end", in_peak, in_rms, out_peak, out_rms,
                   true, false,
                   input_active, output_active, jitter,
                   late, wd_events, overruns, underruns);
        log_now = true;
    }
    if (tel->prev_output_active && !output_active) {
        log_status(LOG_INFO, "output-end", in_peak, in_rms, out_peak, out_rms,
                   false, true,
                   input_active, output_active, jitter,
                   late, wd_events, overruns, underruns);
        log_now = true;
    }

    /* Reset level accumulators after an end-of-transmission log so the
     * next transmission's stats reflect only that transmission. */
    if (log_now) {
        if (in_proc)  audio_proc_reset_levels(in_proc);
        if (out_proc) audio_proc_reset_levels(out_proc);
    }

    tel->prev_input_active  = input_active;
    tel->prev_output_active = output_active;

    /* DEBUG: periodic heartbeat — only when level=debug is configured. */
    if (tel->cfg->logging.level <= LOG_LEVEL_DEBUG &&
        now - tel->last_log_ts >=
            (uint64_t)tel->cfg->logging.status_interval_sec * 1000u) {
        tel->last_log_ts = now;
        log_status(LOG_DEBUG, "heartbeat", in_peak, in_rms, out_peak, out_rms,
                   true, true,
                   input_active, output_active, jitter,
                   late, wd_events, overruns, underruns);
    } else if (now - tel->last_log_ts >=
                   (uint64_t)tel->cfg->logging.status_interval_sec * 1000u) {
        tel->last_log_ts = now;
    }
}

void telemetry_destroy(telemetry_t *tel)
{
    free(tel);
}
