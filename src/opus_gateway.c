/*
 * opus-gateway — PCM FM (MMDVMHost) ↔ Opus USRP (network) bridge.
 *
 * MMDVMHost's FM mode sends audio to a gateway process using the FM protocol:
 *   FMS<callsign>\0   transmission start (sent automatically before first FMD)
 *   FMD<S16LE bytes>  audio data (variable number of samples per packet)
 *   FME               transmission end
 *   FMP               keepalive ping (ignored)
 *
 * OpusGateway listens for those packets on the loopback, Opus-encodes the
 * audio, and forwards it as USRP type-7 packets to a rusrp peer or reflector.
 * Incoming Opus (or PCM USRP) packets are decoded and sent back as FMD
 * packets so MMDVMHost can re-transmit them on RF.
 *
 * PCM USRP peers (type-0) are also accepted on the receive path for
 * compatibility with nodes that do not use Opus.
 *
 * Usage: opus-gateway [-c config]   default: /etc/rusrp/opus-gateway.toml
 */

#include "usrp_protocol.h"
#include "opus_codec.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

/* ── constants ───────────────────────────────────────────────────────────── */

#define DEFAULT_CONFIG   "/etc/rusrp/opus-gateway.toml"
#define FM_MAX_PKT       1500
#define TX_ACCUM_MAX     480     /* max Opus frame: 60 ms at 8 kHz */
#define NET_MAX_PKT      (USRP_HEADER_LEN + USRP_OPUS_MAX_BYTES + 4)

/* ── config ──────────────────────────────────────────────────────────────── */

typedef struct {
    char     local_address[64];
    uint16_t local_port;      /* OpusGateway binds here to receive from MMDVM */
    char     rpt_address[64]; /* MMDVMHost address — where to send FMD back   */
    uint16_t rpt_port;
} mmdvm_cfg_t;

typedef struct {
    char     remote_host[256];
    uint16_t remote_port;
    uint16_t local_port;
    char     bind_address[64];
} net_cfg_t;

typedef struct {
    int opus_bitrate;
    int opus_frame_ms;
} audio_cfg_t;

typedef struct {
    mmdvm_cfg_t mmdvm;
    net_cfg_t   net;
    audio_cfg_t audio;
} gw_config_t;

/* ── globals ─────────────────────────────────────────────────────────────── */

static volatile bool g_stop = false;
static void sig_handler(int sig) { (void)sig; g_stop = true; }

/* ── TOML helpers (same pattern as config.c) ─────────────────────────────── */

static void rd_str(toml_table_t *t, const char *k, char *dst, size_t n)
{
    toml_datum_t d = toml_string_in(t, k);
    if (d.ok) { strncpy(dst, d.u.s, n - 1); dst[n - 1] = '\0'; free(d.u.s); }
}

static void rd_u16(toml_table_t *t, const char *k, uint16_t *dst)
{
    toml_datum_t d = toml_int_in(t, k);
    if (d.ok && d.u.i > 0 && d.u.i <= 65535) *dst = (uint16_t)d.u.i;
}

static void rd_int(toml_table_t *t, const char *k, int *dst)
{
    toml_datum_t d = toml_int_in(t, k);
    if (d.ok) *dst = (int)d.u.i;
}

static int gw_config_load(gw_config_t *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));

    /* defaults */
    strncpy(cfg->mmdvm.local_address, "127.0.0.1", sizeof(cfg->mmdvm.local_address) - 1);
    cfg->mmdvm.local_port = 34001;
    strncpy(cfg->mmdvm.rpt_address,   "127.0.0.1", sizeof(cfg->mmdvm.rpt_address) - 1);
    cfg->mmdvm.rpt_port   = 32001;
    strncpy(cfg->net.bind_address,    "0.0.0.0",   sizeof(cfg->net.bind_address) - 1);
    strncpy(cfg->net.remote_host,     "127.0.0.1", sizeof(cfg->net.remote_host) - 1);
    cfg->net.remote_port    = 34002;
    cfg->net.local_port     = 34003;
    cfg->audio.opus_bitrate  = 8000;
    cfg->audio.opus_frame_ms = 60;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "opus-gateway: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);
    if (!root) {
        fprintf(stderr, "opus-gateway: config parse error: %s\n", errbuf);
        return -1;
    }

    toml_table_t *t;
    if ((t = toml_table_in(root, "mmdvm"))) {
        rd_str(t, "local_address", cfg->mmdvm.local_address, sizeof(cfg->mmdvm.local_address));
        rd_u16(t, "local_port",    &cfg->mmdvm.local_port);
        rd_str(t, "rpt_address",   cfg->mmdvm.rpt_address,   sizeof(cfg->mmdvm.rpt_address));
        rd_u16(t, "rpt_port",      &cfg->mmdvm.rpt_port);
    }
    if ((t = toml_table_in(root, "network"))) {
        rd_str(t, "remote_host",  cfg->net.remote_host,  sizeof(cfg->net.remote_host));
        rd_u16(t, "remote_port",  &cfg->net.remote_port);
        rd_u16(t, "local_port",   &cfg->net.local_port);
        rd_str(t, "bind_address", cfg->net.bind_address, sizeof(cfg->net.bind_address));
    }
    if ((t = toml_table_in(root, "audio"))) {
        rd_int(t, "opus_bitrate",  &cfg->audio.opus_bitrate);
        rd_int(t, "opus_frame_ms", &cfg->audio.opus_frame_ms);
    }

    toml_free(root);

    int ok = 1;
    if (cfg->mmdvm.local_port == 0 || cfg->mmdvm.rpt_port == 0 ||
        cfg->net.remote_port == 0  || cfg->net.local_port == 0) {
        fprintf(stderr, "opus-gateway: all ports must be 1–65535\n"); ok = 0;
    }
    if (cfg->net.remote_host[0] == '\0') {
        fprintf(stderr, "opus-gateway: network.remote_host is required\n"); ok = 0;
    }
    if (cfg->audio.opus_bitrate < 4000 || cfg->audio.opus_bitrate > 64000) {
        fprintf(stderr, "opus-gateway: audio.opus_bitrate must be 4000–64000 bps\n"); ok = 0;
    }
    if (cfg->audio.opus_frame_ms != 20 && cfg->audio.opus_frame_ms != 40 &&
        cfg->audio.opus_frame_ms != 60) {
        fprintf(stderr, "opus-gateway: audio.opus_frame_ms must be 20, 40, or 60\n"); ok = 0;
    }
    return ok ? 0 : -1;
}

