/*
 * usrp-reflector — USRP conference bridge for a fixed set of repeaters
 *
 * A drop-in for an ASL node that only links USRP channels together. Each
 * repeater is configured the way an ASL rxchannel is: the port the reflector
 * listens on for it, and the address it sends that repeater's audio to.
 * Audio to a repeater always goes out from that repeater's listen port, so a
 * peer with a connected socket (usrp-rc) accepts it.
 *
 * Half-duplex floor: the first repeater to key holds it until it unkeys or
 * goes quiet for floor_timeout_ms; everyone else's traffic is dropped
 * meanwhile. Payloads are forwarded untouched, so PCM and Opus both work.
 *
 * Usage: usrp-reflector [-c config] [-v]
 */

#include "conf.h"
#include "usrp_protocol.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define DEFAULT_CONFIG "/etc/rusrp/usrp-reflector.conf"
#define MAX_REPEATERS  32
#define MAX_PKT        (USRP_PKT_LEN + 64)
#define NAME_LEN       32
#define UNKNOWN_LOG_MS 30000

typedef struct {
    char               name[NAME_LEN];
    uint16_t           listen_port;
    struct sockaddr_in addr;
    int                sock;       /* index into socks[] */
    uint64_t           last_rx_ms;
} repeater_t;

typedef struct {
    int      fd;
    uint16_t port;
    int      n_repeaters;
} lsock_t;

static const char *const LEVELS[] = { "info", "debug", NULL };

typedef struct {
    char     bind_address[64];
    unsigned floor_timeout_ms;
    int      log_level;            /* 0 info, 1 debug */
} refl_cfg_t;

