#include "usrp_protocol.h"
#include <string.h>
#include <arpa/inet.h>

int usrp_parse(const uint8_t *buf, size_t len, usrp_packet_t *out)
{
    if (len < USRP_HEADER_LEN)
        return -1;
    if (memcmp(buf, USRP_MAGIC, 4) != 0)
        return -1;

    const uint32_t *h = (const uint32_t *)(const void *)buf;
    out->seq      = ntohl(h[1]);
    out->memory   = ntohl(h[2]);
    out->keyup    = ntohl(h[3]);
    out->talker   = ntohl(h[4]);
    out->type     = (usrp_type_t)ntohl(h[5]);
    out->mpxid    = ntohl(h[6]);
    out->reserved = ntohl(h[7]);

    memset(out->audio, 0, sizeof(out->audio));
    if (out->type == USRP_TYPE_VOICE && len >= USRP_PKT_LEN)
        memcpy(out->audio, buf + USRP_HEADER_LEN, USRP_AUDIO_BYTES);

    return 0;
}

void usrp_build_voice(uint8_t *buf, uint32_t seq, uint32_t keyup,
                      const int16_t *audio)
{
    memset(buf, 0, USRP_PKT_LEN);
    memcpy(buf, USRP_MAGIC, 4);
    uint32_t *h = (uint32_t *)(void *)buf;
    h[1] = htonl(seq);
    h[3] = htonl(keyup);
    h[5] = htonl((uint32_t)USRP_TYPE_VOICE);
    memcpy(buf + USRP_HEADER_LEN, audio, USRP_AUDIO_BYTES);
}

void usrp_build_key(uint8_t *buf, uint32_t seq, uint32_t keyup)
{
    memset(buf, 0, USRP_PKT_LEN);
    memcpy(buf, USRP_MAGIC, 4);
    uint32_t *h = (uint32_t *)(void *)buf;
    h[1] = htonl(seq);
    h[3] = htonl(keyup);
    h[5] = htonl((uint32_t)USRP_TYPE_VOICE);
    /* audio payload remains zero (silence) */
}
