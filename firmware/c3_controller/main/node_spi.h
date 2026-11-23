/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "receiver_service.h"
#include "../../common/node_contract.h"

typedef struct {
    uint8_t schema, contract, firmware_major, firmware_minor;
    uint8_t channels, max_gain, event_capacity, max_payload;
    uint32_t features;
    uint16_t observation_ms, settle_ms, clip_limit_pm, stale_ms, bandwidth_mhz;
} node_capabilities_t;
typedef struct { uint64_t timestamp_us; uint32_t session; } node_clock_t;
typedef struct {
    uint64_t timestamp_us;
    uint16_t power, clip_pm;
    uint32_t sample_count, session;
    uint8_t quality_flags, channel;
} node_observation_detail_t;

typedef struct {
    uint8_t mac[6], channel, rf_valid, faults;
    uint32_t session, transactions;
} node_info_t;
typedef struct {
    uint32_t windows, valid_windows, receive_errors, raw_power, clip_pm;
} node_rf_stats_t;
typedef receiver_config_t node_config_t;
typedef struct {
    uint16_t power, clip_pm;
    uint32_t sample_count, overflow_count;
    uint8_t queue_depth, active, calibrated;
} node_observation_t;
typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    uint16_t power, clip_pm;
    uint8_t edge, valid;
} node_event_t;

esp_err_t node_spi_init(void);
esp_err_t node_spi_get_info(node_info_t *out);
esp_err_t node_spi_get_rf_stats(node_rf_stats_t *out);
esp_err_t node_spi_get_config(node_config_t *out);
esp_err_t node_spi_set_config(const node_config_t *config);
esp_err_t node_spi_get_observation(node_observation_t *out);
esp_err_t node_spi_get_event(node_event_t *out);
esp_err_t node_spi_ack_event(uint32_t sequence);
esp_err_t node_spi_set_active(bool active, bool calibrated);
esp_err_t node_spi_scan(uint8_t start, uint16_t power[8]);
bool node_spi_ready(void);
bool node_spi_interrupt(void);
esp_err_t node_spi_get_capabilities(node_capabilities_t *out);
esp_err_t node_spi_get_clock(node_clock_t *out);
esp_err_t node_spi_get_observation_detail(node_observation_detail_t *out);
esp_err_t node_spi_exchange(uint8_t command, const uint8_t *input, uint8_t input_length,
    uint8_t expected_length, uint8_t *output);

/* Explicit slots: board setup only, before workers start. No GPIO reassignment
 * through BLE. Slot0 is the existing CS7/READY3/INT10 board profile. */
#include "node_slot_pins.h"
esp_err_t node_spi_attach(unsigned index,const node_slot_pins_t *pins);
bool node_spi_slot_enabled(unsigned index);
bool node_spi_slot_ready(unsigned index);
/* Default READY budget: 100ms shared by both frames, 2s for legacy scan9.
 * Explicit absolute deadlines override defaults. IDF transmission is not cancellable. */
esp_err_t node_spi_exchange_slot(unsigned index,uint8_t command,const uint8_t *input,uint8_t input_length,uint8_t expected_length,uint8_t *output);
typedef struct {uint32_t requests,errors,invalid_responses;int32_t last_error;uint64_t mutation_revision;} node_spi_slot_stats_t;
bool node_spi_slot_stats(unsigned index,node_spi_slot_stats_t *out);

/* Absolute C3 deadline covers READY phases of both request/NOP frames.
 * Uses a zero-wait per-device lock. Does not cancel an in-flight IDF transfer. */
esp_err_t node_spi_exchange_until(unsigned index,uint8_t command,const uint8_t *input,uint8_t input_length,uint8_t expected_length,uint8_t *output,int64_t deadline_us);

/* One bounded diagnostic exchange per step. Caller reserves slot0 across steps. */
typedef struct { uint8_t kind,step,sequence,config[8],health[22]; } node_spi_protocol_test_t;
esp_err_t node_spi_protocol_step(node_spi_protocol_test_t *test,bool *done);
