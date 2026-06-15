#include "config.h"
#include "usrp_protocol.h"
#include "usrp_transport.h"
#include "audio_alsa.h"
#include "audio_processing.h"
#include "audio_trim.h"
#include "logic_hid.h"
#include "jitter_buffer.h"
#include "telemetry.h"
#include "watchdog.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <systemd/sd-journal.h>
#include <systemd/sd-daemon.h>

#define DEFAULT_CONFIG "/etc/rusrp/rusrp.toml"

static atomic_bool g_stop  = ATOMIC_VAR_INIT(false);
static atomic_int  g_floor = ATOMIC_VAR_INIT(FLOOR_IDLE);

static void sig_handler(int sig)
{
    (void)sig;
    atomic_store(&g_stop, true);
}

/* ── Input path: capture → DSP → USRP ──────────────────────────────────── */

typedef struct {
    const config_t          *cfg;
    usrp_transport_t        *transport;
    audio_proc_t            *in_proc;
    audio_trim_t            *in_trim;
    logic_hid_t             *logic;
    atomic_uint_fast32_t     tx_seq;
    atomic_bool              prev_keyed;
    atomic_uint_fast64_t     clip_count;
    atomic_int              *floor;
    bool                     floor_held;  /* only touched by capture callback */
} tx_ctx_t;

/* Called from the ALSA capture thread for each 160-sample frame. */
static void on_capture_frame(const int16_t *samples, size_t count, void *userdata)
{
    tx_ctx_t *ctx = userdata;

    int16_t frame[USRP_AUDIO_FRAMES];
    memcpy(frame, samples, count * sizeof(int16_t));

    uint64_t delta = 0;
    audio_proc_run(ctx->in_proc, frame, count, &delta);
    if (delta)
        atomic_fetch_add_explicit(&ctx->clip_count, delta, memory_order_relaxed);

    bool keyed     = logic_hid_input_active(ctx->logic);
    bool was_keyed = atomic_exchange_explicit(&ctx->prev_keyed, keyed,
                                              memory_order_relaxed);
    uint8_t  pkt[USRP_PKT_LEN];
    uint32_t seq;

    /* Rising edge: assert KEY immediately (channel capture), start trim. */
    if (keyed && !was_keyed) {
        bool proceed = true;
        if (ctx->cfg->logic.half_duplex) {
            if (ctx->floor_held) {
                /* Already hold floor (back-to-back input transmission). */
            } else {
                int expected = FLOOR_IDLE;
                proceed = atomic_compare_exchange_strong_explicit(
                    ctx->floor, &expected, (int)FLOOR_INPUT,
                    memory_order_acquire, memory_order_relaxed);
                if (!proceed)
                    sd_journal_print(LOG_INFO,
                                     "input: blocked (output active)");
            }
        }
        ctx->floor_held = proceed;
        if (proceed) {
            if (ctx->cfg->logging.level <= LOG_LEVEL_DEBUG)
                sd_journal_print(LOG_DEBUG, "input: active");
            audio_trim_tx_start(ctx->in_trim);
            seq = (uint32_t)atomic_fetch_add_explicit(&ctx->tx_seq, 1,
                                                       memory_order_relaxed);
            usrp_build_key(pkt, seq, 1);
            usrp_transport_send(ctx->transport, pkt, USRP_PKT_LEN);
        }
    }

    /* Snapshot trim state BEFORE the falling edge so drain-complete can detect
     * the TRIM_ACTIVE→TRIM_IDLE transition on the same frame that tx_end fires.
     * audio_trim_tx_end() sets TRIM_IDLE immediately (no multi-frame drain), so
     * trim_was_active must be captured here or the transition is invisible. */
    bool trim_was_active = audio_trim_active(ctx->in_trim);

    /* Falling edge: log, begin trailing drain (UNKEY sent after drain). */
    if (!keyed && was_keyed && ctx->floor_held) {
        if (ctx->cfg->logging.level <= LOG_LEVEL_DEBUG)
            sd_journal_print(LOG_DEBUG, "input: ended");
        audio_trim_tx_end(ctx->in_trim);
    }

    /* Route audio through the trim module.
     * During leading window: outputs silence.
     * During trailing drain: outputs buffered pre-edge frames (in ignored).
     * Returns false while the FIFO is filling or the module is idle. */
    int16_t trimmed[USRP_AUDIO_FRAMES];
    bool has_frame = audio_trim_process(ctx->in_trim,
                                        keyed ? frame : NULL,
                                        trimmed);
    if (has_frame) {
        seq = (uint32_t)atomic_fetch_add_explicit(&ctx->tx_seq, 1,
                                                   memory_order_relaxed);
        usrp_build_voice(pkt, seq, 1, trimmed);
        usrp_transport_send(ctx->transport, pkt, USRP_PKT_LEN);
    }

    /* Drain complete: send UNKEY on the falling edge of trim_active. */
    if (trim_was_active && !audio_trim_active(ctx->in_trim)) {
        seq = (uint32_t)atomic_fetch_add_explicit(&ctx->tx_seq, 1,
                                                   memory_order_relaxed);
        usrp_build_key(pkt, seq, 0);
        usrp_transport_send(ctx->transport, pkt, USRP_PKT_LEN);
        if (ctx->cfg->logic.half_duplex && ctx->floor_held) {
            atomic_store_explicit(ctx->floor, FLOOR_IDLE, memory_order_release);
            ctx->floor_held = false;
        }
    }
}

