#include "config.h"
#include "toml.h"

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
    cfg->audio.input_leading_trim_ms   = 0;
    cfg->audio.input_trailing_trim_ms  = 0;
    cfg->audio.output_leading_trim_ms  = 0;
    cfg->audio.output_trailing_trim_ms = 0;

    strncpy(cfg->logic.hid_device, "/dev/hidraw0", sizeof(cfg->logic.hid_device) - 1);
    cfg->logic.output_active_gpio = 3;
    cfg->logic.input_active_low   = true;
    cfg->logic.output_active_low  = true;

    cfg->network.jitter_buffer_ms = 100;

    cfg->watchdog.network_timeout_ms        = 500;
    cfg->watchdog.output_active_tail_ms     = 100;
    cfg->watchdog.startup_output_inhibit_ms = 2000;

    cfg->logging.level               = LOG_LEVEL_INFO;
    cfg->logging.status_interval_sec = 10;
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void read_str(toml_table_t *tbl, const char *key,
                     char *dst, size_t dstsz)
{
    toml_datum_t d = toml_string_in(tbl, key);
    if (d.ok) {
        strncpy(dst, d.u.s, dstsz - 1);
        dst[dstsz - 1] = '\0';
        free(d.u.s);
    }
}

static void read_uint16(toml_table_t *tbl, const char *key, uint16_t *dst)
{
    toml_datum_t d = toml_int_in(tbl, key);
    if (d.ok && d.u.i > 0 && d.u.i <= 65535)
        *dst = (uint16_t)d.u.i;
}

static void read_uint(toml_table_t *tbl, const char *key, unsigned int *dst)
{
    toml_datum_t d = toml_int_in(tbl, key);
    if (d.ok && d.u.i >= 0)
        *dst = (unsigned int)d.u.i;
}

static void read_int(toml_table_t *tbl, const char *key, int *dst)
{
    toml_datum_t d = toml_int_in(tbl, key);
    if (d.ok)
        *dst = (int)d.u.i;
}

static void read_float(toml_table_t *tbl, const char *key, float *dst)
{
    toml_datum_t d = toml_double_in(tbl, key);
    if (d.ok)
        *dst = (float)d.u.d;
}

static void read_bool(toml_table_t *tbl, const char *key, bool *dst)
{
    toml_datum_t d = toml_bool_in(tbl, key);
    if (d.ok)
        *dst = (bool)d.u.b;
}

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

    return ok ? 0 : -1;
}

/* ── public ───────────────────────────────────────────────────────────────── */

int config_load(config_t *cfg, const char *path)
{
    config_defaults(cfg);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        sd_journal_print(LOG_ERR, "config: cannot open %s: %m", path);
        return -1;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);

    if (!root) {
        sd_journal_print(LOG_ERR, "config: parse error in %s: %s", path, errbuf);
        return -1;
    }

    toml_table_t *t;

    if ((t = toml_table_in(root, "usrp"))) {
        read_str(t,    "remote_host",  cfg->usrp.remote_host,
                 sizeof(cfg->usrp.remote_host));
        read_uint16(t, "remote_port",  &cfg->usrp.remote_port);
        read_uint16(t, "local_port",   &cfg->usrp.local_port);
        read_str(t,    "bind_address", cfg->usrp.bind_address,
                 sizeof(cfg->usrp.bind_address));
    }

    if ((t = toml_table_in(root, "audio"))) {
        read_str(t,   "alsa_device",     cfg->audio.alsa_device,
                 sizeof(cfg->audio.alsa_device));
        read_float(t, "input_gain_db",   &cfg->audio.input_gain_db);
        read_float(t, "output_gain_db",  &cfg->audio.output_gain_db);
        read_bool(t,  "input_highpass",  &cfg->audio.input_highpass);
        read_bool(t,  "output_highpass", &cfg->audio.output_highpass);
        read_uint(t,  "input_leading_trim_ms",   &cfg->audio.input_leading_trim_ms);
        read_uint(t,  "input_trailing_trim_ms",  &cfg->audio.input_trailing_trim_ms);
        read_uint(t,  "output_leading_trim_ms",  &cfg->audio.output_leading_trim_ms);
        read_uint(t,  "output_trailing_trim_ms", &cfg->audio.output_trailing_trim_ms);
    }

    /* Round trim values to 20 ms frame boundaries. */
    cfg->audio.input_leading_trim_ms   = round_trim_ms(cfg->audio.input_leading_trim_ms,
                                                        "audio.input_leading_trim_ms");
    cfg->audio.input_trailing_trim_ms  = round_trim_ms(cfg->audio.input_trailing_trim_ms,
                                                        "audio.input_trailing_trim_ms");
    cfg->audio.output_leading_trim_ms  = round_trim_ms(cfg->audio.output_leading_trim_ms,
                                                        "audio.output_leading_trim_ms");
    cfg->audio.output_trailing_trim_ms = round_trim_ms(cfg->audio.output_trailing_trim_ms,
                                                        "audio.output_trailing_trim_ms");

    if ((t = toml_table_in(root, "logic"))) {
        read_str(t,  "hid_device",         cfg->logic.hid_device,
                 sizeof(cfg->logic.hid_device));
        read_int(t,  "output_active_gpio", &cfg->logic.output_active_gpio);
        read_bool(t, "input_active_low",   &cfg->logic.input_active_low);
        read_bool(t, "output_active_low",  &cfg->logic.output_active_low);
        read_bool(t, "half_duplex",        &cfg->logic.half_duplex);
    }

    if ((t = toml_table_in(root, "network")))
        read_uint(t, "jitter_buffer_ms", &cfg->network.jitter_buffer_ms);

    if ((t = toml_table_in(root, "watchdog"))) {
        read_uint(t, "network_timeout_ms",        &cfg->watchdog.network_timeout_ms);
        read_uint(t, "output_active_tail_ms",     &cfg->watchdog.output_active_tail_ms);
        read_uint(t, "startup_output_inhibit_ms", &cfg->watchdog.startup_output_inhibit_ms);
    }

    if ((t = toml_table_in(root, "logging"))) {
        read_uint(t, "status_interval_sec", &cfg->logging.status_interval_sec);
        toml_datum_t d = toml_string_in(t, "level");
        if (d.ok) {
            if      (strcmp(d.u.s, "debug") == 0) cfg->logging.level = LOG_LEVEL_DEBUG;
            else if (strcmp(d.u.s, "info")  == 0) cfg->logging.level = LOG_LEVEL_INFO;
            else if (strcmp(d.u.s, "warn")  == 0) cfg->logging.level = LOG_LEVEL_WARN;
            else if (strcmp(d.u.s, "error") == 0) cfg->logging.level = LOG_LEVEL_ERROR;
            free(d.u.s);
        }
    }

    toml_free(root);
    return validate(cfg);
}
