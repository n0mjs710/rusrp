#include "usrp_transport.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <systemd/sd-journal.h>

struct usrp_transport {
    int              sockfd;
    usrp_rx_cb_t     rx_cb;
    void            *userdata;
    pthread_t        rx_thread;
    atomic_bool      stop;
};

static void *rx_thread_fn(void *arg)
{
    usrp_transport_t *t = arg;
    uint8_t buf[USRP_PKT_LEN + 64];
    usrp_packet_t pkt;

    while (!atomic_load_explicit(&t->stop, memory_order_relaxed)) {
        ssize_t n = recv(t->sockfd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (!atomic_load_explicit(&t->stop, memory_order_relaxed))
                sd_journal_print(LOG_ERR, "usrp: recv error: %m");
            break;
        }
        if (usrp_parse(buf, (size_t)n, &pkt) == 0)
            t->rx_cb(&pkt, t->userdata);
    }
    return NULL;
}

int usrp_transport_create(usrp_transport_t **out, const config_t *cfg,
                          usrp_rx_cb_t rx_cb, void *userdata)
{
    usrp_transport_t *t = calloc(1, sizeof(*t));
    if (!t)
        return -1;

    t->rx_cb    = rx_cb;
    t->userdata = userdata;
    atomic_init(&t->stop, false);

    t->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (t->sockfd < 0) {
        sd_journal_print(LOG_ERR, "usrp: socket: %m");
        free(t);
        return -1;
    }

    /* Receive timeout so the RX thread wakes to check stop flag. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(t->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_port        = htons(cfg->usrp.local_port),
        .sin_addr.s_addr = inet_addr(cfg->usrp.bind_address),
    };
    if (bind(t->sockfd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        sd_journal_print(LOG_ERR, "usrp: bind :%u: %m", cfg->usrp.local_port);
        close(t->sockfd);
        free(t);
        return -1;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", cfg->usrp.remote_port);
    if (getaddrinfo(cfg->usrp.remote_host, port_str, &hints, &res) != 0) {
        sd_journal_print(LOG_ERR, "usrp: cannot resolve %s", cfg->usrp.remote_host);
        close(t->sockfd);
        free(t);
        return -1;
    }
    int rc = connect(t->sockfd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0) {
        sd_journal_print(LOG_ERR, "usrp: connect %s:%u: %m",
                         cfg->usrp.remote_host, cfg->usrp.remote_port);
        close(t->sockfd);
        free(t);
        return -1;
    }

    if (pthread_create(&t->rx_thread, NULL, rx_thread_fn, t) != 0) {
        sd_journal_print(LOG_ERR, "usrp: pthread_create: %m");
        close(t->sockfd);
        free(t);
        return -1;
    }

    sd_journal_print(LOG_INFO, "usrp: listening on :%u → %s:%u",
                     cfg->usrp.local_port,
                     cfg->usrp.remote_host,
                     cfg->usrp.remote_port);

    *out = t;
    return 0;
}

int usrp_transport_send(usrp_transport_t *t, const uint8_t *buf, size_t len)
{
    ssize_t n = send(t->sockfd, buf, len, 0);
    return (n == (ssize_t)len) ? 0 : -1;
}

void usrp_transport_destroy(usrp_transport_t *t)
{
    if (!t)
        return;
    atomic_store(&t->stop, true);
    shutdown(t->sockfd, SHUT_RDWR);
    pthread_join(t->rx_thread, NULL);
    close(t->sockfd);
    free(t);
}
