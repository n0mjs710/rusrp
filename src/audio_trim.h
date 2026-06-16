#pragma once

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_TRIM_FRAME_SAMPLES 160u   /* must match USRP_AUDIO_FRAMES */

typedef struct audio_trim audio_trim_t;

int  audio_trim_create(audio_trim_t **t, unsigned int leading_frames);
void audio_trim_destroy(audio_trim_t *t);

void audio_trim_tx_start(audio_trim_t *t);
void audio_trim_tx_end(audio_trim_t *t);

bool audio_trim_process(audio_trim_t *t, const int16_t *in, int16_t *out);
bool audio_trim_active(const audio_trim_t *t);
