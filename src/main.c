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
#include "opus_codec.h"
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

#define DEFAULT_CONFIG "/etc/rusrp/rusrp.conf"

static atomic_bool g_stop  = ATOMIC_VAR_INIT(false);
static atomic_int  g_floor = ATOMIC_VAR_INIT(FLOOR_IDLE);

static void sig_handler(int sig)
{
    (void)sig;
    atomic_store(&g_stop, true);
}

/* ── Input path: capture → DSP → USRP ──────────────────────────────────── */

typedef struct tx_ctx {
    const config_t          *cfg;
    usrp_transport_t        *transport;
    audio_proc_t            *in_proc;
    audio_trim_t            *in_trim;
    logic_hid_t             *logic;
    audio_alsa_t            *alsa;
    telemetry_t             *tel;
    atomic_uint_fast32_t     tx_seq;
    bool                     prev_keyed;
    atomic_int              *floor;
    bool                     floor_held;
    void                   (*send)(struct tx_ctx *, const uint8_t *, size_t);
    usrp_rx_cb_t             loopback_cb;
    void                    *loopback_userdata;
    /* Opus encode state — NULL in PCM mode */
    opus_codec_t            *opus_enc;
    int16_t                  opus_accum[480]; /* max 60 ms accumulation buffer */
    int                      opus_accum_n;    /* samples currently accumulated */
} tx_ctx_t;

static void send_network(tx_ctx_t *ctx, const uint8_t *raw, size_t len)
{
    usrp_transport_send(ctx->transport, raw, len);
}

static void send_loopback(tx_ctx_t *ctx, const uint8_t *raw, size_t len)
{
    usrp_packet_t lpkt;
    if (usrp_parse(raw, len, &lpkt) == 0)
        ctx->loopback_cb(&lpkt, ctx->loopback_userdata);
}

/* Encode the full accumulation buffer and send one Opus packet.
 * Advances tx_seq by the number of 20 ms sub-frames the packet covers.
 * Resets opus_accum_n to 0.  Assumes accum is already full (padded if needed). */
static void flush_opus_frame(tx_ctx_t *ctx)
{
    uint8_t  pkt[USRP_PKT_LEN];   /* USRP_PKT_LEN > USRP_HEADER_LEN + USRP_OPUS_MAX_BYTES */
    uint8_t  opus_buf[USRP_OPUS_MAX_BYTES];

    int opus_len = opus_codec_encode(ctx->opus_enc, ctx->opus_accum, opus_buf);
    if (opus_len > 0) {
        int frame_samples = opus_codec_frame_samples(ctx->opus_enc);
        int sub_frames    = frame_samples / (int)USRP_AUDIO_FRAMES;
        uint32_t seq = (uint32_t)atomic_fetch_add_explicit(&ctx->tx_seq,
                                  (unsigned)sub_frames, memory_order_relaxed);
        size_t pkt_len = usrp_build_opus_voice(pkt, seq, 1,
                                               opus_buf, (size_t)opus_len);
        ctx->send(ctx, pkt, pkt_len);
    }
    ctx->opus_accum_n = 0;
}

