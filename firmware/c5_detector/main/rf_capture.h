/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t windows;
    uint32_t valid_windows;
    uint32_t receive_errors;
    uint32_t stale_windows;
    uint32_t last_power;
    uint32_t last_clip_pm;
    uint64_t power_sum;
    uint32_t power_windows;
    uint32_t nonzero_power_windows;
    uint32_t clipped_windows;
    uint64_t iq_samples;
    uint64_t clipped_iq_samples;
    uint64_t last_sample_us;
    uint32_t generation;
    uint64_t stable_after_us;
} rf_capture_stats_t;

esp_err_t rf_capture_start(void);
void rf_capture_get_stats(rf_capture_stats_t *out);
esp_err_t rf_capture_set_channel(uint8_t channel);
esp_err_t rf_capture_set_gain(uint8_t gain);
uint16_t rf_capture_channel_frequency(uint8_t channel);
esp_err_t rf_capture_scan(uint8_t start, uint16_t power[8]);
typedef struct {
    uint8_t state, completed, total, channel;
    uint16_t power, clip_pm;
    uint32_t generation;
} rf_scan_status_t;
bool rf_scan_busy(void);
esp_err_t rf_scan_start(uint8_t start, uint8_t count);
void rf_scan_cancel(void);
rf_scan_status_t rf_scan_status(void);
bool rf_scan_result(uint8_t channel, uint16_t *power, uint16_t *clip, uint8_t *quality);
