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
    uint64_t        input_tx_start_ts;
    uint64_t        output_tx_start_ts;
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

/* Flags controlling which fields appear in a log_status line. */
#define STAT_DURATION        (1u << 0)  /* dur=Xms */
#define STAT_INPUT_LEVELS    (1u << 1)  /* in=Xpk/Yrms dBFS */
#define STAT_OUTPUT_LEVELS   (1u << 2)  /* out=Xpk/Yrms dBFS */
#define STAT_ACTIVE_FLAGS    (1u << 3)  /* input_active= output_active= */
#define STAT_INPUT_PATH      (1u << 4)  /* overruns= */
#define STAT_OUTPUT_PATH     (1u << 5)  /* jitter= late= silence= underruns= */
#define STAT_IN_CLIPS        (1u << 6)  /* clips= (input path) */
#define STAT_OUT_CLIPS       (1u << 7)  /* clips= (output path) */

static void log_status(int priority, const char *label,
                       unsigned int flags,
                       uint64_t dur_ms,
                       float in_peak,  float in_rms,
                       float out_peak, float out_rms,
                       bool  input_active, bool output_active,
                       float jitter,
                       uint64_t late, uint64_t silence,
                       uint64_t overruns, uint64_t underruns,
                       uint64_t in_clips, uint64_t out_clips)
{
    char buf[384];
    int n = snprintf(buf, sizeof(buf), "%s:", label);
    if (flags & STAT_DURATION)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " dur=%.1fs", (double)dur_ms / 1000.0);
    if (flags & STAT_INPUT_LEVELS)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " in=%.1fpk/%.1frms dBFS", in_peak, in_rms);
    if (flags & STAT_OUTPUT_LEVELS)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " out=%.1fpk/%.1frms dBFS", out_peak, out_rms);
    if (flags & STAT_ACTIVE_FLAGS)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " input_active=%d output_active=%d",
                      (int)input_active, (int)output_active);
    if (flags & STAT_INPUT_PATH)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " overruns=%llu", (unsigned long long)overruns);
    if (flags & STAT_IN_CLIPS)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " clips=%llu", (unsigned long long)in_clips);
    if (flags & STAT_OUTPUT_PATH)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " jitter=%.1fms late=%llu silence=%llu underruns=%llu",
                      jitter,
                      (unsigned long long)late,
                      (unsigned long long)silence,
                      (unsigned long long)underruns);
    if (flags & STAT_OUT_CLIPS)
        n += snprintf(buf + n, sizeof(buf) - n,
                      " clips=%llu", (unsigned long long)out_clips);
    sd_journal_print(priority, "%s", buf);
}

void telemetry_log(telemetry_t *tel,
                   audio_alsa_t          *alsa,
                   audio_proc_t          *in_proc,
                   audio_proc_t          *out_proc,
                   const logic_hid_t     *logic,
                   jitter_buffer_t       *jb)
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
    float jitter        = jb   ? jitter_buffer_estimate_ms(jb)    : 0.0f;
    uint64_t late       = jb   ? jitter_buffer_late_count(jb)     : 0;

    /* Detect rising edges to record transmission start times. */
    if (!tel->prev_input_active  && input_active)  tel->input_tx_start_ts  = now;
    if (!tel->prev_output_active && output_active) tel->output_tx_start_ts = now;

    /* INFO: one summary line on the falling edge of each transmission. */
    bool log_now = false;

    if (tel->prev_input_active && !input_active) {
        uint64_t dur_ms   = tel->input_tx_start_ts ? (now - tel->input_tx_start_ts) : 0;
        uint64_t in_clips = in_proc ? audio_proc_clip_count(in_proc) : 0;
        log_status(LOG_INFO, "input-end",
                   STAT_DURATION | STAT_INPUT_LEVELS | STAT_INPUT_PATH | STAT_IN_CLIPS,
                   dur_ms,
                   in_peak, in_rms, out_peak, out_rms,
                   input_active, output_active,
                   jitter, late, 0, overruns, underruns, in_clips, 0);
        log_now = true;
    }
    if (tel->prev_output_active && !output_active) {
        uint64_t dur_ms     = tel->output_tx_start_ts ? (now - tel->output_tx_start_ts) : 0;
        uint64_t tx_silence = jb ? jitter_buffer_latched_silence_count(jb) : 0;
        uint64_t out_clips  = out_proc ? audio_proc_clip_count(out_proc) : 0;
        log_status(LOG_INFO, "output-end",
                   STAT_DURATION | STAT_OUTPUT_LEVELS | STAT_OUTPUT_PATH | STAT_OUT_CLIPS,
                   dur_ms,
                   in_peak, in_rms, out_peak, out_rms,
                   input_active, output_active,
                   jitter, late, tx_silence, overruns, underruns, 0, out_clips);
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
        /* hb_silence accumulates mid-tx missed frames across all transmissions
         * since the last heartbeat — excludes inter-transmission idle pulls. */
        uint64_t hb_silence = jb ? jitter_buffer_hb_silence_count(jb) : 0;
        log_status(LOG_DEBUG, "heartbeat",
                   STAT_INPUT_LEVELS | STAT_OUTPUT_LEVELS | STAT_ACTIVE_FLAGS |
                   STAT_INPUT_PATH   | STAT_OUTPUT_PATH,
                   0,
                   in_peak, in_rms, out_peak, out_rms,
                   input_active, output_active,
                   jitter, late, hb_silence, overruns, underruns, 0, 0);
    } else if (now - tel->last_log_ts >=
                   (uint64_t)tel->cfg->logging.status_interval_sec * 1000u) {
        tel->last_log_ts = now;
    }
}

void telemetry_destroy(telemetry_t *tel)
{
    free(tel);
}