/* Called from the ALSA capture thread for each 160-sample frame. */
static void on_capture_frame(const int16_t *samples, size_t count, void *userdata)
{
    tx_ctx_t *ctx = userdata;

    int16_t frame[USRP_AUDIO_FRAMES];
    memcpy(frame, samples, count * sizeof(int16_t));

    audio_proc_run(ctx->in_proc, frame, count);

    bool keyed     = logic_hid_input_active(ctx->logic);
    bool was_keyed = ctx->prev_keyed;
    ctx->prev_keyed = keyed;
    uint8_t  pkt[USRP_PKT_LEN];
    uint32_t seq;

    /* Rising edge: claim floor (half-duplex), start trim, send KEY. */
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
                    sd_journal_print(LOG_INFO, "input: blocked (output active)");
            }
        }
        ctx->floor_held = proceed;
        if (proceed) {
            if (ctx->opus_enc != NULL)
                ctx->opus_accum_n = 0;
            telemetry_input_start(ctx->tel);
            if (ctx->cfg->logging.level <= LOG_LEVEL_DEBUG)
                sd_journal_print(LOG_DEBUG, "input: active");
            audio_trim_tx_start(ctx->in_trim);
            seq = (uint32_t)atomic_fetch_add_explicit(&ctx->tx_seq, 1,
                                                       memory_order_relaxed);
            usrp_build_key(pkt, seq, 1);
            ctx->send(ctx, pkt, USRP_PKT_LEN);
        }
    }

    /* Falling edge: flush any partial Opus frame, send UNKEY, release floor. */
    if (!keyed && was_keyed && ctx->floor_held) {
        if (ctx->cfg->logging.level <= LOG_LEVEL_DEBUG)
            sd_journal_print(LOG_DEBUG, "input: ended");
        audio_trim_tx_end(ctx->in_trim);

        /* Pad and flush any partially-filled Opus accumulation buffer. */
        if (ctx->opus_enc != NULL && ctx->opus_accum_n > 0) {
            int frame_samples = opus_codec_frame_samples(ctx->opus_enc);
            memset(ctx->opus_accum + ctx->opus_accum_n, 0,
                   (size_t)(frame_samples - ctx->opus_accum_n) * sizeof(int16_t));
            ctx->opus_accum_n = frame_samples;  /* mark full for flush_opus_frame */
            flush_opus_frame(ctx);
        }

        seq = (uint32_t)atomic_fetch_add_explicit(&ctx->tx_seq, 1,
                                                   memory_order_relaxed);
        usrp_build_key(pkt, seq, 0);
        ctx->send(ctx, pkt, USRP_PKT_LEN);
        telemetry_input_end(ctx->tel, ctx->alsa, ctx->in_proc);
        if (ctx->cfg->logic.half_duplex) {
            atomic_store_explicit(ctx->floor, FLOOR_IDLE, memory_order_release);
            ctx->floor_held = false;
        }
    }

    /* Per-frame audio: trim then send (PCM) or accumulate (Opus). */
    int16_t trimmed[USRP_AUDIO_FRAMES];
    bool has_frame = audio_trim_process(ctx->in_trim,
                                        keyed ? frame : NULL,
                                        trimmed);
    if (has_frame) {
        if (ctx->opus_enc == NULL) {
            /* PCM mode: one 20 ms frame → one packet. */
            seq = (uint32_t)atomic_fetch_add_explicit(&ctx->tx_seq, 1,
                                                       memory_order_relaxed);
            usrp_build_voice(pkt, seq, 1, trimmed);
            ctx->send(ctx, pkt, USRP_PKT_LEN);
        } else {
            /* Opus mode: accumulate; send when a full frame is ready. */
            memcpy(ctx->opus_accum + ctx->opus_accum_n, trimmed,
                   USRP_AUDIO_FRAMES * sizeof(int16_t));
            ctx->opus_accum_n += (int)USRP_AUDIO_FRAMES;

            if (ctx->opus_accum_n >= opus_codec_frame_samples(ctx->opus_enc))
                flush_opus_frame(ctx);
        }
    }
}

/* ── Output path: USRP → watchdog → jitter buffer ──────────────────────── */

typedef struct {
    watchdog_t      *wd;
    jitter_buffer_t *jb;
    opus_codec_t    *opus_dec;  /* NULL in PCM mode */
} rx_ctx_t;

static void on_usrp_packet(const usrp_packet_t *pkt, void *userdata)
{
    rx_ctx_t *ctx = userdata;

    watchdog_packet_received(ctx->wd);
    watchdog_key_event(ctx->wd, pkt->keyup != 0);

    if (!pkt->keyup)
        return;

    if (pkt->type == USRP_TYPE_VOICE) {
        jitter_buffer_push(ctx->jb, pkt->audio);
    } else if (pkt->type == USRP_TYPE_OPUS && ctx->opus_dec != NULL) {
        int16_t pcm[480];
        int n = opus_codec_decode(ctx->opus_dec,
                                  pkt->opus, (int)pkt->opus_len, pcm);
        if (n > 0) {
            /* Split decoded frame into 20 ms sub-frames for the jitter buffer. */
            int sub_frames = n / (int)USRP_AUDIO_FRAMES;
            for (int i = 0; i < sub_frames; i++)
                jitter_buffer_push(ctx->jb, pcm + i * (int)USRP_AUDIO_FRAMES);
        }
    }
}

/* ── Playback thread: jitter buffer → DSP → ALSA ────────────────────────── */