static repeater_t rpt[MAX_REPEATERS];
static int        n_rpt;
static lsock_t    socks[MAX_REPEATERS];
static int        n_socks;
static refl_cfg_t cfg;

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool g_tty;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...)
{
    if (g_tty) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        printf("%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    }
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

#define DEBUG(...) do { if (cfg.log_level) logmsg(__VA_ARGS__); } while (0)

static const char *addr_str(const struct sockaddr_in *a)
{
    static char buf[2][INET_ADDRSTRLEN + 8];
    static int which;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    which ^= 1;
    snprintf(buf[which], sizeof(buf[which]), "%s:%u", ip, ntohs(a->sin_port));
    return buf[which];
}

/* ── config ──────────────────────────────────────────────────────────────── */

#define F(key, type, member) \
    { "reflector", #key, type, offsetof(refl_cfg_t, member), sizeof(((refl_cfg_t *)0)->member), NULL }

static const conf_field_t FIELDS[] = {
    F(bind_address,     CONF_STR,  bind_address),
    F(floor_timeout_ms, CONF_UINT, floor_timeout_ms),
    { "reflector", "log_level", CONF_CHOICE, offsetof(refl_cfg_t, log_level), 0, LEVELS },
    { 0 }
};

static bool resolve(const char *host, const char *port, struct sockaddr_in *out)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return false;
    memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);
    return true;
}

/* name = listen_port host:port */
static void read_repeater(conf_t *c, const conf_entry_t *e)
{
    if (n_rpt >= MAX_REPEATERS) {
        conf_err(c, e->line, "more than %d repeaters", MAX_REPEATERS);
        return;
    }
    if (strlen(e->key) >= NAME_LEN) {
        conf_err(c, e->line, "%s: name longer than %d characters", e->key, NAME_LEN - 1);
        return;
    }
    char lport[16], host[256], *colon;
    long long l;
    if (sscanf(e->value, "%15s %255s", lport, host) != 2
        || !conf_parse_ll(lport, &l) || l < 1 || l > 65535
        || !(colon = strrchr(host, ':'))) {
        conf_err(c, e->line, "%s: expected 'listen_port host:port', got '%s'", e->key, e->value);
        return;
    }
    *colon = '\0';
    repeater_t *r = &rpt[n_rpt];
    long long p;
    if (!conf_parse_ll(colon + 1, &p) || p < 1 || p > 65535) {
        conf_err(c, e->line, "%s: '%s' is not a port (1-65535)", e->key, colon + 1);
        return;
    }
    if (!resolve(host, colon + 1, &r->addr)) {
        conf_err(c, e->line, "%s: cannot resolve '%s'", e->key, host);
        return;
    }
    for (int i = 0; i < n_rpt; i++) {
        if (rpt[i].listen_port == l && rpt[i].addr.sin_addr.s_addr == r->addr.sin_addr.s_addr
            && rpt[i].addr.sin_port == r->addr.sin_port) {
            conf_err(c, e->line, "%s: same port and address as %s", e->key, rpt[i].name);
            return;
        }
    }
    strcpy(r->name, e->key);
    r->listen_port = (uint16_t)l;
    n_rpt++;
}

static int load_config(const char *path)
{
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.bind_address, "0.0.0.0");
    cfg.floor_timeout_ms = 1000;

    conf_t c;
    if (conf_read(&c, path) != 0) {
        fprintf(stderr, "usrp-reflector: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    static const char *const OPEN[] = { "repeaters", NULL };
    conf_apply(&c, FIELDS, &cfg, OPEN);
    for (int i = 0; i < c.n; i++)
        if (strcmp(c.v[i].section, "repeaters") == 0)
            read_repeater(&c, &c.v[i]);
    if (!c.errors && n_rpt < 2)
        conf_err(&c, 0, "[repeaters] needs at least two entries");
    int errors = c.errors;
    conf_free(&c);
    return errors ? -1 : 0;
}

/* ── sockets ─────────────────────────────────────────────────────────────── */

static int open_sockets(void)
{
    struct in_addr bind_ip;
    if (inet_pton(AF_INET, cfg.bind_address, &bind_ip) != 1) {
        fprintf(stderr, "usrp-reflector: invalid bind_address: %s\n", cfg.bind_address);
        return -1;
    }
    for (int i = 0; i < n_rpt; i++) {
        int s = 0;
        while (s < n_socks && socks[s].port != rpt[i].listen_port)
            s++;
        if (s == n_socks) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in local = { .sin_family = AF_INET, .sin_addr = bind_ip,
                                         .sin_port = htons(rpt[i].listen_port) };
            if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
                fprintf(stderr, "usrp-reflector: bind %s:%u: %s\n",
                        cfg.bind_address, rpt[i].listen_port, strerror(errno));
                return -1;
            }
            socks[s].fd = fd;
            socks[s].port = rpt[i].listen_port;
            n_socks++;
        }
        rpt[i].sock = s;
        socks[s].n_repeaters++;
    }
    return 0;
}

/* Which repeater sent this? Sole owner of the port, or exact source match. */
static int identify(int s, const struct sockaddr_in *src)
{
    int only = -1;
    for (int i = 0; i < n_rpt; i++) {
        if (rpt[i].sock != s)
            continue;
        if (rpt[i].addr.sin_addr.s_addr == src->sin_addr.s_addr
            && rpt[i].addr.sin_port == src->sin_port)
            return i;
        only = i;
    }
    return socks[s].n_repeaters == 1 ? only : -1;
}

static void send_to(int i, const uint8_t *buf, size_t n)
{
    if (sendto(socks[rpt[i].sock].fd, buf, n, 0,
               (const struct sockaddr *)&rpt[i].addr, sizeof(rpt[i].addr)) < 0)
        DEBUG("%s: send failed: %s", rpt[i].name, strerror(errno));
}

static void forward(int from, const uint8_t *buf, size_t n)
{
    for (int i = 0; i < n_rpt; i++)
        if (i != from)
            send_to(i, buf, n);
}

/* ── floor ───────────────────────────────────────────────────────────────── */

static int      holder = -1;
static uint64_t floor_start_ms;
static unsigned floor_pkts;
static uint32_t holder_seq;

static void floor_release(const char *why)
{
    logmsg("UNKEY %s (%s, %.1f s, %u packets)", rpt[holder].name, why,
           (double)(now_ms() - floor_start_ms) / 1000.0, floor_pkts);
    holder = -1;
}

