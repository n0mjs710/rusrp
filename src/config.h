#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char     remote_host[256];
    uint16_t remote_port;
    uint16_t local_port;
    char     bind_address[64];
} config_usrp_t;

typedef struct {
    char  alsa_device[64];
    float input_gain_db;
    float output_gain_db;
    bool  input_highpass;
    bool  output_highpass;
} config_audio_t;

typedef struct {
    char hid_device[64];
    int  output_active_gpio;   /* GPIO number for output_active (PTT); default 3 */
    bool input_active_low;
    bool output_active_low;
} config_logic_t;

typedef struct {
    unsigned int jitter_buffer_ms;  /* 40–250 */
} config_network_t;

typedef struct {
    unsigned int network_timeout_ms;         /* no USRP traffic → force unkey */
    unsigned int output_active_tail_ms;      /* hold output_active after UNKEY */
    unsigned int startup_output_inhibit_ms;  /* suppress output_active at boot */
} config_watchdog_t;

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} log_level_t;

typedef struct {
    log_level_t  level;
    unsigned int status_interval_sec;
} config_logging_t;

typedef struct {
    config_usrp_t     usrp;
    config_audio_t    audio;
    config_logic_t    logic;
    config_network_t  network;
    config_watchdog_t watchdog;
    config_logging_t  logging;
} config_t;

void config_defaults(config_t *cfg);
int  config_load(config_t *cfg, const char *path);
