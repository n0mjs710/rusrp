#pragma once

#include "config.h"
#include "usrp_protocol.h"

typedef void (*usrp_rx_cb_t)(const usrp_packet_t *pkt, void *userdata);

typedef struct usrp_transport usrp_transport_t;

int  usrp_transport_create(usrp_transport_t **t, const config_t *cfg,
                           usrp_rx_cb_t rx_cb, void *userdata);

/* Send a pre-built USRP packet (buf must be USRP_PKT_LEN bytes). */
int  usrp_transport_send(usrp_transport_t *t, const uint8_t *buf, size_t len);

void usrp_transport_destroy(usrp_transport_t *t);
