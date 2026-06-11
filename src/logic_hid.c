#include "logic_hid.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <systemd/sd-journal.h>

/* CM119A GPIO bit for output_active.
 * HID_OR1 bit N controls GPIO(N+1); GPIO3 = bit 2 = 0x04. */
#define GPIO_BIT(n)  (1u << ((n) - 1))

/* HID_IR0 bit 1 = VOLDN (input_active source). */
#define VOLDN_BIT    0x02u

struct logic_hid {
    int          fd;
    unsigned int gpio_bit;       /* bitmask for output_active GPIO in HID_OR1 */
    bool         input_active_low;
    bool         output_active_low;
    atomic_bool  input_active;
    atomic_bool  output_desired;
    pthread_t    poll_thread;
    atomic_bool  stop;
};

static int hid_write_gpio(logic_hid_t *l, bool active)
{
    bool pin_high = (active == l->output_active_low);
    /* hidraw requires report ID as byte 0 even for devices without numbered
     * reports; data begins at byte 1. Total 5 bytes for the 4-byte CM119A report. */
    uint8_t report[5] = {
        0x00,                                       /* report ID (always 0) */
        0x00,                                       /* HID_OR0: GPIO mode */
        pin_high ? (uint8_t)l->gpio_bit : 0x00,    /* HID_OR1: output data */
        (uint8_t)l->gpio_bit,                       /* HID_OR2: direction=output */
        0x00,                                       /* HID_OR3 */
    };
    ssize_t n = write(l->fd, report, sizeof(report));
    if (n != (ssize_t)sizeof(report)) {
        sd_journal_print(LOG_ERR, "logic: HID write failed (active=%d): %m", (int)active);
        return -1;
    }
    return 0;
}

static void *poll_thread_fn(void *arg)
{
    logic_hid_t *l = arg;
    uint8_t buf[4];

    while (!atomic_load_explicit(&l->stop, memory_order_relaxed)) {
        struct pollfd pfd = { .fd = l->fd, .events = POLLIN };
        if (poll(&pfd, 1, 200) <= 0)
            continue;
        if (!(pfd.revents & POLLIN))
            continue;

        ssize_t n = read(l->fd, buf, sizeof(buf));
        if (n < 1) continue;

        bool raw = (buf[0] & VOLDN_BIT) != 0;
        bool active = (raw == l->input_active_low);
        atomic_store_explicit(&l->input_active, active, memory_order_relaxed);
    }
    return NULL;
}

int logic_hid_create(logic_hid_t **out, const config_t *cfg)
{
    logic_hid_t *l = calloc(1, sizeof(*l));
    if (!l) return -1;

    l->gpio_bit          = GPIO_BIT(cfg->logic.output_active_gpio);
    l->input_active_low  = cfg->logic.input_active_low;
    l->output_active_low = cfg->logic.output_active_low;
    atomic_init(&l->input_active,  false);
    atomic_init(&l->output_desired, false);
    atomic_init(&l->stop,          false);

    l->fd = open(cfg->logic.hid_device, O_RDWR);
    if (l->fd < 0) {
        sd_journal_print(LOG_ERR, "logic: open %s: %m", cfg->logic.hid_device);
        free(l);
        return -1;
    }

    /* Ensure output_active starts deasserted (fail-safe). */
    hid_write_gpio(l, false);

    if (pthread_create(&l->poll_thread, NULL, poll_thread_fn, l) != 0) {
        sd_journal_print(LOG_ERR, "logic: pthread_create: %m");
        close(l->fd);
        free(l);
        return -1;
    }

    sd_journal_print(LOG_INFO, "logic: opened %s (gpio%d, out_low=%d)",
                     cfg->logic.hid_device,
                     cfg->logic.output_active_gpio,
                     cfg->logic.output_active_low);

    *out = l;
    return 0;
}

bool logic_hid_input_active(const logic_hid_t *l)
{
    return atomic_load_explicit(&l->input_active, memory_order_relaxed);
}

int logic_hid_set_output(logic_hid_t *l, bool active)
{
    bool prev = atomic_exchange_explicit(&l->output_desired, active, memory_order_relaxed);
    if (active == prev)
        return 0;
    return hid_write_gpio(l, active);
}

bool logic_hid_output_active(const logic_hid_t *l)
{
    return atomic_load_explicit(&l->output_desired, memory_order_relaxed);
}

void logic_hid_destroy(logic_hid_t *l)
{
    if (!l) return;
    atomic_store(&l->stop, true);
    pthread_join(l->poll_thread, NULL);
    hid_write_gpio(l, false);   /* fail-safe: deassert before closing */
    close(l->fd);
    free(l);
}