/* ── Output path: USRP → watchdog → jitter buffer ──────────────────────── */

typedef struct {
    watchdog_t      *wd;
    jitter_buffer_t *jb;
} rx_ctx_t;

static void on_usrp_packet(const usrp_packet_t *pkt, void *userdata)
{
    rx_ctx_t *ctx = userdata;

    watchdog_packet_received(ctx->wd);
    watchdog_key_event(ctx->wd, pkt->keyup != 0);

    if (pkt->type == USRP_TYPE_VOICE && pkt->keyup)
        jitter_buffer_push(ctx->jb, pkt->seq, pkt->audio);
}

/* ── Playback thread: jitter buffer → DSP → ALSA ────────────────────────── */

typedef struct {
    audio_alsa_t         *alsa;
    audio_proc_t         *out_proc;
    audio_trim_t         *out_trim;
    jitter_buffer_t      *jb;
    const logic_hid_t    *logic;
    atomic_bool          *stop;
    atomic_uint_fast64_t  clip_count;
} pb_ctx_t;

/* The ALSA blocking writei provides the 20 ms frame clock; no separate
 * sleep is needed.  Silence is injected by the jitter buffer when the
 * network is idle, keeping the ALSA stream flowing and preventing underruns. */
static void *playback_thread_fn(void *arg)
{
    pb_ctx_t *ctx = arg;
    int16_t   jb_frame[USRP_AUDIO_FRAMES];
    int16_t   trimmed[USRP_AUDIO_FRAMES];
    int16_t   work[USRP_AUDIO_FRAMES];
    static const int16_t silence[USRP_AUDIO_FRAMES];
    bool prev_output_active = false;

    while (!atomic_load_explicit(ctx->stop, memory_order_relaxed)) {
        bool output_active = logic_hid_output_active(ctx->logic);

        /* Detect transitions to drive the trim module. */
        if (output_active && !prev_output_active) {
            audio_trim_tx_start(ctx->out_trim);
            /* Reset per-transmission silence counter so output-end reflects
             * only missed frames from this transmission, not idle accumulation. */
            jitter_buffer_reset_silence_count(ctx->jb);
        } else if (!output_active && prev_output_active) {
            audio_trim_tx_end(ctx->out_trim);
            jitter_buffer_latch_silence(ctx->jb);
        }
        prev_output_active = output_active;

        /* Always pull from the jitter buffer to keep the playout window
         * advancing.  If the cursor stalls while the network keeps pushing,
         * incoming frames overflow the depth window and are dropped as late. */
        (void)jitter_buffer_pull(ctx->jb, jb_frame);

        const int16_t *to_write = silence;
        if (output_active) {
            if (audio_trim_process(ctx->out_trim, jb_frame, trimmed)) {
                memcpy(work, trimmed, sizeof(work));
                uint64_t delta = 0;
                audio_proc_run(ctx->out_proc, work, USRP_AUDIO_FRAMES, &delta);
                if (delta)
                    atomic_fetch_add_explicit(&ctx->clip_count, delta,
                                              memory_order_relaxed);
                to_write = work;
            }
        }

        audio_alsa_write(ctx->alsa, to_write, USRP_AUDIO_FRAMES);
    }
    return NULL;
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
    sd_journal_print(LOG_INFO, "rusrp starting, config: %s", config_path);

    /* ── signals ── */
    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ── module handles ── */
    logic_hid_t      *logic     = NULL;
    jitter_buffer_t  *jb        = NULL;
    watchdog_t       *wd        = NULL;
    usrp_transport_t *transport = NULL;
    audio_proc_t     *in_proc   = NULL;
    audio_proc_t     *out_proc  = NULL;
    audio_alsa_t     *alsa      = NULL;
    telemetry_t      *tel       = NULL;
    audio_trim_t     *in_trim   = NULL;
    audio_trim_t     *out_trim  = NULL;
    pthread_t         pb_thread;
    bool              pb_started = false;

    /* ── init: HID logic first (holds fail-safe state) ── */
    if (logic_hid_create(&logic, &cfg) != 0)                           goto fail;
    if (jitter_buffer_create(&jb, cfg.network.jitter_buffer_ms) != 0) goto fail;
    if (watchdog_create(&wd, &cfg, logic, jb, &g_floor) != 0)         goto fail;

    /* ── init: USRP transport ── */
    rx_ctx_t rx_ctx = { .wd = wd, .jb = jb };
    if (usrp_transport_create(&transport, &cfg, on_usrp_packet, &rx_ctx) != 0)
        goto fail;

    /* ── init: audio processing chains ── */
    if (audio_proc_create(&in_proc,
                          cfg.audio.input_highpass,
                          cfg.audio.input_gain_db)  != 0) goto fail;
    if (audio_proc_create(&out_proc,
                          cfg.audio.output_highpass,
                          cfg.audio.output_gain_db) != 0) goto fail;

    /* ── init: audio trim ── */
    if (audio_trim_create(&in_trim,
                          cfg.audio.input_leading_trim_ms  / 20,
                          cfg.audio.input_trailing_trim_ms / 20) != 0) goto fail;
    if (audio_trim_create(&out_trim,
                          cfg.audio.output_leading_trim_ms  / 20,
                          cfg.audio.output_trailing_trim_ms / 20) != 0) goto fail;

    /* ── init: ALSA (capture callback drives the input path) ── */
    tx_ctx_t tx_ctx = {0};
    atomic_init(&tx_ctx.tx_seq,     0);
    atomic_init(&tx_ctx.prev_keyed, false);
    atomic_init(&tx_ctx.clip_count, 0);
    tx_ctx.cfg         = &cfg;
    tx_ctx.transport   = transport;
    tx_ctx.in_proc     = in_proc;
    tx_ctx.in_trim     = in_trim;
    tx_ctx.logic       = logic;
    tx_ctx.floor       = &g_floor;
    tx_ctx.floor_held  = false;

    if (audio_alsa_create(&alsa, &cfg, on_capture_frame, &tx_ctx) != 0)
        goto fail;
    watchdog_set_alsa(wd, alsa);

    /* ── init: playback thread ── */
    pb_ctx_t pb_ctx = {0};
    atomic_init(&pb_ctx.clip_count, 0);
    pb_ctx.alsa     = alsa;
    pb_ctx.out_proc = out_proc;
    pb_ctx.out_trim = out_trim;
    pb_ctx.jb       = jb;
    pb_ctx.logic    = logic;
    pb_ctx.stop     = &g_stop;

    if (pthread_create(&pb_thread, NULL, playback_thread_fn, &pb_ctx) != 0) {
        sd_journal_print(LOG_ERR, "main: playback pthread_create: %m");
        goto fail;
    }
    pb_started = true;

    /* ── init: telemetry ── */
    if (telemetry_create(&tel, &cfg) != 0) goto fail;

    /* ── ready ── */
    sd_notify(0, "READY=1");
    sd_journal_print(LOG_INFO, "rusrp ready");

    /* ── main loop ── */
    while (!atomic_load(&g_stop)) {
        sleep(1);
        telemetry_log(tel, alsa, in_proc, out_proc, logic, jb);
    }

    sd_journal_print(LOG_INFO, "rusrp stopping");
    sd_notify(0, "STOPPING=1");

    /* ── shutdown: reverse init order ──
     * Critical ordering:
     *   1. telemetry first (read-only, no active threads)
     *   2. pb_thread: sees g_stop, exits after current 20 ms writei completes
     *   3. audio_alsa: stops capture thread (which uses transport/in_proc/logic),
     *      then closes both PCM handles — safe because pb_thread is already done
     *   4. everything else: no audio threads remain */
    telemetry_destroy(tel);
    if (pb_started)   pthread_join(pb_thread, NULL);
    audio_alsa_destroy(alsa);
    audio_trim_destroy(out_trim);
    audio_trim_destroy(in_trim);
    audio_proc_destroy(out_proc);
    audio_proc_destroy(in_proc);
    usrp_transport_destroy(transport);
    watchdog_destroy(wd);
    jitter_buffer_destroy(jb);
    logic_hid_destroy(logic);   /* releases output_active last — fail-safe */

    return 0;

fail:
    sd_journal_print(LOG_ERR, "rusrp: initialization failed");
    sd_notify(0, "STOPPING=1");
    /* Signal playback thread to stop before joining. */
    atomic_store(&g_stop, true);
    if (pb_started)   pthread_join(pb_thread, NULL);
    audio_alsa_destroy(alsa);
    audio_trim_destroy(out_trim);
    audio_trim_destroy(in_trim);
    audio_proc_destroy(out_proc);
    audio_proc_destroy(in_proc);
    usrp_transport_destroy(transport);
    watchdog_destroy(wd);
    jitter_buffer_destroy(jb);
    logic_hid_destroy(logic);
    return 1;
}
