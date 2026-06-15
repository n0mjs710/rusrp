#pragma once

#include <stdint.h>
#include <stddef.h>

#define USRP_MAGIC          "USRP"
#define USRP_HEADER_LEN     32u
#define USRP_AUDIO_FRAMES   160u
#define USRP_AUDIO_BYTES    (USRP_AUDIO_FRAMES * sizeof(int16_t))
#define USRP_PKT_LEN        (USRP_HEADER_LEN + USRP_AUDIO_BYTES)  /* 352 bytes */

typedef enum {
    USRP_TYPE_VOICE      = 0,
    USRP_TYPE_DTMF       = 1,
    USRP_TYPE_TEXT       = 2,
    USRP_TYPE_PING       = 3,
    USRP_TYPE_TLV        = 4, /* DVSwitch: generic TLV/AMBE carrier — do not use */
    USRP_TYPE_VOICE_ADPCM = 5, /* DVSwitch: ADPCM compressed audio — do not use   */
    USRP_TYPE_VOICE_ULAW  = 6, /* DVSwitch: μ-law compressed audio — do not use   */
    USRP_TYPE_OPUS       = 7, /* rusrp extension: Opus-encoded audio, var length  */
} usrp_type_t;

typedef struct {
    uint32_t    seq;
    uint32_t    memory;
    uint32_t    keyup;       /* 0 = unkeyed, non-zero = keyed */
    uint32_t    talker;
    usrp_type_t type;
    uint32_t    mpxid;
    uint32_t    reserved;
    int16_t     audio[USRP_AUDIO_FRAMES]; /* valid when type == USRP_TYPE_VOICE */
} usrp_packet_t;

/* Parse a raw UDP payload into *out. Returns 0 on success, -1 on bad packet. */
int usrp_parse(const uint8_t *buf, size_t len, usrp_packet_t *out);

/* Build a 352-byte voice packet into buf (must be >= USRP_PKT_LEN). */
void usrp_build_voice(uint8_t *buf, uint32_t seq, uint32_t keyup,
                      const int16_t *audio);

/* Build a 352-byte key/unkey frame with silence audio. */
void usrp_build_key(uint8_t *buf, uint32_t seq, uint32_t keyup);
