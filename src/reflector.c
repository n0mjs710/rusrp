/*
 * usrp-reflector — simple multi-client USRP conference bridge
 *
 * Clients connect by sending packets; they are remembered for CLIENT_TTL_SEC
 * seconds after the last received packet.  When a client keys up, its traffic
 * is forwarded to all other known clients until it unkeys.  Half-duplex floor:
 * the first client to key up holds the floor; latecomers are ignored until the
 * floor is released.  Works transparently with both PCM and Opus packet types
 * since it forwards raw bytes without inspecting the audio payload.
 *
 * Usage: usrp-reflector [-b bind_addr] [-p port]
 *   Default: bind 0.0.0.0, port 34001
 */

#include "usrp_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_CLIENTS    16
#define CLIENT_TTL_SEC 30
#define DEFAULT_PORT   34001
#define MAX_PKT        (USRP_PKT_LEN + 64)  /* generous headroom for Opus or unknown types */

/* ── types ───────────────────────────────────────────────────────────────── */

typedef struct {
    struct sockaddr_in addr;
    time_t             last_seen;
    bool               active;
} client_t;

/* ── globals ─────────────────────────────────────────────────────────────── */

static volatile bool g_stop = false;

static void sig_handler(int sig) { (void)sig; g_stop = true; }

/* ── client table ────────────────────────────────────────────────────────── */

static client_t clients[MAX_CLIENTS];

static int client_find_or_add(const struct sockaddr_in *addr, time_t now)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr
            && clients[i].addr.sin_port == addr->sin_port) {
            clients[i].last_seen = now;
            return i;
        }
    }
    if (free_slot < 0) {
        fprintf(stderr, "reflector: client table full; dropping new client\n");
        return -1;
    }
    clients[free_slot].addr      = *addr;
    clients[free_slot].last_seen = now;
    clients[free_slot].active    = true;
    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
    printf("reflector: new client %s:%u (slot %d)\n",
           ipstr, ntohs(addr->sin_port), free_slot);
    return free_slot;
}

static void client_expire(time_t now)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) continue;
        if (now - clients[i].last_seen > CLIENT_TTL_SEC) {
            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clients[i].addr.sin_addr, ipstr, sizeof(ipstr));
            printf("reflector: client %s:%u expired (slot %d)\n",
                   ipstr, ntohs(clients[i].addr.sin_port), i);
            clients[i].active = false;
        }
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char  *bind_addr = "0.0.0.0";
    uint16_t     port      = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0)
            && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--bind") == 0)
                   && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-b bind_addr] [-p port]\n", argv[0]);
            printf("  Default: 0.0.0.0:%u\n", DEFAULT_PORT);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, bind_addr, &local.sin_addr) != 1) {
        fprintf(stderr, "reflector: invalid bind address: %s\n", bind_addr);
        return 1;
    }
    if (bind(sockfd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind"); return 1;
    }

    /* 200 ms receive timeout so we can check g_stop and expire idle clients. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("reflector: listening on %s:%u (max %d clients, %ds TTL)\n",
           bind_addr, port, MAX_CLIENTS, CLIENT_TTL_SEC);

    /* floor_holder: index into clients[], or -1 when idle */
    int    floor_holder = -1;
    time_t last_expire  = time(NULL);
    uint8_t buf[MAX_PKT];

    while (!g_stop) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &src_len);

        time_t now = time(NULL);

        /* Periodic client expiry (every ~5 s is fine; we just check cheaply). */
        if (now - last_expire >= 5) {
            /* If the floor holder expired, release the floor. */
            if (floor_holder >= 0 && !clients[floor_holder].active)
                floor_holder = -1;
            client_expire(now);
            last_expire = now;
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            perror("recvfrom");
            break;
        }

        if (n < (ssize_t)USRP_HEADER_LEN)
            continue;

        /* Parse just enough to get keyup — don't need audio payload. */
        usrp_packet_t pkt;
        if (usrp_parse(buf, (size_t)n, &pkt) != 0)
            continue;

        int sender = client_find_or_add(&src, now);
        if (sender < 0)
            continue;

        bool keyed = (pkt.keyup != 0);

        /* Half-duplex floor arbitration. */
        if (keyed) {
            if (floor_holder < 0) {
                floor_holder = sender;
            } else if (floor_holder != sender) {
                /* Another client holds the floor; drop this packet. */
                continue;
            }
        } else {
            /* Unkey — release floor if this sender held it. */
            if (floor_holder == sender)
                floor_holder = -1;
        }

        /* Forward raw bytes to all other active clients. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active || i == sender)
                continue;
            sendto(sockfd, buf, (size_t)n, 0,
                   (struct sockaddr *)&clients[i].addr,
                   sizeof(clients[i].addr));
        }
    }

    close(sockfd);
    printf("reflector: stopped\n");
    return 0;
}