static void handle_packet(int s, const uint8_t *buf, size_t n, const struct sockaddr_in *src)
{
    static struct sockaddr_in last_unknown;
    static uint64_t last_unknown_ms;

    uint64_t now = now_ms();
    int i = identify(s, src);
    if (i < 0) {
        if (memcmp(&last_unknown, src, sizeof(*src)) != 0 || now - last_unknown_ms > UNKNOWN_LOG_MS) {
            logmsg("ignoring packets on port %u from unconfigured %s", socks[s].port, addr_str(src));
            last_unknown = *src;
            last_unknown_ms = now;
        }
        return;
    }
    repeater_t *r = &rpt[i];
    if (memcmp(&r->addr.sin_addr, &src->sin_addr, sizeof(src->sin_addr)) != 0
        || r->addr.sin_port != src->sin_port)
        DEBUG("%s: packet from %s, configured %s", r->name, addr_str(src), addr_str(&r->addr));

    usrp_packet_t pkt;
    if (usrp_parse(buf, n, &pkt) != 0) {
        DEBUG("%s: not a USRP packet (%zu bytes)", r->name, n);
        return;
    }
    r->last_rx_ms = now;
    bool keyed = pkt.keyup != 0;

    if (holder < 0 && keyed) {
        holder = i;
        floor_start_ms = now;
        floor_pkts = 0;
        logmsg("KEY %s", r->name);
    }
    if (holder != i) {
        if (keyed && holder >= 0)
            DEBUG("%s: keyed while %s holds the floor, dropped", r->name, rpt[holder].name);
        else
            DEBUG("%s: type %d unkeyed packet while %s, dropped", r->name, (int)pkt.type,
                  holder >= 0 ? "floor held" : "idle");
        return;
    }

    floor_pkts++;
    holder_seq = pkt.seq;
    forward(i, buf, n);
    if (!keyed)
        floor_release("unkey");
}

static void check_floor_timeout(void)
{
    if (holder < 0 || now_ms() - rpt[holder].last_rx_ms < cfg.floor_timeout_ms)
        return;
    uint8_t buf[USRP_PKT_LEN];
    usrp_build_key(buf, holder_seq + 1, 0);
    forward(holder, buf, sizeof(buf));
    floor_release("timeout");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *path = DEFAULT_CONFIG;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-c config] [-v]\n  default config: %s\n", argv[0], DEFAULT_CONFIG);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    g_tty = isatty(STDOUT_FILENO);

    if (load_config(path) != 0)
        return 1;
    if (verbose)
        cfg.log_level = 1;
    if (open_sockets() != 0)
        return 1;

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    logmsg("usrp-reflector: %d repeaters, floor timeout %u ms", n_rpt, cfg.floor_timeout_ms);
    for (int i = 0; i < n_rpt; i++)
        logmsg("  %-12s listen %s:%u -> %s", rpt[i].name, cfg.bind_address,
               rpt[i].listen_port, addr_str(&rpt[i].addr));

    struct pollfd pfd[MAX_REPEATERS];
    for (int s = 0; s < n_socks; s++)
        pfd[s] = (struct pollfd){ .fd = socks[s].fd, .events = POLLIN };

    while (!g_stop) {
        int rc = poll(pfd, (nfds_t)n_socks, 100);
        if (rc < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        for (int s = 0; rc > 0 && s < n_socks; s++) {
            if (!(pfd[s].revents & POLLIN))
                continue;
            for (;;) {
                uint8_t buf[MAX_PKT];
                struct sockaddr_in src;
                socklen_t len = sizeof(src);
                ssize_t n = recvfrom(socks[s].fd, buf, sizeof(buf), MSG_DONTWAIT,
                                     (struct sockaddr *)&src, &len);
                if (n < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        DEBUG("port %u: recv: %s", socks[s].port, strerror(errno));
                    break;
                }
                handle_packet(s, buf, (size_t)n, &src);
            }
        }
        check_floor_timeout();
    }

    logmsg("usrp-reflector: stopped");
    return 0;
}
