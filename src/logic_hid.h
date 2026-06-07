#pragma once

#include "config.h"
#include <stdbool.h>

typedef struct logic_hid logic_hid_t;

int  logic_hid_create(logic_hid_t **l, const config_t *cfg);

/* Atomically read the current input_active state (updated by the HID poll thread). */
bool logic_hid_input_active(const logic_hid_t *l);

/* Drive output_active. Thread-safe. */
int  logic_hid_set_output(logic_hid_t *l, bool active);

/* Releases output_active before closing the HID device. */
void logic_hid_destroy(logic_hid_t *l);
