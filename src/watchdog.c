#include "watchdog.h"
#include "util.h"

#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <systemd/sd-journal.h>
#include <systemd/sd-daemon.h>

struct watchdog {
    const config_t  *cfg;
    logic_hid_t     *logic;
    jitter_buffer_t *jb;
    audio_alsa_t    *alsa;   /* set via watchdog_set_alsa after ALSA init */
    pthread_t        thread;
    atomic_bool      stop;
    atomic_bool      keyed;
    atomic_uint_least64_t last_packet_ts;
    atomic_uint_least64_t key_end_ts;    /* monotonic_ms when unkey was detected */
    atomic_uint_least64_t ptt_ready_ts;  /* monotonic_ms when to fire PTT (0=none) */
    atomic_uint_least64_t event_count;
    uint64_t         startup_end_ts;
};

static void force_unkey(watchdog_t *wd, const char *reason)
{
    bool was_keyed = atomic_exchange(&wd->keyed, false);
    atomic_store(&wd->ptt_ready_ts, 0);   /* cancel any pending deferred PTT */
    if (was_keyed) {
        sd_journal_print(LOG_WARNING, "watchdog: forcing unkey: %s", reason);
        logic_hid_set_output(wd->logic, false);
        jitter_buffer_flush(wd->jb);
        atomic_fetch_add(&wd->event_count, 1);
    }
}

static void *watchdog_thread_fn(void *arg)
{
    watchdog_t *wd = arg;

    while (!atomic_load_explicit(&wd->stop, memory_order_relaxed)) {
        usleep(10000);  /* 10ms tick */

        uint64_t now = monotonic_ms();

        /* Kick systemd watchdog if configured. */
        sd_notify(0, "WATCHDOG=1");

        /* Startup inhibit: suppress output_active during boot window. */
        if (now < wd->startup_end_ts) {
            logic_hid_set_output(wd->logic, false);
            continue;
        }

        /* Deferred PTT: assert after jitter_buffer_ms so audio is buffered
         * and ready to play the instant the radio keys up. */
        uint64_t ptt_ready = atomic_load_explicit(&wd->ptt_ready_ts,
                                                   memory_order_relaxed);
        if (ptt_ready > 0 && now >= ptt_ready) {
            atomic_store(&wd->ptt_ready_ts, 0);
            logic_hid_set_output(wd->logic, true);
            if (wd->cfg->logging.level <= LOG_LEVEL_DEBUG)
                sd_journal_print(LOG_DEBUG, "watchdog: PTT asserted");
        }

        /* Network timeout: no USRP traffic in network_timeout_ms. */
        uint64_t last = atomic_load_explicit(&wd->last_packet_ts,
                                             memory_order_relaxed);
        if (last > 0 && (now - last) > wd->cfg->watchdog.network_timeout_ms) {
            force_unkey(wd, "network timeout");
            atomic_store(&wd->last_packet_ts, 0);
            continue;
        }

        /* output_active tail timer: hold output after UNKEY. */
        if (atomic_load_explicit(&wd->keyed, memory_order_relaxed))
            continue;

        uint64_t key_end = atomic_load_explicit(&wd->key_end_ts,
                                                memory_order_relaxed);
        if (key_end > 0) {
            if ((now - key_end) >= wd->cfg->watchdog.output_active_tail_ms) {
                logic_hid_set_output(wd->logic, false);
                atomic_store(&wd->key_end_ts, 0);
            }
        }
    }

    /* Final cleanup on exit. */
    force_unkey(wd, "daemon stopping");
    return NULL;
}

int watchdog_create(watchdog_t **wd, const config_t *cfg,
                    logic_hid_t *logic, jitter_buffer_t *jb)
{
    watchdog_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;

    self->cfg   = cfg;
    self->logic = logic;
    self->jb    = jb;
    self->startup_end_ts = monotonic_ms() + cfg->watchdog.startup_output_inhibit_ms;

    atomic_init(&self->stop,           false);
    atomic_init(&self->keyed,          false);
    atomic_init(&self->last_packet_ts, 0);
    atomic_init(&self->key_end_ts,     0);
    atomic_init(&self->ptt_ready_ts,   0);
    atomic_init(&self->event_count,    0);

    if (pthread_create(&self->thread, NULL, watchdog_thread_fn, self) != 0) {
        free(self);
        return -1;
    }

    *wd = self;
    return 0;
}

void watchdog_set_alsa(watchdog_t *wd, audio_alsa_t *alsa)
{
    wd->alsa = alsa;
}

void watchdog_packet_received(watchdog_t *wd)
{
    atomic_store_explicit(&wd->last_packet_ts, monotonic_ms(),
                          memory_order_relaxed);
}

void watchdog_key_event(watchdog_t *wd, bool keyed)
{
    bool prev = atomic_exchange(&wd->keyed, keyed);
    if (keyed && !prev) {
        /* KEY: flush jitter buffer so it re-seeds to the new transmission's
         * sequence numbers (remote resets seq to 0 each transmission).
         * Defer PTT by jitter_buffer_ms so the buffer is full and audio
         * plays the moment the radio keys up. */
        jitter_buffer_flush(wd->jb);
        atomic_store(&wd->key_end_ts, 0);
        uint64_t fire = monotonic_ms() + wd->cfg->network.jitter_buffer_ms;
        atomic_store(&wd->ptt_ready_ts, fire);
        if (wd->cfg->logging.level <= LOG_LEVEL_DEBUG)
            sd_journal_print(LOG_DEBUG, "watchdog: keyed, PTT in %u ms",
                             wd->cfg->network.jitter_buffer_ms);
    } else if (!keyed && prev) {
        /* UNKEY: cancel pending PTT, flush buffers to prevent stale audio
         * bleeding into the next transmission, then start tail timer. */
        atomic_store(&wd->ptt_ready_ts, 0);
        jitter_buffer_flush(wd->jb);
        if (wd->alsa)
            audio_alsa_request_drain(wd->alsa);
        atomic_store(&wd->key_end_ts, monotonic_ms());
        if (wd->cfg->logging.level <= LOG_LEVEL_DEBUG)
            sd_journal_print(LOG_DEBUG, "watchdog: unkeyed, starting tail timer");
    }
}

uint64_t watchdog_event_count(const watchdog_t *wd)
{
    return atomic_load(&wd->event_count);
}

void watchdog_destroy(watchdog_t *wd)
{
    if (!wd) return;
    atomic_store(&wd->stop, true);
    pthread_join(wd->thread, NULL);
    free(wd);
}
