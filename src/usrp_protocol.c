#include "usrp_protocol.h"
#include <string.h>
#include <arpa/inet.h>

/* Safe big-endian word accessors — avoids strict aliasing UB from casting
 * uint8_t* to uint32_t*.  Compilers reduce these to single load/store insns. */
static inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return ntohl(v);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    v = htonl(v);
    memcpy(p, &v, 4);
}

int usrp_parse(const uint8_t *buf, size_t len, usrp_packet_t *out)
{
    if (len < USRP_HEADER_LEN)
        return -1;
    if (memcmp(buf, USRP_MAGIC, 4) != 0)
        return -1;

    out->seq      = rd32(buf + 4);
    out->memory   = rd32(buf + 8);
    out->keyup    = rd32(buf + 12);
    out->talker   = rd32(buf + 16);
    out->type     = (usrp_type_t)rd32(buf + 20);
    out->mpxid    = rd32(buf + 24);
    out->reserved = rd32(buf + 28);

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
    wr32(buf + 4,  seq);
    wr32(buf + 12, keyup);
    wr32(buf + 20, (uint32_t)USRP_TYPE_VOICE);
    memcpy(buf + USRP_HEADER_LEN, audio, USRP_AUDIO_BYTES);
}

void usrp_build_key(uint8_t *buf, uint32_t seq, uint32_t keyup)
{
    memset(buf, 0, USRP_PKT_LEN);
    memcpy(buf, USRP_MAGIC, 4);
    wr32(buf + 4,  seq);
    wr32(buf + 12, keyup);
    wr32(buf + 20, (uint32_t)USRP_TYPE_VOICE);
    /* audio payload remains zero (silence) */
}

