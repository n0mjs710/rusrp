#pragma once

#include "config.h"
#include "logic_hid.h"
#include "jitter_buffer.h"
#include "audio_alsa.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* Half-duplex floor state — shared between the capture callback and watchdog.
 * First direction to claim FLOOR_IDLE wins; the other is blocked until release. */
typedef enum { FLOOR_IDLE = 0, FLOOR_INPUT, FLOOR_OUTPUT } floor_t;

typedef struct watchdog watchdog_t;

int  watchdog_create(watchdog_t **wd, const config_t *cfg,
                     logic_hid_t *logic, jitter_buffer_t *jb,
                     atomic_int *floor);

/* Provide the ALSA handle after audio is initialized, to enable playback
 * drain on transmission end. */
void watchdog_set_alsa(watchdog_t *wd, audio_alsa_t *alsa);

/* Call on every received USRP packet to reset the network timeout clock. */
void watchdog_packet_received(watchdog_t *wd);

/* Call on USRP key state change. */
void watchdog_key_event(watchdog_t *wd, bool keyed);

/* Number of forced output releases due to network timeout since last call (resets counter). */
uint64_t watchdog_timeout_count(watchdog_t *wd);

void watchdog_destroy(watchdog_t *wd);
