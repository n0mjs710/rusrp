#pragma once

#include "config.h"
#include "audio_alsa.h"
#include "audio_processing.h"
#include "logic_hid.h"
#include "jitter_buffer.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct telemetry telemetry_t;

int  telemetry_create(telemetry_t **tel, const config_t *cfg);
void telemetry_log(telemetry_t *tel,
                   audio_alsa_t          *alsa,
                   audio_proc_t          *in_proc,
                   audio_proc_t          *out_proc,
                   const logic_hid_t     *logic,
                   jitter_buffer_t       *jb);
void telemetry_destroy(telemetry_t *tel);
