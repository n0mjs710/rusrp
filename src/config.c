#include "config.h"
#include "conf.h"

#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-journal.h>

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->usrp.remote_host, "127.0.0.1",  sizeof(cfg->usrp.remote_host) - 1);
    cfg->usrp.remote_port = 34001;
    cfg->usrp.local_port  = 32001;
    strncpy(cfg->usrp.bind_address, "0.0.0.0", sizeof(cfg->usrp.bind_address) - 1);

    strncpy(cfg->audio.alsa_device, "hw:1,0", sizeof(cfg->audio.alsa_device) - 1);
    cfg->audio.input_gain_db    = 0.0f;
    cfg->audio.output_gain_db   = 0.0f;
    cfg->audio.input_highpass   = false;
    cfg->audio.output_highpass  = false;
    cfg->audio.input_bandpass   = false;
    cfg->audio.output_bandpass  = false;
    cfg->audio.input_leading_trim_ms  = 0;
    cfg->audio.output_leading_trim_ms = 0;
    cfg->audio.codec         = AUDIO_CODEC_PCM;
    cfg->audio.opus_bitrate  = 8000;
    cfg->audio.opus_frame_ms = 60;

    strncpy(cfg->logic.hid_device, "/dev/hidraw0", sizeof(cfg->logic.hid_device) - 1);
    cfg->logic.output_active_gpio = 3;
    cfg->logic.input_active_low   = true;
    cfg->logic.output_active_low  = true;

    cfg->network.jitter_buffer_ms = 100;

    cfg->watchdog.network_timeout_ms        = 500;
    cfg->watchdog.output_active_tail_ms     = 100;
    cfg->watchdog.startup_output_inhibit_ms = 2000;
    cfg->watchdog.unkey_debounce_ms         = 0;

    cfg->logging.level               = LOG_LEVEL_INFO;
    cfg->logging.status_interval_sec = 10;
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

/* Round ms to the nearest 20 ms frame boundary and clamp to 0–260 ms.
 * Logs a warning if the value was changed so the user knows what was applied. */
static unsigned int round_trim_ms(unsigned int ms, const char *name)
{
    unsigned int rounded = ((ms + 10u) / 20u) * 20u;
    if (rounded > 260u) rounded = 260u;
    if (rounded != ms) {
        fprintf(stderr, "config: %s = %u rounded to %u ms (nearest 20 ms frame)\n",
                name, ms, rounded);
        sd_journal_print(LOG_WARNING,
                         "config: %s = %u rounded to %u ms (nearest 20 ms frame)",
                         name, ms, rounded);
    }
    return rounded;
}

/* ── validation ───────────────────────────────────────────────────────────── */

static int validate(const config_t *cfg)
{
    int ok = 1;

    if (cfg->usrp.remote_host[0] == '\0') {
        fprintf(stderr, "config: usrp.remote_host is required\n");
        sd_journal_print(LOG_ERR, "config: usrp.remote_host is required");
        ok = 0;
    }
    if (cfg->usrp.remote_port == 0 || cfg->usrp.local_port == 0) {
        fprintf(stderr, "config: usrp ports must be 1–65535\n");
        sd_journal_print(LOG_ERR, "config: usrp ports must be 1–65535");
        ok = 0;
    }
    if (cfg->audio.input_gain_db < -12.0f || cfg->audio.input_gain_db > 12.0f ||
        cfg->audio.output_gain_db < -12.0f || cfg->audio.output_gain_db > 12.0f) {
        fprintf(stderr, "config: gain_db must be in range -12 to +12\n");
        sd_journal_print(LOG_ERR, "config: gain_db must be in range -12 to +12");
        ok = 0;
    }
    if (cfg->network.jitter_buffer_ms < 40 || cfg->network.jitter_buffer_ms > 250) {
        fprintf(stderr, "config: jitter_buffer_ms = %u is invalid; must be 40–250 ms\n",
                cfg->network.jitter_buffer_ms);
        sd_journal_print(LOG_ERR, "config: jitter_buffer_ms = %u is invalid; must be 40–250 ms",
                         cfg->network.jitter_buffer_ms);
        ok = 0;
    }
    if (cfg->logic.hid_device[0] == '\0') {
        fprintf(stderr, "config: logic.hid_device is required\n");
        sd_journal_print(LOG_ERR, "config: logic.hid_device is required");
        ok = 0;
    }
    if (cfg->audio.codec == AUDIO_CODEC_OPUS) {
        if (cfg->audio.opus_bitrate < 4000 || cfg->audio.opus_bitrate > 64000) {
            fprintf(stderr, "config: opus_bitrate must be 4000–64000 bps\n");
            sd_journal_print(LOG_ERR, "config: opus_bitrate must be 4000–64000 bps");
            ok = 0;
        }
        if (cfg->audio.opus_frame_ms != 20 && cfg->audio.opus_frame_ms != 40
            && cfg->audio.opus_frame_ms != 60) {
            fprintf(stderr, "config: opus_frame_ms must be 20, 40, or 60\n");
            sd_journal_print(LOG_ERR, "config: opus_frame_ms must be 20, 40, or 60");
            ok = 0;
        }
    }

    return ok ? 0 : -1;
}

