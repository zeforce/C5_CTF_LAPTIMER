/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t channel;       /* 64-channel analog and digital VTX table index. */
    uint8_t gain;          /* Fixed PHY gain index. */
    uint16_t enter_power;
    uint16_t exit_power;
    uint16_t confirm_ms;
} detector_config_t;

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    uint16_t power;
    uint16_t clip_pm;
    uint8_t edge;          /* 1 ENTER, 2 EXIT. */
    uint8_t valid;
} detector_event_t;

typedef struct {
    uint16_t power;
    uint16_t clip_pm;
    uint32_t sample_count;
    uint32_t overflow_count;
    uint8_t queue_depth;
    uint8_t active;
    uint8_t calibrated;
    uint64_t timestamp_us;
    uint32_t varied_windows;
    uint8_t quality_flags;
} detector_observation_t;

void detector_start(void);
#if C5_ACCEPTANCE_TEST
unsigned detector_acceptance_test(void);
bool detector_transport_stimulus(uint8_t cycles);
#endif
bool detector_set_config(const detector_config_t *config);
detector_config_t detector_get_config(void);
detector_observation_t detector_get_observation(void);
bool detector_peek(detector_event_t *event);
bool detector_ack(uint32_t sequence);
bool detector_overflow_latched(void);
bool detector_clear_fault(void);
void detector_set_active(bool active);
void detector_set_calibrated(bool calibrated);
typedef struct {
    uint16_t observation_ms, clip_limit_pm, varied_min_pm, min_peak_delta;
    uint32_t max_hold_ms;
} detector_limits_t;
detector_limits_t detector_get_limits(void);
bool detector_set_limits(const detector_limits_t *limits);