/* ── socket helpers ──────────────────────────────────────────────────────── */

static int udp_bind(const char *addr, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "opus-gateway: bad bind address: %s\n", addr);
        close(fd); return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    return fd;
}

static int resolve_peer(const char *host, uint16_t port, struct sockaddr_in *out)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res;
    int r = getaddrinfo(host, portstr, &hints, &res);
    if (r != 0) {
        fprintf(stderr, "opus-gateway: getaddrinfo(%s): %s\n", host, gai_strerror(r));
        return -1;
    }
    *out = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    return 0;
}

/* ── FM helpers ──────────────────────────────────────────────────────────── */

static void fm_send_data(int sock, const struct sockaddr_in *dst,
                         const int16_t *samples, int n)
{
    uint8_t buf[3 + TX_ACCUM_MAX * sizeof(int16_t)];
    buf[0] = 'F'; buf[1] = 'M'; buf[2] = 'D';
    memcpy(buf + 3, samples, (size_t)n * sizeof(int16_t));
    sendto(sock, buf, 3 + (size_t)n * sizeof(int16_t), 0,
           (const struct sockaddr *)dst, sizeof(*dst));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *config_path = DEFAULT_CONFIG;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0)
            && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: opus-gateway [-c config]\n  Default: %s\n", DEFAULT_CONFIG);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    gw_config_t cfg;
    if (gw_config_load(&cfg, config_path) != 0)
        return 1;

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    opus_codec_t *codec;
    if (opus_codec_create(&codec, cfg.audio.opus_bitrate, cfg.audio.opus_frame_ms) != 0)
        return 1;

    int frame_samples = opus_codec_frame_samples(codec);
    int sub_frames    = cfg.audio.opus_frame_ms / 20;  /* seq step per Opus packet */

    int mmdvm_sock = udp_bind(cfg.mmdvm.local_address, cfg.mmdvm.local_port);
    int net_sock   = udp_bind(cfg.net.bind_address,    cfg.net.local_port);
    if (mmdvm_sock < 0 || net_sock < 0) {
        if (mmdvm_sock >= 0) close(mmdvm_sock);
        if (net_sock   >= 0) close(net_sock);
        opus_codec_destroy(codec);
        return 1;
    }

    struct sockaddr_in mmdvm_peer, net_peer;
    if (resolve_peer(cfg.mmdvm.rpt_address, cfg.mmdvm.rpt_port, &mmdvm_peer) != 0 ||
        resolve_peer(cfg.net.remote_host,   cfg.net.remote_port, &net_peer)   != 0) {
        close(mmdvm_sock); close(net_sock);
        opus_codec_destroy(codec);
        return 1;
    }

    fprintf(stderr,
            "opus-gateway: mmdvm listen=%s:%u rpt=%s:%u  net bind=%s:%u remote=%s:%u"
            "  opus %dms %dbps\n",
            cfg.mmdvm.local_address, cfg.mmdvm.local_port,
            cfg.mmdvm.rpt_address,   cfg.mmdvm.rpt_port,
            cfg.net.bind_address,    cfg.net.local_port,
            cfg.net.remote_host,     cfg.net.remote_port,
            cfg.audio.opus_frame_ms, cfg.audio.opus_bitrate);

    /* TX state: MMDVM → network */
    bool     tx_active  = false;
    uint32_t tx_seq     = 0;
    int16_t  tx_accum[TX_ACCUM_MAX];
    int      tx_accum_n = 0;

    uint8_t net_pkt[NET_MAX_PKT];
    uint8_t opus_buf[USRP_OPUS_MAX_BYTES];
    uint8_t fm_buf[FM_MAX_PKT];

    struct pollfd fds[2] = {
        { .fd = mmdvm_sock, .events = POLLIN },
        { .fd = net_sock,   .events = POLLIN },
    };

    while (!g_stop) {
        int r = poll(fds, 2, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }

        /* ── MMDVM → network ─────────────────────────────────────────── */
        if (fds[0].revents & POLLIN) {
            ssize_t n = recvfrom(mmdvm_sock, fm_buf, sizeof(fm_buf), 0, NULL, NULL);

            if (n >= 3 && fm_buf[0] == 'F' && fm_buf[1] == 'M') {
                char type = (char)fm_buf[2];

                if (type == 'D' && n > 3) {
                    if (!tx_active) {
                        usrp_build_key(net_pkt, tx_seq++, 1);
                        sendto(net_sock, net_pkt, USRP_PKT_LEN, 0,
                               (const struct sockaddr *)&net_peer, sizeof(net_peer));
                        tx_active  = true;
                        tx_accum_n = 0;
                    }

                    /* FMD payload: variable-length S16LE audio */
                    int new_n = (int)(n - 3) / 2;
                    const int16_t *src = (const int16_t *)(fm_buf + 3);
                    int off = 0;

                    while (new_n > 0) {
                        int space = frame_samples - tx_accum_n;
                        int take  = new_n < space ? new_n : space;
                        memcpy(tx_accum + tx_accum_n, src + off,
                               (size_t)take * sizeof(int16_t));
                        tx_accum_n += take;
                        off        += take;
                        new_n      -= take;

                        if (tx_accum_n >= frame_samples) {
                            int olen = opus_codec_encode(codec, tx_accum, opus_buf);
                            if (olen > 0) {
                                size_t plen = usrp_build_opus_voice(
                                    net_pkt, tx_seq, 1, opus_buf, (size_t)olen);
                                sendto(net_sock, net_pkt, plen, 0,
                                       (const struct sockaddr *)&net_peer, sizeof(net_peer));
                            }
                            tx_seq    += (uint32_t)sub_frames;
                            tx_accum_n = 0;
                        }
                    }

                } else if (type == 'E') {
                    if (tx_active) {
                        if (tx_accum_n > 0) {
                            /* Pad partial frame with silence and flush */
                            memset(tx_accum + tx_accum_n, 0,
                                   (size_t)(frame_samples - tx_accum_n) * sizeof(int16_t));
                            int olen = opus_codec_encode(codec, tx_accum, opus_buf);
                            if (olen > 0) {
                                size_t plen = usrp_build_opus_voice(
                                    net_pkt, tx_seq, 1, opus_buf, (size_t)olen);
                                sendto(net_sock, net_pkt, plen, 0,
                                       (const struct sockaddr *)&net_peer, sizeof(net_peer));
                            }
                            tx_seq    += (uint32_t)sub_frames;
                            tx_accum_n = 0;
                        }
                        usrp_build_key(net_pkt, tx_seq++, 0);
                        sendto(net_sock, net_pkt, USRP_PKT_LEN, 0,
                               (const struct sockaddr *)&net_peer, sizeof(net_peer));
                        tx_active = false;
                        tx_seq    = 0;
                    }
                }
                /* FMS (type 'S') and FMP (type 'P') are silently ignored */
            }
        }

        /* ── network → MMDVM ─────────────────────────────────────────── */
        if (fds[1].revents & POLLIN) {
            uint8_t net_buf[NET_MAX_PKT];
            ssize_t n = recvfrom(net_sock, net_buf, sizeof(net_buf), 0, NULL, NULL);
            if (n < (ssize_t)USRP_HEADER_LEN) continue;

            usrp_packet_t pkt;
            if (usrp_parse(net_buf, (size_t)n, &pkt) != 0) continue;

            if (pkt.keyup == 0) continue;  /* UNKEY — MMDVMHost drains its buffer naturally */

            if (pkt.type == USRP_TYPE_OPUS && pkt.opus_len > 0) {
                int16_t pcm[TX_ACCUM_MAX];
                int decoded = opus_codec_decode(codec, pkt.opus, (int)pkt.opus_len, pcm);
                if (decoded > 0)
                    fm_send_data(mmdvm_sock, &mmdvm_peer, pcm, decoded);
            } else if (pkt.type == USRP_TYPE_VOICE) {
                fm_send_data(mmdvm_sock, &mmdvm_peer,
                             pkt.audio, (int)USRP_AUDIO_FRAMES);
            }
        }
    }

    /* If mid-transmission at shutdown, send UNKEY to the network peer */
    if (tx_active) {
        usrp_build_key(net_pkt, tx_seq, 0);
        sendto(net_sock, net_pkt, USRP_PKT_LEN, 0,
               (const struct sockaddr *)&net_peer, sizeof(net_peer));
    }

    close(mmdvm_sock);
    close(net_sock);
    opus_codec_destroy(codec);

    fprintf(stderr, "opus-gateway: stopped\n");
    return 0;
}
