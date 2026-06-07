#include "config.h"
#include "usrp_protocol.h"
#include "usrp_transport.h"
#include "audio_alsa.h"
#include "audio_processing.h"
#include "logic_hid.h"
#include "jitter_buffer.h"
#include "telemetry.h"
#include "watchdog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
#include <systemd/sd-journal.h>
#include <systemd/sd-daemon.h>

#define DEFAULT_CONFIG "/etc/usrp-remote-link/usrp-remote-link.toml"

static atomic_bool g_stop = ATOMIC_VAR_INIT(false);

static void sig_handler(int sig)
{
    (void)sig;
    atomic_store(&g_stop, true);
}

/* ── USRP RX callback (called from transport RX thread) ─────────────────── */

typedef struct {
    watchdog_t      *wd;
    jitter_buffer_t *jb;
} rx_ctx_t;

static void on_usrp_packet(const usrp_packet_t *pkt, void *userdata)
{
    rx_ctx_t *ctx = userdata;

    watchdog_packet_received(ctx->wd);

    if (pkt->keyup)
        watchdog_key_event(ctx->wd, true);
    else
        watchdog_key_event(ctx->wd, false);

    if (pkt->type == USRP_TYPE_VOICE && pkt->keyup)
        jitter_buffer_push(ctx->jb, pkt->seq, pkt->audio);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *config_path = DEFAULT_CONFIG;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0)
            && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-c config_file]\n", argv[0]);
            printf("  Default config: %s\n", DEFAULT_CONFIG);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    /* ── config ── */
    config_t cfg;
    if (config_load(&cfg, config_path) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", config_path);
        return 1;
    }

    sd_journal_print(LOG_INFO, "usrp-remote-link starting, config: %s", config_path);

    /* ── signals ── */
    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ── modules ── */
    logic_hid_t     *logic = NULL;
    jitter_buffer_t *jb    = NULL;
    watchdog_t      *wd    = NULL;
    usrp_transport_t *transport = NULL;
    telemetry_t      *tel  = NULL;

    if (logic_hid_create(&logic, &cfg) != 0)    goto fail;
    if (jitter_buffer_create(&jb, cfg.network.jitter_buffer_ms) != 0) goto fail;
    if (watchdog_create(&wd, &cfg, logic, jb) != 0) goto fail;

    rx_ctx_t rx_ctx = { .wd = wd, .jb = jb };

    if (usrp_transport_create(&transport, &cfg, on_usrp_packet, &rx_ctx) != 0)
        goto fail;
    if (telemetry_create(&tel, &cfg) != 0) goto fail;

    /* ── ready ── */
    sd_notify(0, "READY=1");
    sd_journal_print(LOG_INFO, "usrp-remote-link ready");

    /* ── main loop ── */
    while (!atomic_load(&g_stop)) {
        sleep(1);
        telemetry_log(tel, NULL, NULL, NULL, logic, jb);
    }

    sd_journal_print(LOG_INFO, "usrp-remote-link stopping");

    /* ── shutdown (reverse init order) ── */
    telemetry_destroy(tel);
    usrp_transport_destroy(transport);
    watchdog_destroy(wd);
    jitter_buffer_destroy(jb);
    logic_hid_destroy(logic);   /* releases output_active last */

    sd_notify(0, "STOPPING=1");
    return 0;

fail:
    sd_journal_print(LOG_ERR, "usrp-remote-link: initialization failed");
    watchdog_destroy(wd);
    jitter_buffer_destroy(jb);
    logic_hid_destroy(logic);
    return 1;
}
