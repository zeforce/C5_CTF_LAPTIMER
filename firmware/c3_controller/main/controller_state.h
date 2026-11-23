/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "node_spi.h"
#include "game.h"

typedef struct {
    uint8_t protocol_version, node_online, rf_valid, capabilities;
    uint32_t boot_session, windows, valid_windows, receive_errors, raw_power, clip_pm;
    uint32_t spi_errors;
    uint32_t total_laps, last_lap_ms, fastest_lap_ms, event_overflows;
    uint16_t observed_power, observed_clip_pm;
    uint8_t running, calibrated, channel, gain;
    uint16_t enter_power, exit_power, confirm_ms;
    uint16_t scan_power[64];
    uint8_t scan_valid;
    uint8_t auto_gain_state, auto_gain_progress;
    uint8_t observation_quality;
    uint32_t observation_samples;
    uint64_t observation_updated_us;
    int32_t config_save_error;
} controller_snapshot_t;

void controller_get_snapshot(controller_snapshot_t *out);
esp_err_t controller_finder(bool enabled,uint8_t channel);
bool controller_finder_active(void);
/* Network worker resets an incomplete transfer before accepting a new host boot. */
void controller_config_transfer_reset(void);
bool controller_get_lap(uint16_t newest_index, uint32_t *lap_ms);
esp_err_t controller_set_config(const node_config_t *config);
/* Persist a desired RF config even if no C5 is attached; never calibrates it. */
esp_err_t controller_stage_config(const node_config_t *config);
esp_err_t controller_game_ready(bool resume);
esp_err_t controller_set_calibrated(bool calibrated);
esp_err_t controller_set_running(bool running);
esp_err_t controller_scan(void);
esp_err_t controller_auto_gain(bool start);
/* Reject maintenance while RF work or a failed-stop obligation remains. */
esp_err_t controller_maintenance_ready(void);
/* Successful begin retains the RF mutex across a short local storage operation.
 * Caller holds network control_lock; pair every success with end. */
esp_err_t controller_maintenance_begin(void);
void controller_maintenance_end(void);
bool controller_game_phase_try(uint8_t *state,uint32_t *session);
esp_err_t controller_game_config(const game_config_t *config);
esp_err_t controller_game_rules(uint8_t mode, uint32_t minimum_ms, uint32_t duration_ms, uint32_t capture_ms);
esp_err_t controller_game_pilot(unsigned index, const game_pilot_t *pilot);
esp_err_t controller_game_name(unsigned index, const char *name);
esp_err_t controller_game_action(uint8_t action);
esp_err_t controller_game_save_result(void);
esp_err_t controller_game_start_session(uint32_t session);
esp_err_t controller_game_resume_session(uint32_t session);
esp_err_t controller_game_fault_pause(uint8_t fault);
/* Refuse a stale guard observation after another match replaced its session. */
esp_err_t controller_game_fault_pause_session(uint8_t fault,uint32_t session);
bool controller_game_feedback(uint8_t *state,uint8_t *owner,uint8_t *fault);
#include "feedback.h"
bool controller_feedback_read(feedback_input_t *out);
/* Network-facing coherent snapshot excludes per-pilot lap rings. */
typedef struct {
    uint8_t state,mode,owner,fault;
    uint8_t acquisition[12];
    uint8_t outcome[24]; /* state/mode/winner/fault/solo flags/reason + timing */
    uint32_t session;uint64_t fault_revision;
    uint64_t team_us[2],pilot_us[GAME_PILOTS];
    uint32_t pilot_laps[GAME_PILOTS],pilot_fastest_ms[GAME_PILOTS],pilot_captures[GAME_PILOTS];
    game_pilot_t match_pilots[GAME_PILOTS];
} controller_game_summary_t;
bool controller_game_summary(controller_game_summary_t *out);
bool controller_game_phase(uint8_t *state,uint32_t *session);

esp_err_t controller_game_tuning(uint16_t freshness,uint16_t drain,uint16_t recover);

bool controller_game_config_get(game_config_t *out);
bool controller_game_page(unsigned page,unsigned offset,uint8_t out[32]);

esp_err_t controller_event_capture(void);

/* Independent consumers peek then commit delivery.position locally. */
#include "event_service.h"
esp_err_t controller_event_peek(uint64_t cursor,service_event_t *event,event_delivery_t *delivery);

esp_err_t controller_event_acquisition(bool active);

bool controller_game_summary_try(controller_game_summary_t *out);
esp_err_t controller_event_peek_try(uint64_t cursor,service_event_t *event,event_delivery_t *delivery);

esp_err_t controller_slot_event_peek(unsigned slot,uint64_t cursor,service_event_t *event,event_delivery_t *delivery);

esp_err_t controller_slot_config(unsigned slot,const node_config_t *config);

bool controller_slot_config_page_try(unsigned slot,uint8_t out[32]);

esp_err_t controller_slot_capture(unsigned slot,unsigned action);
esp_err_t controller_stop_all_capture(void);