/* ── public ───────────────────────────────────────────────────────────────── */

#define F(sec, key, type, member) \
    { sec, #key, type, offsetof(config_t, member), sizeof(((config_t *)0)->member), NULL }
#define FC(sec, key, member, choices) \
    { sec, #key, CONF_CHOICE, offsetof(config_t, member), 0, choices }

static const char *const CODECS[] = { "pcm", "opus", NULL };                   /* audio_codec_t */
static const char *const LEVELS[] = { "debug", "info", "warn", "error", NULL }; /* log_level_t */

static const conf_field_t FIELDS[] = {
    F("usrp", remote_host,  CONF_STR,  usrp.remote_host),
    F("usrp", remote_port,  CONF_PORT, usrp.remote_port),
    F("usrp", local_port,   CONF_PORT, usrp.local_port),
    F("usrp", bind_address, CONF_STR,  usrp.bind_address),

    F("audio", alsa_device,            CONF_STR,   audio.alsa_device),
    F("audio", input_gain_db,          CONF_FLOAT, audio.input_gain_db),
    F("audio", output_gain_db,         CONF_FLOAT, audio.output_gain_db),
    F("audio", input_highpass,         CONF_BOOL,  audio.input_highpass),
    F("audio", output_highpass,        CONF_BOOL,  audio.output_highpass),
    F("audio", input_bandpass,         CONF_BOOL,  audio.input_bandpass),
    F("audio", output_bandpass,        CONF_BOOL,  audio.output_bandpass),
    F("audio", input_leading_trim_ms,  CONF_UINT,  audio.input_leading_trim_ms),
    F("audio", output_leading_trim_ms, CONF_UINT,  audio.output_leading_trim_ms),
    FC("audio", codec, audio.codec, CODECS),
    F("audio", opus_bitrate,           CONF_INT,   audio.opus_bitrate),
    F("audio", opus_frame_ms,          CONF_INT,   audio.opus_frame_ms),

    F("logic", hid_device,         CONF_STR,  logic.hid_device),
    F("logic", output_active_gpio, CONF_INT,  logic.output_active_gpio),
    F("logic", input_active_low,   CONF_BOOL, logic.input_active_low),
    F("logic", output_active_low,  CONF_BOOL, logic.output_active_low),
    F("logic", half_duplex,        CONF_BOOL, logic.half_duplex),

    F("network", jitter_buffer_ms, CONF_UINT, network.jitter_buffer_ms),

    F("watchdog", network_timeout_ms,        CONF_UINT, watchdog.network_timeout_ms),
    F("watchdog", output_active_tail_ms,     CONF_UINT, watchdog.output_active_tail_ms),
    F("watchdog", startup_output_inhibit_ms, CONF_UINT, watchdog.startup_output_inhibit_ms),
    F("watchdog", unkey_debounce_ms,         CONF_UINT, watchdog.unkey_debounce_ms),

    FC("logging", level, logging.level, LEVELS),
    F("logging", status_interval_sec, CONF_UINT, logging.status_interval_sec),
    { 0 }
};

int config_load(config_t *cfg, const char *path)
{
    config_defaults(cfg);

    conf_t c;
    if (conf_read(&c, path) != 0) {
        sd_journal_print(LOG_ERR, "config: cannot open %s: %m", path);
        fprintf(stderr, "config: cannot open %s\n", path);
        return -1;
    }
    int errors = conf_apply(&c, FIELDS, cfg, NULL);
    conf_free(&c);
    if (errors) {
        sd_journal_print(LOG_ERR, "config: %d error(s) in %s", errors, path);
        return -1;
    }

    cfg->audio.input_leading_trim_ms  = round_trim_ms(cfg->audio.input_leading_trim_ms,
                                                       "audio.input_leading_trim_ms");
    cfg->audio.output_leading_trim_ms = round_trim_ms(cfg->audio.output_leading_trim_ms,
                                                       "audio.output_leading_trim_ms");
    return validate(cfg);
}