typedef struct {
    audio_alsa_t         *alsa;
    audio_proc_t         *out_proc;
    audio_trim_t         *out_trim;
    jitter_buffer_t      *jb;
    const logic_hid_t    *logic;
    telemetry_t          *tel;
    atomic_bool          *stop;
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
            jitter_buffer_reset_silence_count(ctx->jb);
            telemetry_output_start(ctx->tel);
        } else if (!output_active && prev_output_active) {
            audio_trim_tx_end(ctx->out_trim);
            jitter_buffer_latch_silence(ctx->jb);
            telemetry_output_end(ctx->tel, ctx->alsa, ctx->out_proc, ctx->jb);
        }
        prev_output_active = output_active;

        /* Always drain the jitter buffer, even while output is inactive
         * (silence is discarded below in that case) -- this is what keeps
         * the FIFO from growing unbounded latency during the watchdog's
         * deferred-PTT window; see watchdog.c's ptt_ready_ts. */
        (void)jitter_buffer_pull(ctx->jb, jb_frame);

        const int16_t *to_write = silence;
        if (output_active) {
            if (audio_trim_process(ctx->out_trim, jb_frame, trimmed)) {
                memcpy(work, trimmed, sizeof(work));
                audio_proc_run(ctx->out_proc, work, USRP_AUDIO_FRAMES);
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
    bool        loopback    = false;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0)
            && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--loopback") == 0) {
            loopback = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-c config_file] [-l]\n", argv[0]);
            printf("  -c, --config   Config file (default: %s)\n", DEFAULT_CONFIG);
            printf("  -l, --loopback Digital loopback: full-duplex, no network\n");
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
    if (loopback)
        cfg.logic.half_duplex = false;
    sd_journal_print(LOG_INFO, "rusrp starting, config: %s%s%s", config_path,
                     loopback ? " (loopback)" : "",
                     cfg.audio.codec == AUDIO_CODEC_OPUS ? " (opus)" : "");

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
    opus_codec_t     *opus      = NULL;
    pthread_t         pb_thread;
    bool              pb_started = false;

    /* ── init: HID logic first (holds fail-safe state) ── */
    if (logic_hid_create(&logic, &cfg) != 0)                           goto fail;
    if (jitter_buffer_create(&jb, cfg.network.jitter_buffer_ms) != 0) goto fail;
    if (watchdog_create(&wd, &cfg, logic, jb, &g_floor) != 0)         goto fail;

    /* ── init: Opus codec (skipped in PCM mode) ── */
    if (cfg.audio.codec == AUDIO_CODEC_OPUS) {
        if (opus_codec_create(&opus,
                              cfg.audio.opus_bitrate,
                              cfg.audio.opus_frame_ms) != 0)
            goto fail;
    }

    /* ── init: USRP transport (skipped in loopback mode) ── */
    rx_ctx_t rx_ctx = { .wd = wd, .jb = jb, .opus_dec = opus };
    if (!loopback) {
        if (usrp_transport_create(&transport, &cfg, on_usrp_packet, &rx_ctx) != 0)
            goto fail;
    }

    /* ── init: audio processing chains ── */
    if (audio_proc_create(&in_proc,
                          cfg.audio.input_highpass,
                          cfg.audio.input_bandpass,
                          cfg.audio.input_gain_db)  != 0) goto fail;
    if (audio_proc_create(&out_proc,
                          cfg.audio.output_highpass,
                          cfg.audio.output_bandpass,
                          cfg.audio.output_gain_db) != 0) goto fail;

    /* ── init: audio trim ── */
    if (audio_trim_create(&in_trim,
                          cfg.audio.input_leading_trim_ms / 20) != 0) goto fail;
    if (audio_trim_create(&out_trim,
                          cfg.audio.output_leading_trim_ms / 20) != 0) goto fail;

    /* ── init: telemetry (needed by capture callback and playback thread) ── */
    if (telemetry_create(&tel, &cfg) != 0) goto fail;

    /* ── init: ALSA (capture callback drives the input path) ── */
    tx_ctx_t tx_ctx = {0};
    atomic_init(&tx_ctx.tx_seq, 0);
    tx_ctx.cfg               = &cfg;
    tx_ctx.transport         = transport;
    tx_ctx.in_proc           = in_proc;
    tx_ctx.in_trim           = in_trim;
    tx_ctx.logic             = logic;
    tx_ctx.tel               = tel;
    tx_ctx.floor             = &g_floor;
    tx_ctx.floor_held        = false;
    tx_ctx.send              = loopback ? send_loopback : send_network;
    tx_ctx.loopback_cb       = on_usrp_packet;
    tx_ctx.loopback_userdata = &rx_ctx;
    tx_ctx.opus_enc          = opus;
    tx_ctx.opus_accum_n      = 0;

    if (audio_alsa_create(&alsa, &cfg, on_capture_frame, &tx_ctx) != 0)
        goto fail;
    tx_ctx.alsa = alsa;
    watchdog_set_alsa(wd, alsa);

    /* ── init: playback thread ── */
    pb_ctx_t pb_ctx = {0};
    pb_ctx.alsa     = alsa;
    pb_ctx.out_proc = out_proc;
    pb_ctx.out_trim = out_trim;
    pb_ctx.jb       = jb;
    pb_ctx.logic    = logic;
    pb_ctx.tel      = tel;
    pb_ctx.stop     = &g_stop;

    if (pthread_create(&pb_thread, NULL, playback_thread_fn, &pb_ctx) != 0) {
        sd_journal_print(LOG_ERR, "main: playback pthread_create: %m");
        goto fail;
    }
    pb_started = true;

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
    opus_codec_destroy(opus);
    usrp_transport_destroy(transport);
    watchdog_destroy(wd);
    jitter_buffer_destroy(jb);
    logic_hid_destroy(logic);   /* releases output_active last — fail-safe */

    return 0;

fail:
    sd_journal_print(LOG_ERR, "rusrp: initialization failed");
    sd_notify(0, "STOPPING=1");
    atomic_store(&g_stop, true);
    if (pb_started)   pthread_join(pb_thread, NULL);
    audio_alsa_destroy(alsa);
    audio_trim_destroy(out_trim);
    audio_trim_destroy(in_trim);
    audio_proc_destroy(out_proc);
    audio_proc_destroy(in_proc);
    opus_codec_destroy(opus);
    usrp_transport_destroy(transport);
    watchdog_destroy(wd);
    jitter_buffer_destroy(jb);
    logic_hid_destroy(logic);
    return 1;
}
