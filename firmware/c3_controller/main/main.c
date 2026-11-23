#include "game_selftest.h"
#include "game_feedback.h"
#include "game_identity.h"
/* SPDX-License-Identifier: GPL-3.0-only */
#include <string.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ble_control.h"
#include "network.h"
#include "io_service.h"
#include "controller_state.h"
#include "lap_timer.h"
#include "game_page.h"
#include "storage_fault.h"
#include "node_time.h"
#include "node_poll_guard.h"
#include "event_service.h"
#include "receiver_poll.h"
#include "receiver_poll_step.h"
#include "receiver_events.h"
#include "receiver_restore.h"
#include "receiver_scan.h"
#include "game_receiver.h"
#include "game_signal.h"
#include "../../common/node_contract.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static void serial_diag_task(void *arg);
static int32_t additional_slot_exchange(void *,unsigned,uint8_t,const uint8_t *,uint8_t,uint8_t,uint8_t *);
static uint64_t additional_slot_now(void *);

static const char *TAG = "c3_controller";
static controller_snapshot_t snapshot = {.protocol_version = 1};
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static lap_timer_t timer;
static game_t game;
static receiver_service_t receivers;
static game_receiver_t game_receivers;
#define node_time receivers.slots[0].clock
#define event_service receivers.slots[0].events
static event_snapshot_t event_snapshot;
static bool event_capture_active,event_capture_baseline;
static bool scan_owned,scan_cancel_pending;
static bool finder_active;
bool controller_finder_active(void){return __atomic_load_n(&finder_active,__ATOMIC_RELAXED);}
static bool game_preparing;
static bool eventwatch_owned,eventwatch_cancel;
static bool protocol_owned,protocol_cancel;
static unsigned protocol_steps;
static esp_err_t protocol_error;
static node_spi_protocol_test_t protocol_test;
static bool clocktest_owned,clocktest_cancel;
static unsigned clocktest_samples;
static esp_err_t clocktest_error;
/* node_lock protects status, including updates from the diagnostic worker. */
static struct {uint8_t phase;int32_t error;uint32_t retries,events;} eventwatch_status;
static receiver_scan_stop_t scan_stop_step;
static uint32_t event_capture_overflow,event_capture_error;
static game_signal_t flag_signal;
static game_signal_scan_t flag_scan;
static node_config_t flag_original[RECEIVER_SLOTS];
static bool flag_have_original[RECEIVER_SLOTS];
static receiver_restore_step_t flag_restore_steps[RECEIVER_SLOTS];
static bool flag_restore_busy(void){for(unsigned i=0;i<RECEIVER_SLOTS;i++)if(flag_have_original[i])return true;return false;}

static esp_err_t game_save(bool result);
static uint32_t game_saved_session;
static esp_err_t game_result_error;
static esp_err_t game_save_result(void);
static bool additional_capture_busy(void){for(unsigned i=1;i<RECEIVER_SLOTS;i++)if(receiver_capture_busy(&receivers.slots[i]))return true;return false;}
static bool primary_reconnecting;
static bool primary_capture_busy(void){return controller_finder_active() || protocol_owned || clocktest_owned || eventwatch_owned || primary_reconnecting || receivers.slots[0].restore_pending || scan_owned || event_capture_active || receivers.slots[0].stop_pending || (game.state!=GAME_RUNNING && game.state!=GAME_PAUSED && flag_restore_busy());}
static bool game_busy(void) { return game.state == GAME_RUNNING || game.state == GAME_PAUSED; }
static SemaphoreHandle_t node_lock;
static bool calibrated;
static uint32_t poll_completed,poll_discarded;
/* Caller holds node_lock. Preserve uncertain stop outcomes across probe errors. */
static esp_err_t primary_stop(bool keep_calibration){
    receivers.slots[0].stop_pending=true;event_capture_active=false;
    esp_err_t e=node_spi_set_active(false,keep_calibration);
    receiver_stop_result(&receivers.slots[0],e);event_capture_error=e;
    if(e!=ESP_OK || !keep_calibration)calibrated=false;
    return e;
}
static bool auto_gain_cancel;
static void auto_gain_task(void *arg);
static node_config_t saved_config;
static bool have_saved_config;

typedef struct {
    uint32_t version;
    uint32_t total_laps, fastest_lap_number;
    uint64_t fastest_us;
    uint16_t recent_count, recent_next;
    uint64_t recent_us[LAP_RECENT_MAX];
    uint32_t checksum;
} saved_result_t;

static uint32_t checksum_bytes(const uint8_t *bytes, size_t length)
{
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < length; ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}

static void save_results(void)
{
    saved_result_t result = { .version = 1, .total_laps = timer.total_laps,
        .fastest_lap_number = timer.fastest_lap_number, .fastest_us = timer.fastest_us,
        .recent_count = timer.recent_count, .recent_next = timer.recent_next };
    memcpy(result.recent_us, timer.recent_us, sizeof(result.recent_us));
    result.checksum = checksum_bytes((const uint8_t *)&result, offsetof(saved_result_t, checksum));
    nvs_handle_t handle;
    if (nvs_open("race", NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(handle, "last", &result, sizeof(result));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "save results failed: %s", esp_err_to_name(err));
}

static void load_results(void)
{
    nvs_handle_t handle;
    if (nvs_open("race", NVS_READONLY, &handle) != ESP_OK) return;
    saved_result_t result;
    size_t length = sizeof(result);
    esp_err_t err = nvs_get_blob(handle, "last", &result, &length);
    nvs_close(handle);
    if (err != ESP_OK || length != sizeof(result) || result.version != 1 ||
        result.recent_count > LAP_RECENT_MAX || result.recent_next >= LAP_RECENT_MAX ||
        result.checksum != checksum_bytes((const uint8_t *)&result, offsetof(saved_result_t, checksum))) return;
    timer.total_laps = result.total_laps;
    timer.fastest_lap_number = result.fastest_lap_number;
    timer.fastest_us = result.fastest_us;
    timer.recent_count = result.recent_count;
    timer.recent_next = result.recent_next;
    memcpy(timer.recent_us, result.recent_us, sizeof(timer.recent_us));
    snapshot.total_laps = timer.total_laps;
    snapshot.fastest_lap_ms = timer.fastest_us / 1000u;
    if (timer.recent_count) {
        uint16_t index = (timer.recent_next + LAP_RECENT_MAX - 1u) % LAP_RECENT_MAX;
        snapshot.last_lap_ms = timer.recent_us[index] / 1000u;
    }
}

typedef struct {
    uint32_t version;
    node_config_t config;
    uint32_t checksum;
} saved_config_t;

static esp_err_t save_config(const node_config_t *config)
{
    saved_config_t saved = {.version = 1, .config = *config};
    saved.checksum = checksum_bytes((const uint8_t *)&saved, offsetof(saved_config_t, checksum));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("race", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "config", &saved, sizeof(saved));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "save config failed: %s", esp_err_to_name(err));
    return err;
}

static void load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open("race", NVS_READONLY, &handle) != ESP_OK) return;
    saved_config_t saved = {0};
    size_t length = sizeof(saved);
    esp_err_t err = nvs_get_blob(handle, "config", &saved, &length);
    nvs_close(handle);
    if (err == ESP_OK && length == sizeof(saved) && saved.version == 1 &&
        saved.checksum == checksum_bytes((const uint8_t *)&saved, offsetof(saved_config_t, checksum)) &&
        saved.config.channel < 64 && saved.config.gain <= 63 &&
        saved.config.enter_power > saved.config.exit_power &&
        saved.config.confirm_ms >= 1 && saved.config.confirm_ms <= 1000) {
        saved_config = saved.config;
        have_saved_config = true;
        snapshot.channel=saved.config.channel;snapshot.gain=saved.config.gain;
        snapshot.enter_power=saved.config.enter_power;snapshot.exit_power=saved.config.exit_power;
        snapshot.confirm_ms=saved.config.confirm_ms;
    }
}

/* Additional slots use separate keys; slot0 remains the canonical race/config
 * used by existing RF controls. Caller holds node_lock. */
static esp_err_t slot_config_store(unsigned slot,const node_config_t *config){
 char key[12];snprintf(key,sizeof(key),"rfslot%u",slot);
 saved_config_t record={.version=1,.config=*config};record.checksum=checksum_bytes((uint8_t*)&record,offsetof(saved_config_t,checksum));
 nvs_handle_t h;esp_err_t e=nvs_open("race",NVS_READWRITE,&h);if(e!=ESP_OK)return e;
 e=nvs_set_blob(h,key,&record,sizeof(record));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e;
}
static void slot_configs_load(void){
 nvs_handle_t h;if(nvs_open("race",NVS_READONLY,&h)!=ESP_OK)return;
 for(unsigned slot=1;slot<RECEIVER_SLOTS;slot++){
  char key[12];snprintf(key,sizeof(key),"rfslot%u",slot);saved_config_t record={0};size_t length=sizeof(record);
  if(nvs_get_blob(h,key,&record,&length)==ESP_OK && length==sizeof(record) && record.version==1 && record.checksum==checksum_bytes((uint8_t*)&record,offsetof(saved_config_t,checksum)))receiver_desire(&receivers,slot,&record.config);
 }nvs_close(h);
}
esp_err_t controller_slot_config(unsigned slot,const node_config_t *config){
 if(slot>=RECEIVER_SLOTS || !receiver_config_valid(config))return ESP_ERR_INVALID_ARG;
 if(slot==0){esp_err_t e=controller_stage_config(config);
  if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(100))==pdTRUE){if(have_saved_config)receiver_desire(&receivers,0,&saved_config);receivers.slots[0].config_error=e;xSemaphoreGive(node_lock);}return e;
 }
 if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
 receiver_slot_t *r=&receivers.slots[slot];esp_err_t e=ESP_OK;
 if(game_busy() || primary_capture_busy() || timer.running || snapshot.auto_gain_state==1 || receiver_capture_busy(r))e=ESP_ERR_INVALID_STATE;
 else {
  /* Save first: unavailable/disabled hardware can receive a desired profile.
   * Apply and readback are separate from persistence, with explicit error. */
  e=slot_config_store(slot,config);
  if(e==ESP_OK){receiver_desire(&receivers,slot,config);r->calibrated=false;
   /* The receiver worker owns STOP/SET/GET. Success here means persisted
    * desire; config page pending remains true until verified convergence. */
   if(r->enabled)receiver_request_stop(&receivers,1u<<slot,0);
  }
 }r->config_error=e;xSemaphoreGive(node_lock);return e;
}

bool controller_get_lap(uint16_t newest_index, uint32_t *lap_ms)
{
    if (!lap_ms || !node_lock || xSemaphoreTake(node_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool present = newest_index < timer.recent_count;
    if (present) {
        uint16_t index = (timer.recent_next + LAP_RECENT_MAX - 1u - newest_index) % LAP_RECENT_MAX;
        *lap_ms = timer.recent_us[index] / 1000u;
    }
    xSemaphoreGive(node_lock);
    return present;
}

/* Synchronous callers retain verified completion, but wait without node_lock.
 * The poll worker performs one bounded exchange per turn. */
esp_err_t controller_set_config(const node_config_t *config)
{
 if(!config || !receiver_config_valid(config))return ESP_ERR_INVALID_ARG;
 controller_snapshot_t current;controller_get_snapshot(&current);
 if(!current.node_online)return ESP_ERR_INVALID_STATE;
 esp_err_t error=controller_stage_config(config);if(error)return error;
 int64_t until=esp_timer_get_time()+5000000;
 do{
  if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(100))==pdTRUE){
   receiver_slot_t *r=&receivers.slots[0];
   bool same=r->have_desired && !memcmp(&r->desired,config,8);
   bool done=same && !primary_reconnecting && receiver_config_ready(r) && r->have_observed && !memcmp(&r->observed,config,8);
   error=same?r->config_error:ESP_ERR_INVALID_STATE;
   xSemaphoreGive(node_lock);
   if(done)return ESP_OK;
   if(error)return error;
  }
  vTaskDelay(pdMS_TO_TICKS(20));
 }while(esp_timer_get_time()<until);
 return ESP_ERR_TIMEOUT;
}

esp_err_t controller_stage_config(const node_config_t *config)
{
 if(!config || !receiver_config_valid(config))return ESP_ERR_INVALID_ARG;
 if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return ESP_ERR_TIMEOUT;
 if(additional_capture_busy() || game_busy() || timer.running || snapshot.auto_gain_state==1){xSemaphoreGive(node_lock);return ESP_ERR_INVALID_STATE;}
 receiver_slot_t *r=&receivers.slots[0];
 /* Retrying the identical persisted desire is idempotent, including while
  * offline or while its background application still owns the receiver. */
 bool same=have_saved_config && !snapshot.config_save_error && !memcmp(&saved_config,config,8);
 if(same){xSemaphoreGive(node_lock);return ESP_OK;}
 if(primary_capture_busy()){xSemaphoreGive(node_lock);return ESP_ERR_INVALID_STATE;}
 esp_err_t error=save_config(config);
 if(!error){
  saved_config=*config;have_saved_config=true;calibrated=false;receiver_desire(&receivers,0,config);r->calibrated=false;r->config_error=0;
  if(r->enabled && r->online){primary_reconnecting=true;r->restore_pending=true;receiver_request_stop(&receivers,1,0);}
 }
 taskENTER_CRITICAL(&state_lock);snapshot.config_save_error=error;
 if(!error){snapshot.calibrated=0;snapshot.scan_valid=0;snapshot.auto_gain_state=0;snapshot.auto_gain_progress=0;}
 taskEXIT_CRITICAL(&state_lock);
 xSemaphoreGive(node_lock);return error;
}

esp_err_t controller_set_calibrated(bool value)
{
    if (!node_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(node_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (additional_capture_busy() || primary_capture_busy() || snapshot.auto_gain_state == 1 || game_busy()) { xSemaphoreGive(node_lock); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = value && !receiver_config_ready(&receivers.slots[0]) ? ESP_ERR_INVALID_STATE :
        !snapshot.node_online && !value ? ESP_OK :
        snapshot.node_online && !timer.running ? node_spi_set_active(false, value) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        calibrated = value;
        taskENTER_CRITICAL(&state_lock); snapshot.calibrated = value; taskEXIT_CRITICAL(&state_lock);
    }
    xSemaphoreGive(node_lock);
    return err;
}

esp_err_t controller_set_running(bool value)
{
    return controller_game_action(value ? 1 : 0);
}

typedef struct { uint32_t version; game_config_t config; uint32_t checksum; } game_saved_config_t;
/* Persist one lap ring per physical receiver. */
typedef struct {
 uint32_t version;game_config_t config;
 uint8_t state_data[sizeof(game_t)-offsetof(game_t,state)];
 struct {uint32_t total,fastest_number;uint64_t fastest;} local_laps[RECEIVER_SLOTS];
 uint32_t checksum;
} game_saved_result_t;
static esp_err_t game_save(bool result)
{
    if(storage_fault_take(result?STORAGE_FAULT_GAME_RESULT:STORAGE_FAULT_GAME_CONFIG))return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    nvs_handle_t h; esp_err_t err=nvs_open("game",NVS_READWRITE,&h); if(err!=ESP_OK)return err;
    if(result) {
        game_saved_result_t *saved=calloc(1,sizeof(*saved));
        if(!saved){nvs_close(h);return ESP_ERR_NO_MEM;}
        saved->version=8;saved->config=*game_session_config(&game);
        memcpy(saved->state_data,&game.state,sizeof(saved->state_data));
        if(saved->config.mode==GAME_LAPS)for(unsigned i=0;i<GAME_PILOTS;i++)if(saved->config.pilots[i].enabled){unsigned slot=saved->config.pilots[i].slot;saved->local_laps[slot].total=game.laps[i].total_laps;saved->local_laps[slot].fastest_number=game.laps[i].fastest_lap_number;saved->local_laps[slot].fastest=game.laps[i].fastest_us;}
        saved->checksum=checksum_bytes((uint8_t*)saved,offsetof(game_saved_result_t,checksum));
        err=nvs_set_blob(h,"result",saved,sizeof(*saved));free(saved);
    } else {
        game_saved_config_t saved={.version=3,.config=game.config};
        saved.checksum=checksum_bytes((uint8_t*)&saved,offsetof(game_saved_config_t,checksum));
        err=nvs_set_blob(h,"config",&saved,sizeof(saved));
    }
    if(err==ESP_OK){err=nvs_commit(h);}
    nvs_close(h);return err;
}
static esp_err_t game_save_result(void){
    game_result_error=game_save(true);
    if(game_result_error==ESP_OK)game_saved_session=game.session;
    return game_result_error;
}
esp_err_t controller_game_save_result(void){
    if(!node_lock || xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t e=game.state==GAME_FINISHED && game.session?game_save_result():ESP_ERR_INVALID_STATE;
    xSemaphoreGive(node_lock);return e;
}
static void game_load(void)
{
    nvs_handle_t h;if(nvs_open("game",NVS_READWRITE,&h)!=ESP_OK)return;
    /* Version7 stored four full rings, exhausting the 24KiB shared partition.
     * Retire only that incompatible result; retain rules, RF and network keys. */
    size_t old_size=0;
    if(nvs_get_blob(h,"result",NULL,&old_size)==ESP_OK && old_size>sizeof(game_saved_result_t)){
        if(nvs_erase_key(h,"result")==ESP_OK)nvs_commit(h);
    }
    game_saved_result_t *r=calloc(1,sizeof(*r));
    if(r){size_t n=sizeof(*r);if(nvs_get_blob(h,"result",r,&n)==ESP_OK && n==sizeof(*r) && r->version==8 &&
        r->checksum==checksum_bytes((uint8_t*)r,offsetof(game_saved_result_t,checksum)) && game_config_valid(&r->config) && r->state_data[0]==GAME_FINISHED){
            game.config=r->config;memcpy(&game.state,r->state_data,sizeof(r->state_data));game_saved_session=game.session;
            if(game.config.mode==GAME_LAPS)for(unsigned i=0;i<GAME_PILOTS;i++)if(game.config.pilots[i].enabled){unsigned slot=game.config.pilots[i].slot;game.laps[i].total_laps=r->local_laps[slot].total;game.laps[i].fastest_lap_number=r->local_laps[slot].fastest_number;game.laps[i].fastest_us=r->local_laps[slot].fastest;}
        }
        free(r);}
    game_saved_config_t c={0};size_t n=sizeof(c);
    if(nvs_get_blob(h,"config",&c,&n)==ESP_OK && n==sizeof(c) && c.version==3 &&
       c.checksum==checksum_bytes((uint8_t*)&c,offsetof(game_saved_config_t,checksum)) && game_config_valid(&c.config))game.config=c.config;
    nvs_close(h); /* Finished result may be read; never restart a match. */
}
bool controller_game_phase(uint8_t *state,uint32_t *session){
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return false;
    *state=game.state;*session=game.session;xSemaphoreGive(node_lock);return true;
}
bool controller_game_phase_try(uint8_t *state,uint32_t *session){
    if(!state || !session || !node_lock || xSemaphoreTake(node_lock,0)!=pdTRUE)return false;
    *state=game.state;*session=game.session;xSemaphoreGive(node_lock);return true;
}
bool controller_game_feedback(uint8_t *state,uint8_t *owner,uint8_t *fault){
    if(!state || !owner || !fault || !node_lock || xSemaphoreTake(node_lock,0)!=pdTRUE)return false;
    *state=game.state;*owner=game.owner;*fault=game.fault;xSemaphoreGive(node_lock);return true;
}
bool controller_feedback_read(feedback_input_t *out){
    if(!out || !node_lock || xSemaphoreTake(node_lock,0)!=pdTRUE)return false;
    game_feedback_read(&game,out);xSemaphoreGive(node_lock);return true;
}
static bool game_summary_read(controller_game_summary_t *out,TickType_t wait){
    if(!out || !node_lock || xSemaphoreTake(node_lock,wait)!=pdTRUE)return false;
    *out=(controller_game_summary_t){.state=game.state,.mode=game_session_config(&game)->mode,.owner=game.owner,.fault=game.fault,.session=game.session,.fault_revision=game.fault_revision};
    const game_config_t *rules=game_session_config(&game);
    out->outcome[0]=game.state;out->outcome[1]=rules->mode;out->outcome[2]=game.winner;out->outcome[3]=game.fault;
    out->outcome[4]=game.solo_started;out->outcome[5]=game.solo_failed;
    out->outcome[6]=game.state!=GAME_FINISHED?0:game.solo_failed?3:game.elapsed_us>=(uint64_t)rules->duration_ms*1000?2:1;
    uint32_t timing[4]={game.elapsed_us/1000,game.life_us/1000,rules->duration_ms,rules->capture_ms};
    memcpy(out->outcome+8,timing,sizeof(timing));
    out->acquisition[0]=event_capture_active;out->acquisition[1]=calibrated;memcpy(out->acquisition+2,&event_capture_error,4);memcpy(out->acquisition+6,&event_capture_overflow,4);
    memcpy(out->team_us,game.score_us,sizeof(out->team_us));
    memcpy(out->pilot_us,game.pilot_score_us,sizeof(out->pilot_us));
    memcpy(out->match_pilots,game_session_config(&game)->pilots,sizeof(out->match_pilots));
    for(unsigned i=0;i<GAME_PILOTS;i++){out->pilot_captures[i]=game.captures[i];out->pilot_laps[i]=game.laps[i].total_laps;out->pilot_fastest_ms[i]=game.laps[i].fastest_us/1000;}
    xSemaphoreGive(node_lock);return true;
}
bool controller_game_summary(controller_game_summary_t *out){return game_summary_read(out,pdMS_TO_TICKS(1000));}
bool controller_game_summary_try(controller_game_summary_t *out){return game_summary_read(out,0);}
bool controller_game_config_get(game_config_t *out){
    if(!out || xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return false;
    *out=game.config;xSemaphoreGive(node_lock);return true;
}
esp_err_t controller_event_acquisition(bool active){
    if(!node_lock || xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t e=ESP_OK;
    if(active){
        if(!receiver_config_ready(&receivers.slots[0]) || receivers.slots[0].stop_pending || additional_capture_busy() || game_busy() || timer.running || snapshot.auto_gain_state==1 || !snapshot.node_online || !snapshot.rf_valid || !calibrated)e=ESP_ERR_INVALID_STATE;
        else if(!event_capture_active){e=node_spi_set_active(true,true);if(e==ESP_OK){event_capture_active=true;event_capture_baseline=false;event_capture_error=0;}else {receivers.slots[0].stop_pending=true;event_capture_error=e;}}
    }else if(eventwatch_owned){
        eventwatch_cancel=true;
    }else if(primary_capture_busy()){
        e=primary_stop(calibrated);
    }
    taskENTER_CRITICAL(&state_lock);snapshot.running=event_capture_active || game.state==GAME_RUNNING;taskEXIT_CRITICAL(&state_lock);
    xSemaphoreGive(node_lock);return e;
}
esp_err_t controller_slot_capture(unsigned slot,unsigned action){
 if(slot>=RECEIVER_SLOTS || action>3)return ESP_ERR_INVALID_ARG;
 if(!slot)return action==0?controller_event_acquisition(false):action==2?controller_event_acquisition(true):controller_set_calibrated(action==1);
 if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return ESP_ERR_TIMEOUT;
 esp_err_t e=ESP_OK;
 if(action && (game_busy() || timer.running || primary_capture_busy() || snapshot.auto_gain_state==1))e=ESP_ERR_INVALID_STATE;
 else {int64_t deadline=esp_timer_get_time()+500000;int32_t result=receiver_capture_action(&receivers,slot,action,additional_slot_exchange,&deadline);e=result==-1?ESP_ERR_INVALID_STATE:result;}
 xSemaphoreGive(node_lock);return e;
}
esp_err_t controller_stop_all_capture(void){
 esp_err_t e=controller_event_acquisition(false);
 if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(100))!=pdTRUE)return ESP_ERR_TIMEOUT;
 for(unsigned i=1;i<RECEIVER_SLOTS;i++)if(receivers.slots[i].enabled && (receivers.slots[i].capture_requested || receivers.slots[i].active || receivers.slots[i].stop_pending)){
  int64_t deadline=esp_timer_get_time()+500000;int32_t result=receiver_capture_action(&receivers,i,0,additional_slot_exchange,&deadline);if(result)e=result;
 }
 xSemaphoreGive(node_lock);return e;
}
static esp_err_t event_peek_read(unsigned slot,uint64_t cursor,service_event_t *event,event_delivery_t *delivery,TickType_t wait){
    if(slot>=RECEIVER_SLOTS || !event || !delivery)return ESP_ERR_INVALID_ARG;
    if(!node_lock || xSemaphoreTake(node_lock,wait)!=pdTRUE)return ESP_ERR_TIMEOUT;
    bool ready=receivers.slots[slot].enabled && event_service_peek(&receivers.slots[slot].events,cursor,event,delivery);
    xSemaphoreGive(node_lock);return ready?ESP_OK:ESP_ERR_NOT_FOUND;
}
esp_err_t controller_event_peek(uint64_t cursor,service_event_t *event,event_delivery_t *delivery){return event_peek_read(0,cursor,event,delivery,pdMS_TO_TICKS(100));}
esp_err_t controller_event_peek_try(uint64_t cursor,service_event_t *event,event_delivery_t *delivery){return event_peek_read(0,cursor,event,delivery,0);}
esp_err_t controller_slot_event_peek(unsigned slot,uint64_t cursor,service_event_t *event,event_delivery_t *delivery){return event_peek_read(slot,cursor,event,delivery,0);}
esp_err_t controller_event_capture(void){
    if(!node_lock || xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    event_snapshot_capture(&event_snapshot,&event_service,esp_timer_get_time());xSemaphoreGive(node_lock);return ESP_OK;
}
bool controller_slot_config_page_try(unsigned slot,uint8_t out[32]){
 if(slot>=RECEIVER_SLOTS || !out || !node_lock || xSemaphoreTake(node_lock,0)!=pdTRUE)return false;
 bool ok=receiver_config_page(&receivers,slot,out);if(ok && !slot){out[25]=(receivers.slots[0].enabled?1:0)|(snapshot.node_online?2:0)|(snapshot.rf_valid?4:0)|(calibrated?8:0)|(event_capture_active?48:0)|(receivers.slots[0].stop_pending?64:0);memcpy(out+26,&event_capture_error,4);}xSemaphoreGive(node_lock);return ok;
}
bool controller_game_page(unsigned page,unsigned offset,uint8_t out[32]){
    if(page==122 && out){
        memset(out,0,32);out[0]=29;
        uint32_t heap[3]={heap_caps_get_free_size(MALLOC_CAP_8BIT),heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)};
        memcpy(out+4,heap,sizeof(heap));nvs_stats_t stats={0};esp_err_t e=nvs_get_stats(NULL,&stats);
        out[1]=e==ESP_OK;uint32_t entries[3]={stats.used_entries,stats.free_entries,stats.total_entries};memcpy(out+16,entries,sizeof(entries));memcpy(out+28,&e,4);return true;
    }
    if(!out || !node_lock || xSemaphoreTake(node_lock,pdMS_TO_TICKS(100))!=pdTRUE)return false;
    bool ok;
    if(page>=123 && page<123+GAME_PILOTS){unsigned pilot=page-123;ok=game_signal_page(&flag_signal,pilot,game_session_config(&game)->pilots[pilot].slot,esp_timer_get_time(),out);
    }else if(page==36){
        memset(out,0,32);out[0]=1;out[1]=36;out[2]=game.state==GAME_FINISHED && game.session && game_saved_session==game.session && game_result_error==ESP_OK;
        memcpy(out+4,&game.session,4);memcpy(out+8,&game_saved_session,4);memcpy(out+12,&game_result_error,4);ok=true;
    }else if(page>=118 && page<=121){receiver_slot_t *r=&receivers.slots[page-118];memset(out,0,32);out[0]=28;out[1]=page-118;out[2]=r->capture_requested;out[3]=r->stop_pending;memcpy(out+4,&r->capture_error,4);memcpy(out+8,&r->capture_overflow,4);if(page==118){out[2]=event_capture_active;memcpy(out+4,&event_capture_error,4);memcpy(out+8,&event_capture_overflow,4);}ok=true;
    }else if(page>=114 && page<=117){ok=receiver_clock_page(&receivers,page-114,esp_timer_get_time(),out);
    }else if(page>=110 && page<=113){ok=receiver_config_page(&receivers,page-110,out);
    }else if(page>=106 && page<=109){ok=receiver_page(&receivers,page-106,esp_timer_get_time(),out);
    }else if(page==37){
        memset(out,0,32);out[0]=1;out[1]=37;
        uint64_t now=esp_timer_get_time(),mapped;uint32_t bound;
        out[2]=node_time_map(&node_time,node_time.session,node_time.node_us,now,&mapped,&bound);
        memcpy(out+4,&node_time.session,4);memcpy(out+8,&node_time.offset_us,8);memcpy(out+16,&node_time.rtt_us,4);
        uint32_t age=node_time.valid && now>=node_time.sampled_us?(now-node_time.sampled_us)/1000:UINT32_MAX;
        memcpy(out+20,&age,4);memcpy(out+24,&node_time.samples,4);memcpy(out+28,&node_time.rejected,4);ok=true;
    }else if(page>=38 && page<71){event_service_page(&event_service,page,out);ok=true;}
    else if(page>=71 && page<=104)ok=event_snapshot_page(&event_snapshot,page,esp_timer_get_time(),out);
    else if(page==105){memset(out,0,32);out[0]=1;out[1]=105;out[2]=event_capture_active;out[3]=calibrated;memcpy(out+4,&event_capture_error,4);memcpy(out+8,&event_capture_overflow,4);ok=true;}
    else ok=game_page_encode(&game,page,offset,out);
    xSemaphoreGive(node_lock);return ok;
}
esp_err_t controller_game_config(const game_config_t *c){
    if(!game_config_valid(c))return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t e=game_busy()?ESP_ERR_INVALID_STATE:ESP_OK;
    if(e==ESP_OK){game_config_t before=game.config;game.config=*c;e=game_save(false);if(e!=ESP_OK)game.config=before;}xSemaphoreGive(node_lock);return e;
}
esp_err_t controller_game_rules(uint8_t mode,uint32_t minimum_ms,uint32_t duration_ms,uint32_t capture_ms)
{
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    game_config_t c=game.config;c.mode=mode;c.minimum_lap_ms=minimum_ms;c.duration_ms=duration_ms;c.capture_ms=capture_ms;
    esp_err_t err=game_busy() || snapshot.auto_gain_state==1?ESP_ERR_INVALID_STATE:!game_config_valid(&c)?ESP_ERR_INVALID_ARG:ESP_OK;
    if(err==ESP_OK){game_config_t before=game.config;game.config=c;err=game_save(false);if(err!=ESP_OK)game.config=before;}xSemaphoreGive(node_lock);return err;
}
esp_err_t controller_game_tuning(uint16_t freshness,uint16_t drain,uint16_t recover){
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    game_config_t c=game.config;c.freshness_ms=freshness;c.solo_drain_percent=drain;c.solo_recover_percent=recover;
    esp_err_t e=game_busy()?ESP_ERR_INVALID_STATE:!game_config_valid(&c)?ESP_ERR_INVALID_ARG:ESP_OK;
    if(e==ESP_OK){game_config_t before=game.config;game.config=c;e=game_save(false);if(e!=ESP_OK)game.config=before;}
    xSemaphoreGive(node_lock);return e;
}
esp_err_t controller_game_pilot(unsigned index,const game_pilot_t *pilot)
{
    if(index>=GAME_PILOTS || !pilot)return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    game_config_t c=game.config;c.pilots[index]=*pilot;
    esp_err_t err=game_busy()?ESP_ERR_INVALID_STATE:!game_config_valid(&c)?ESP_ERR_INVALID_ARG:ESP_OK;
    if(err==ESP_OK){game_config_t before=game.config;game.config=c;err=game_save(false);if(err!=ESP_OK)game.config=before;}xSemaphoreGive(node_lock);return err;
}
esp_err_t controller_game_name(unsigned index,const char *name)
{
    if(index>=GAME_PILOTS || !game_name_valid(name,13))return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t err=game_busy()?ESP_ERR_INVALID_STATE:ESP_OK;
    if(err==ESP_OK){game_pilot_t before=game.config.pilots[index];memset(game.config.pilots[index].name,0,17);memcpy(game.config.pilots[index].name,name,strlen(name));err=game_save(false);if(err!=ESP_OK)game.config.pilots[index]=before;}
    xSemaphoreGive(node_lock);return err;
}
/* Caller holds node_lock. The same checks apply to preflight and execution. */
static esp_err_t game_ready(bool resume)
{
    if(game_preparing || (game.state!=GAME_RUNNING && flag_restore_busy()))return ESP_ERR_INVALID_STATE;
    if(additional_capture_busy() || primary_capture_busy() || snapshot.auto_gain_state==1 ||
       (resume ? game.state!=GAME_PAUSED : game_busy()))return ESP_ERR_INVALID_STATE;
    if(!game_config_valid(&game.config))return ESP_ERR_INVALID_ARG;
    if(game.config.mode==GAME_LAPS){
        /* Slot0 controls update calibration synchronously, before the poll cache. */
        receivers.slots[0].calibrated=calibrated;
        if(!game_receiver_ready(&game.config,&receivers))return ESP_ERR_INVALID_STATE;
    }else if(!game_signal_ready(&game.config,&receivers))return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
esp_err_t controller_game_ready(bool resume)
{
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t error=game_ready(resume);xSemaphoreGive(node_lock);return error;
}
/* Publish obligations before returning control. The receiver worker owns IO. */
static void flag_restore_request(void){
 flag_scan.pending=false;
 for(unsigned slot=0;slot<RECEIVER_SLOTS;slot++)if(flag_have_original[slot]){
  flag_restore_steps[slot].phase=0;receivers.slots[slot].restore_pending=true;
  receiver_request_stop(&receivers,1u<<slot,0);
 }
 calibrated=false;
}
static void game_stop_request(int32_t reason){
 uint8_t mask=game_receiver_mask(game_session_config(&game));
 receiver_request_stop(&receivers,mask,reason);
 if(mask&1){event_capture_active=false;event_capture_error=reason;calibrated=false;lap_timer_stop(&timer);lap_timer_invalidate(&timer);}
}
/* Return holding node_lock. Reserve RF across yields; each request has its
 * own 100ms READY budget. Stop/pause may cancel, never continue stale IO. */
static esp_err_t game_prepare_yield(uint32_t session,uint8_t state,unsigned slot){
 node_spi_slot_stats_t before,after;
 if(!node_spi_slot_stats(slot,&before))return ESP_ERR_INVALID_STATE;
 xSemaphoreGive(node_lock);vTaskDelay(1);xSemaphoreTake(node_lock,portMAX_DELAY);
 if(!node_spi_slot_stats(slot,&after) || !node_prepare_unchanged(session,state,before.mutation_revision,
    game.session,game.state,after.mutation_revision))return ESP_ERR_INVALID_STATE;
 return ESP_OK;
}
static esp_err_t flag_prepare(void){
 uint8_t mask=game_receiver_mask(game_session_config(&game)),state=game.state;
 uint32_t session=game.session;esp_err_t result=ESP_OK;game_preparing=true;
 memset(&flag_signal,0,sizeof(flag_signal));flag_scan.pending=false;
 for(unsigned slot=0;slot<RECEIVER_SLOTS;slot++)if(mask&(1u<<slot)){
  uint8_t stop[2]={0,0};
  result=node_spi_exchange_until(slot,3,NULL,0,8,(uint8_t*)&flag_original[slot],esp_timer_get_time()+100000);
  if(result)break;
  flag_have_original[slot]=true;
  result=game_prepare_yield(session,state,slot);if(result)break;
  result=node_spi_exchange_until(slot,8,stop,2,0,NULL,esp_timer_get_time()+100000);if(result)break;
  receivers.slots[slot].calibrated=false;if(!slot)calibrated=false;
  result=game_prepare_yield(session,state,slot);if(result)break;
 }
 if(result)flag_restore_request();
 game_preparing=false;return result;
}
/* Keep the same reservation as signal preparation. Publish each successful
 * start before yielding, and retain STOP obligations on any partial failure. */
static esp_err_t game_lap_prepare(bool resume){
 uint8_t mask=game_receiver_mask(game_session_config(&game)),state=game.state;
 uint32_t session=game.session;esp_err_t result=ESP_OK;game_preparing=true;
 game_receiver_bind(&game_receivers,&receivers);
 for(unsigned slot=0;slot<RECEIVER_SLOTS;slot++)if(mask&(1u<<slot)){
  int64_t deadline=esp_timer_get_time()+100000;
  if(!slot){
   uint8_t input[2]={1,1};
   result=node_spi_exchange_until(slot,8,input,2,0,NULL,deadline);
   receivers.slots[0].capture_error=result;
   if(result)receivers.slots[0].stop_pending=true;
  }else {
   int32_t e=receiver_capture_action(&receivers,slot,2,additional_slot_exchange,&deadline);
   result=e==-1?ESP_ERR_INVALID_STATE:e;
  }
  if(result)break;
  result=game_prepare_yield(session,state,slot);if(result)break;
 }
 if(result)game_stop_request(result);
 else if(mask&1){
  timer.minimum_lap_us=game_session_config(&game)->minimum_lap_ms*1000u;
  if(!resume)lap_timer_start(&timer);else timer.running=true;
 }
 game_preparing=false;return result;
}
static void game_receiver_fault(uint8_t fault){
    if(game.state!=GAME_RUNNING || game_session_config(&game)->mode!=GAME_LAPS)return;
    game_pause(&game,fault,esp_timer_get_time());
    uint8_t mask=game_receiver_mask(game_session_config(&game));
    receiver_request_stop(&receivers,mask,fault);
    if(mask&1){timer.running=false;lap_timer_invalidate(&timer);event_capture_active=false;event_capture_error=fault;calibrated=false;}
    taskENTER_CRITICAL(&state_lock);snapshot.running=0;snapshot.calibrated=0;taskEXIT_CRITICAL(&state_lock);
}
static esp_err_t game_action(uint8_t action,uint32_t session)
{
    if(action>3)return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t err=ESP_OK;uint64_t now=esp_timer_get_time();
    if(action==1) {
        err=game_ready(false);
        if(err==ESP_OK && !game_start(&game,now,game_receiver_mask(&game.config)))err=ESP_ERR_INVALID_STATE;
        if(err==ESP_OK && session)game.session=session;
        if(err==ESP_OK && game.config.mode==GAME_LAPS){
            err=game_lap_prepare(false);
        }
        if(err==ESP_OK && game.config.mode!=GAME_LAPS){
            err=flag_prepare();
        }
        if(err!=ESP_OK && game.state==GAME_RUNNING)game_stop(&game,now);
    } else if(action==2) {
        err=session && game.session!=session?ESP_ERR_INVALID_STATE:game_ready(true);
        if(err==ESP_OK){
            if(game.config.mode==GAME_LAPS){
                err=game_lap_prepare(true);
            }
            if(err==ESP_OK && game.config.mode!=GAME_LAPS)err=flag_prepare();
            if(err==ESP_OK){game_resume(&game,esp_timer_get_time());if(game.config.mode==GAME_LAPS && (game_receiver_mask(&game.config)&1))timer.running=true;}
            memset(&flag_signal,0,sizeof(flag_signal));
        }
    } else if(action==3) {
        if(game.state!=GAME_RUNNING)err=ESP_ERR_INVALID_STATE;
        else {game_tick(&game,now);game_pause(&game,0,now);if(game.config.mode==GAME_LAPS){
            timer.running=false;lap_timer_invalidate(&timer);
            receivers.slots[0].calibrated=calibrated;
            receiver_request_pause(&receivers,game_receiver_mask(game_session_config(&game)));
        }}
    } else {
        if(!game_busy())err=ESP_ERR_INVALID_STATE;
        else {
            game_tick(&game,now);game_stop(&game,now);
            if(game.config.mode==GAME_LAPS){game_stop_request(0);save_results();}
            else flag_restore_request();
            esp_err_t saved=game_save_result();if(err==ESP_OK)err=saved;
        }
    }
    taskENTER_CRITICAL(&state_lock);snapshot.running=event_capture_active || game.state==GAME_RUNNING;snapshot.calibrated=calibrated;taskEXIT_CRITICAL(&state_lock);
    xSemaphoreGive(node_lock);return err;
}

esp_err_t controller_game_action(uint8_t action){return game_action(action,0);}
esp_err_t controller_game_start_session(uint32_t session){return session?game_action(1,session):ESP_ERR_INVALID_ARG;}
esp_err_t controller_game_resume_session(uint32_t session){return session?game_action(2,session):ESP_ERR_INVALID_ARG;}
esp_err_t controller_game_fault_pause(uint8_t fault){
    return controller_game_fault_pause_session(fault,0);
}
esp_err_t controller_game_fault_pause_session(uint8_t fault,uint32_t session){
    if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE)return ESP_ERR_TIMEOUT;
    esp_err_t error=ESP_OK;
    if(session && (game.session!=session || game.state!=GAME_RUNNING)){
        xSemaphoreGive(node_lock);return ESP_ERR_INVALID_STATE;
    }
    if(game.state==GAME_RUNNING){
        game_pause(&game,fault,esp_timer_get_time());timer.running=false;lap_timer_invalidate(&timer);
        if(game.config.mode==GAME_LAPS){uint8_t mask=game_receiver_mask(game_session_config(&game));receiver_request_stop(&receivers,mask,fault);if(mask&1){event_capture_active=false;event_capture_error=fault;}}
        taskENTER_CRITICAL(&state_lock);snapshot.running=0;snapshot.calibrated=0;taskEXIT_CRITICAL(&state_lock);calibrated=false;
    }
    xSemaphoreGive(node_lock);return error;
}

/* Caller holds node_lock. Cancellation is not complete until C5 reports a
 * terminal scan state; never release RF ownership on a missing ACK. */
static esp_err_t scan_cleanup(void){
    int64_t deadline=esp_timer_get_time()+100000;bool done=false;
    esp_err_t e=receiver_scan_stop_step(&scan_stop_step,additional_slot_exchange,&deadline,0,ESP_ERR_INVALID_STATE,ESP_ERR_INVALID_RESPONSE,&done);
    receivers.slots[0].config_error=e;
    if(done){scan_owned=false;scan_cancel_pending=false;flag_restore_request();}
    return e;
}

esp_err_t controller_scan(void)
{
    if (!node_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(node_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (additional_capture_busy() || snapshot.auto_gain_state == 1 || game_busy() || primary_capture_busy() ||
        !snapshot.node_online || !snapshot.rf_valid || timer.running) {
        xSemaphoreGive(node_lock);return ESP_ERR_INVALID_STATE;
    }
    uint16_t power[64]={0};uint8_t range[2]={0,64},status[12]={0};
    node_config_t original;node_info_t identity;
    esp_err_t err=node_spi_get_config(&original);
    if(!err)err=node_spi_get_info(&identity);
    if(err){xSemaphoreGive(node_lock);return err;}
    scan_owned=true;scan_cancel_pending=false;scan_stop_step.status_due=false;calibrated=false;
    flag_original[0]=original;flag_have_original[0]=true;receivers.slots[0].restore_pending=true;
    taskENTER_CRITICAL(&state_lock);snapshot.scan_valid=0;snapshot.calibrated=0;taskEXIT_CRITICAL(&state_lock);
    err=node_spi_exchange(NODE_CMD_SCAN_START,range,2,0,NULL);
    int64_t deadline=esp_timer_get_time()+20000000;uint32_t generation=0;
    while(!err){
        xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(100));xSemaphoreTake(node_lock,portMAX_DELAY);
        if(esp_timer_get_time()>=deadline){err=ESP_ERR_TIMEOUT;break;}
        err=node_spi_exchange(NODE_CMD_SCAN_STATUS,NULL,0,12,status);
        if(err)break;
        if(!receiver_scan_status(status,&generation)){err=ESP_ERR_INVALID_RESPONSE;break;}
        if(status[0]==1)continue;
        if(status[0]!=2 || status[1]!=64){err=ESP_FAIL;break;}
        for(uint8_t channel=0;!err && channel<64;channel++){
            uint8_t result[8];err=node_spi_exchange(NODE_CMD_SCAN_RESULT,&channel,1,8,result);
            if(!err && (result[0]!=channel || result[1]!=1))err=ESP_ERR_INVALID_RESPONSE;
            if(!err)power[channel]=(uint16_t)result[4]|((uint16_t)result[5]<<8);
            xSemaphoreGive(node_lock);taskYIELD();xSemaphoreTake(node_lock,portMAX_DELAY);
        }
        node_info_t after;
        if(!err)err=node_spi_get_info(&after);
        if(!err && (after.session!=identity.session || !after.rf_valid || after.faults))err=ESP_ERR_INVALID_STATE;
        break;
    }
    scan_cancel_pending=true;
    esp_err_t cleanup=scan_cleanup();
    /* ACK and terminal read have separate lock ownership. The poller may also
     * finish cancellation while this command yields. */
    if(!cleanup && scan_owned){
        xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));xSemaphoreTake(node_lock,portMAX_DELAY);
        if(scan_owned)cleanup=scan_cleanup();
    }
    if(!cleanup && scan_owned)cleanup=ESP_ERR_INVALID_STATE;
    if(!err)err=cleanup;
    if(!err){taskENTER_CRITICAL(&state_lock);memcpy(snapshot.scan_power,power,sizeof(power));snapshot.scan_valid=1;taskEXIT_CRITICAL(&state_lock);}
    xSemaphoreGive(node_lock);return err;
}

/* The search runs outside the BLE callback. It owns node_lock so no scan,
 * configuration or race can interleave with trial gains. Trials never touch NVS. */
esp_err_t controller_auto_gain(bool start)
{
    taskENTER_CRITICAL(&state_lock);
    if (!start) {
        auto_gain_cancel = true;
        taskEXIT_CRITICAL(&state_lock);
        return ESP_OK;
    }
    taskEXIT_CRITICAL(&state_lock);
    if (xSemaphoreTake(node_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = snapshot.node_online && snapshot.rf_valid && !timer.running &&
        snapshot.auto_gain_state != 1 && !game_busy() && !primary_capture_busy() && !additional_capture_busy() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&state_lock);
        auto_gain_cancel = false;
        snapshot.auto_gain_state = 1;
        snapshot.auto_gain_progress = 0;
        taskEXIT_CRITICAL(&state_lock);
        if (xTaskCreate(auto_gain_task, "auto_gain", 4096, NULL, 3, NULL) != pdPASS) {
            taskENTER_CRITICAL(&state_lock); snapshot.auto_gain_state = 3; taskEXIT_CRITICAL(&state_lock);
            err = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(node_lock);
    return err;
}

static bool gain_cancelled(void)
{
    taskENTER_CRITICAL(&state_lock); bool result = auto_gain_cancel; taskEXIT_CRITICAL(&state_lock);
    return result;
}

/* Successful admission retains node_lock until the caller finishes storage.
 * Lock order is network control_lock -> node_lock; never call network control
 * or another controller API while this lease is held. */
esp_err_t controller_maintenance_begin(void){
    if(!node_lock || xSemaphoreTake(node_lock,pdMS_TO_TICKS(100))!=pdTRUE)return ESP_ERR_TIMEOUT;
    if(game_preparing || additional_capture_busy() || primary_capture_busy() || timer.running || game_busy() || snapshot.auto_gain_state==1){
        xSemaphoreGive(node_lock);return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
esp_err_t controller_finder(bool enabled,uint8_t channel){
 if(channel>=64)return ESP_ERR_INVALID_ARG;
 if(!enabled){
  if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
  __atomic_store_n(&finder_active,false,__ATOMIC_RELAXED);xSemaphoreGive(node_lock);return ESP_OK;
 }
 esp_err_t e=controller_maintenance_begin();if(e)return e;
 if(!snapshot.node_online || !snapshot.rf_valid || snapshot.channel!=channel)e=ESP_ERR_INVALID_STATE;
 else __atomic_store_n(&finder_active,true,__ATOMIC_RELAXED);
 controller_maintenance_end();return e;
}
void controller_maintenance_end(void){xSemaphoreGive(node_lock);}
esp_err_t controller_maintenance_ready(void){
    esp_err_t e=controller_maintenance_begin();
    if(e==ESP_OK)controller_maintenance_end();
    return e;
}

/* The worker owns RF via auto_gain_state, not by holding the mutex while
 * sleeping. Read-only control and other receivers remain serviceable. */
static void gain_wait(unsigned milliseconds){
    xSemaphoreGive(node_lock);
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
    xSemaphoreTake(node_lock,portMAX_DELAY);
}
static esp_err_t gain_exchange(uint8_t command,const void *input,uint8_t length,uint8_t expected,void *output){
 return node_spi_exchange_until(0,command,input,length,expected,output,esp_timer_get_time()+100000);
}
static esp_err_t gain_info(node_info_t *info){
 uint8_t data[17];esp_err_t e=gain_exchange(1,NULL,0,17,data);
 if(!e){memcpy(info->mac,data,6);info->session=receiver_u32(data+6);info->channel=data[10];info->rf_valid=data[11];info->faults=data[12];info->transactions=receiver_u32(data+13);}
 return e;
}
static esp_err_t gain_config_get(node_config_t *config){return gain_exchange(3,NULL,0,8,config);}
static esp_err_t gain_config_set(const node_config_t *config){return gain_exchange(4,config,8,0,NULL);}
static esp_err_t gain_measure(uint16_t *power, uint16_t *clip,uint32_t session)
{
    uint32_t sum = 0; *clip = 0;
    for (unsigned i = 0; i < 6; ++i) {
        if (gain_cancelled()) return ESP_ERR_INVALID_STATE;
        gain_wait(100);
        if(gain_cancelled())return ESP_ERR_INVALID_STATE;
        node_info_t current;
        esp_err_t probe=gain_info(&current);
        if(probe!=ESP_OK)return probe;
        if(current.session!=session || !current.rf_valid || current.faults)return ESP_ERR_INVALID_STATE;
        node_observation_t obs={0};uint8_t data[22];
        esp_err_t err = gain_exchange(12,NULL,0,22,data);
        if(!err){
            if(receiver_u32(data+16)!=session || data[21]!=current.channel || (data[20]&(NODE_QUALITY_SETTLING|NODE_QUALITY_NO_SAMPLE|NODE_QUALITY_STALE)))return ESP_ERR_INVALID_RESPONSE;
            obs.power=(uint16_t)data[8]|((uint16_t)data[9]<<8);obs.clip_pm=(uint16_t)data[10]|((uint16_t)data[11]<<8);obs.sample_count=receiver_u32(data+12);
        }
        if (err != ESP_OK || !obs.sample_count) return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        sum += obs.power;
        if (obs.clip_pm > *clip) *clip = obs.clip_pm;
        taskENTER_CRITICAL(&state_lock);
        snapshot.observed_power = obs.power; snapshot.observed_clip_pm = obs.clip_pm;
        snapshot.observation_updated_us=esp_timer_get_time();snapshot.observation_samples=obs.sample_count;snapshot.observation_quality=data[20];
        taskEXIT_CRITICAL(&state_lock);
    }
    *power = sum / 6;
    return ESP_OK;
}

static void auto_gain_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(node_lock, portMAX_DELAY);
    node_config_t original = {0}, trial, confirmed;
    node_info_t info;
    uint32_t session = 0;
    bool have_original = false;
    uint8_t result = 3; /* 2 locked; 3 no usable signal; 4 cancelled; 5 link error. */
    uint16_t best_power = 0, power = 0, clip = 0;
    int best = -1;
    esp_err_t err = gain_config_get(&original);
    have_original = err == ESP_OK;
    if (err == ESP_OK) err = gain_info(&info);
    if (err == ESP_OK) session = info.session;
    if (err == ESP_OK) err = gain_exchange(8,(const uint8_t[]){0,0},2,0,NULL);
    calibrated = false;
    taskENTER_CRITICAL(&state_lock); snapshot.calibrated = 0; taskEXIT_CRITICAL(&state_lock);
    if (err == ESP_OK) err = gain_measure(&power, &clip,session);
    /* Do not increase gain until noise alone looks like a transmitter. */
    if (err == ESP_OK && power >= 3) {
        for (unsigned step = 0; step < 17 && err == ESP_OK; ++step) {
            if (gain_cancelled()) break;
            trial = original; trial.gain = step == 16 ? 63 : step * 4;
            err = gain_config_set(&trial);
            if (err != ESP_OK) break;
            taskENTER_CRITICAL(&state_lock);
            snapshot.gain = trial.gain; snapshot.auto_gain_progress = step * 100 / 17;
            taskEXIT_CRITICAL(&state_lock);
            gain_wait(150);
            err = gain_measure(&power, &clip,session);
            ESP_LOGI(TAG, "AUTO_GAIN gain=%u power=%u clip=%u result=%s", trial.gain, power, clip, esp_err_to_name(err));
            if (err == ESP_OK && clip < 50 && power >= 3 && power > best_power) {
                best_power = power; best = trial.gain;
            }
        }
        if (err == ESP_OK && best >= 0 && !gain_cancelled()) {
            trial = original; trial.gain = best;
            err = gain_config_set(&trial);
            if (err == ESP_OK) { gain_wait(150); err = gain_measure(&power, &clip,session); }
            if (err == ESP_OK) err = gain_info(&info);
            if (err == ESP_OK && info.session != session) err = ESP_ERR_INVALID_STATE;
            if (err == ESP_OK) err = gain_config_get(&confirmed);
            if (err == ESP_OK && memcmp(&confirmed,&trial,sizeof(trial))) err = ESP_ERR_INVALID_RESPONSE;
            if (err == ESP_OK && power >= 3 && clip < 50 && !gain_cancelled()) {
                saved_config = trial; have_saved_config = true;
                esp_err_t save_error = save_config(&trial);
                taskENTER_CRITICAL(&state_lock);
                snapshot.config_save_error = save_error;
                taskEXIT_CRITICAL(&state_lock);
                result = 2; /* Applied gain; persistence status is reported separately. */
            }
        }
    }
    if (result != 2) {
        result = gain_cancelled() ? 4 : err == ESP_OK ? 3 : 5;
        esp_err_t restore=ESP_OK;
        if(have_original){restore=gain_config_set(&original);if(!restore)restore=gain_config_get(&confirmed);if(!restore && memcmp(&confirmed,&original,sizeof(original)))restore=ESP_ERR_INVALID_RESPONSE;}
        if (have_original && restore != ESP_OK) {
            result=5;flag_original[0]=original;flag_have_original[0]=true;receivers.slots[0].config_error=restore;receivers.slots[0].restore_pending=true;
        }
    }
    taskENTER_CRITICAL(&state_lock);
    snapshot.gain = result == 2 ? trial.gain : original.gain;
    snapshot.auto_gain_state = result;
    snapshot.auto_gain_progress = result == 2 ? 100 : snapshot.auto_gain_progress;
    snapshot.scan_valid = 0;
    taskEXIT_CRITICAL(&state_lock);
    ESP_LOGI(TAG, "AUTO_GAIN DONE state=%u gain=%u", result, result == 2 ? trial.gain : original.gain);
    xSemaphoreGive(node_lock);
    vTaskDelete(NULL);
}

void controller_get_snapshot(controller_snapshot_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&state_lock);
    *out = snapshot;
    taskEXIT_CRITICAL(&state_lock);
}

/* Yield between health reads while preserving the all-or-discard sweep.
 * Caller holds node_lock on entry AND return. Event reads/ACKs can run in
 * between; any attempted RF mutation or business transition restarts reads. */
static bool poll_read_yield(void){
    node_spi_slot_stats_t before,after;
    if(!node_spi_slot_stats(0,&before))return false;
    node_poll_guard_t a={before.mutation_revision,game.session,game.state,event_capture_active,receivers.slots[0].stop_pending,scan_owned || game_preparing || eventwatch_owned || protocol_owned || clocktest_owned || snapshot.auto_gain_state==1};
    xSemaphoreGive(node_lock);vTaskDelay(1);xSemaphoreTake(node_lock,portMAX_DELAY);
    if(!node_spi_slot_stats(0,&after))return false;
    node_poll_guard_t b={after.mutation_revision,game.session,game.state,event_capture_active,receivers.slots[0].stop_pending,scan_owned || game_preparing || eventwatch_owned || protocol_owned || clocktest_owned || snapshot.auto_gain_state==1};
    bool unchanged=node_poll_unchanged(a,b);
    if(!unchanged)++poll_discarded;
    return unchanged;
}
static void poll_task(void *arg)
{
    (void)arg;
    node_info_t info={0};
    node_rf_stats_t rf;
    uint32_t previous_session = 0;
    uint32_t failed_polls = 0;
    uint32_t successful_polls = 0;
    uint32_t previous_overflow = 0;
    uint32_t configured_session = 0;
    receiver_poll_step_t reconnect={0};uint64_t reconnect_revision=0;
    int64_t next_health_log_us = esp_timer_get_time() + 10000000;
    for (;;) {
        if (xSemaphoreTake(node_lock, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if(game_preparing || eventwatch_owned || protocol_owned || clocktest_owned){xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(scan_owned){if(scan_cancel_pending)scan_cleanup();xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(snapshot.auto_gain_state==1){xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(game.state!=GAME_RUNNING && flag_have_original[0]){xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(primary_reconnecting){
            node_spi_slot_stats_t stats;bool done=false;
            if(node_spi_slot_stats(0,&stats) && stats.mutation_revision!=reconnect_revision)reconnect.phase=RP_INFO;
            int64_t deadline=esp_timer_get_time()+100000;
            esp_err_t e=receiver_poll_step(&receivers,0,&reconnect,additional_slot_exchange,&deadline,esp_timer_get_time(),&done);
            if(node_spi_slot_stats(0,&stats))reconnect_revision=stats.mutation_revision;
            if(done && !e){
                configured_session=receivers.slots[0].session;previous_session=configured_session;
                receivers.slots[0].restore_pending=false;primary_reconnecting=false;previous_overflow=0;
                node_config_t *applied=&receivers.slots[0].observed;
                taskENTER_CRITICAL(&state_lock);
                snapshot.channel=applied->channel;snapshot.gain=applied->gain;
                snapshot.enter_power=applied->enter_power;snapshot.exit_power=applied->exit_power;snapshot.confirm_ms=applied->confirm_ms;
                snapshot.calibrated=0;
                taskEXIT_CRITICAL(&state_lock);
                ESP_LOGI(TAG,"RECONNECT session=%"PRIu32" verified=1",configured_session);
            }
            if(e){receivers.slots[0].config_error=e;taskENTER_CRITICAL(&state_lock);++snapshot.spi_errors;taskEXIT_CRITICAL(&state_lock);}
            xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(e?1000:40));continue;
        }
        if(receivers.slots[0].stop_pending){xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));continue;}
        /* After a failed probe, an absent READY is sufficient to keep the
         * node offline. Do not hold the shared control lock for another
         * two-second SPI timeout: game configuration and ESP-NOW must remain
         * available on a controller with no C5 attached. */
        esp_err_t error = failed_polls && !node_spi_ready()
            ? ESP_ERR_TIMEOUT : node_spi_get_info(&info);
        if (error == ESP_OK && info.session != configured_session) {
            primary_reconnecting=true;reconnect.phase=RP_INFO;
            receiver_slot_t *r=&receivers.slots[0];r->restore_pending=r->stop_pending=true;
            r->online=r->rf_valid=r->active=r->calibrated=false;r->clock.valid=false;
            if(have_saved_config)receiver_desire(&receivers,0,&saved_config);
            if(event_capture_active){event_capture_active=false;event_capture_error=4;}
            calibrated=false;lap_timer_invalidate(&timer);
            if(game.state==GAME_RUNNING){
                if(game_session_config(&game)->mode==GAME_LAPS)game_receiver_fault(4);
                else game_pause(&game,4,esp_timer_get_time());
            }
            taskENTER_CRITICAL(&state_lock);snapshot.node_online=snapshot.rf_valid=snapshot.running=snapshot.calibrated=0;taskEXIT_CRITICAL(&state_lock);
            xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));continue;
        }
        if(error==ESP_OK && !poll_read_yield()){xSemaphoreGive(node_lock);continue;}
        if (error == ESP_OK) error = node_spi_get_rf_stats(&rf);
        node_observation_t obs = {0};
        if(error==ESP_OK && !poll_read_yield()){xSemaphoreGive(node_lock);continue;}
        if (error == ESP_OK) error = node_spi_get_observation(&obs);
        if (error == ESP_OK && obs.active && ((!timer.running && !event_capture_active) || game.state==GAME_PAUSED)) {
            receiver_request_stop(&receivers,1,ESP_ERR_INVALID_STATE);calibrated=false;
            xSemaphoreGive(node_lock);vTaskDelay(pdMS_TO_TICKS(40));continue;
        }
        node_config_t cfg = {0};
        if(error==ESP_OK && !poll_read_yield()){xSemaphoreGive(node_lock);continue;}
        if (error == ESP_OK) error = node_spi_get_config(&cfg);
        node_observation_detail_t detail = {0};
        if(error==ESP_OK && !poll_read_yield()){xSemaphoreGive(node_lock);continue;}
        if (error == ESP_OK) error = node_spi_get_observation_detail(&detail);
        if (error == ESP_OK && detail.session != info.session) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK && detail.channel != cfg.channel) error = ESP_ERR_INVALID_RESPONSE;
        if (error == ESP_OK) info.channel=cfg.channel;
        if (error == ESP_OK && previous_session && previous_session != info.session) {
            lap_timer_invalidate(&timer);
            calibrated = false;
            if(game.state==GAME_RUNNING){if(game.config.mode==GAME_LAPS && (game_receiver_mask(game_session_config(&game))&1))game_receiver_fault(4);else if(game.config.mode!=GAME_LAPS)game_pause(&game,4,esp_timer_get_time());}
        }
        if (error == ESP_OK) previous_session = info.session;
        receiver_health(&receivers,0,error,info.session,error==ESP_OK && info.rf_valid,error==ESP_OK && obs.calibrated,error==ESP_OK && obs.active,esp_timer_get_time());
        if(error==ESP_OK){receivers.slots[0].observed=cfg;receivers.slots[0].have_observed=true;
            if(!receiver_config_pending(&receivers.slots[0]))receivers.slots[0].config_error=0;
        }
        if(have_saved_config)receiver_desire(&receivers,0,&saved_config);
        if(error==ESP_OK && (!node_time.valid || node_time.session!=info.session || esp_timer_get_time()-node_time.sampled_us>=1000000)){
            node_clock_t clock;uint64_t start=esp_timer_get_time();esp_err_t clock_error=node_spi_get_clock(&clock);uint64_t end=esp_timer_get_time();
            if(clock_error==ESP_OK && clock.session==info.session)node_time_sample(&node_time,clock.session,clock.timestamp_us,start,end);
            else node_time.valid=false;
        }
        if(error!=ESP_OK)node_time.valid=false;
        if(event_capture_active){
            if(error!=ESP_OK || !info.rf_valid || info.faults || !obs.active){event_capture_active=false;receivers.slots[0].stop_pending=true;event_capture_error=3;}
            else if(event_capture_baseline && obs.overflow_count!=event_capture_overflow){event_capture_active=false;receiver_request_stop(&receivers,1,5);event_capture_error=5;calibrated=false;}
            else {event_capture_overflow=obs.overflow_count;event_capture_baseline=true;}
        }
        if (error == ESP_OK && ((timer.running && game.state==GAME_RUNNING) || event_capture_active)) {
            if (timer.running && (obs.overflow_count != previous_overflow || !obs.active)) {
                game_receiver_fault(5);
            }
            previous_overflow = obs.overflow_count;
            /* Queue work runs one event per receiver-worker turn, independently
             * of the 200ms health snapshot and without a 16-event lock burst. */
        }

        if(game.state==GAME_RUNNING && game.config.mode==GAME_LAPS && (game_receiver_mask(game_session_config(&game))&1) && (error!=ESP_OK || !obs.active || !info.rf_valid || info.faults)) {
            game_receiver_fault(3);
        }
        if(error!=ESP_OK && event_capture_active){event_capture_active=false;receivers.slots[0].stop_pending=true;event_capture_error=3;}
        /* Errors discovered while draining events or switching channels must
         * invalidate the receiver cache too, not just early health probes. */
        if(error!=ESP_OK){receiver_slot_t *r=&receivers.slots[0];if(obs.active || timer.running)r->stop_pending=true;if(!r->last_error)++r->errors;r->last_error=error;r->online=r->rf_valid=r->calibrated=r->active=false;r->clock.valid=false;}
        receivers.slots[0].capture_requested=event_capture_active;
        receivers.slots[0].capture_error=event_capture_error;
        uint32_t recovered_polls = error == ESP_OK ? failed_polls : 0;
        taskENTER_CRITICAL(&state_lock);
        if (error == ESP_OK) {
            failed_polls = 0;
            snapshot.node_online = 1;
            snapshot.capabilities = 7; /* Timing + automatic gain + device detail. */
            snapshot.rf_valid = info.rf_valid;
            snapshot.boot_session = info.session;
            snapshot.windows = rf.windows;
            snapshot.valid_windows = rf.valid_windows;
            snapshot.receive_errors = rf.receive_errors;
            snapshot.raw_power = rf.raw_power;
            snapshot.clip_pm = rf.clip_pm;
            snapshot.observed_power = detail.power;
            snapshot.observed_clip_pm = detail.clip_pm;
            snapshot.observation_quality = detail.quality_flags;
            snapshot.observation_samples = detail.sample_count;
            snapshot.observation_updated_us = esp_timer_get_time();
            snapshot.event_overflows = obs.overflow_count;
            snapshot.channel = info.channel;
            snapshot.gain = cfg.gain;
            snapshot.enter_power = cfg.enter_power;
            snapshot.exit_power = cfg.exit_power;
            snapshot.confirm_ms = cfg.confirm_ms;
            snapshot.running = event_capture_active || game.state==GAME_RUNNING;
            snapshot.calibrated = obs.calibrated && !receivers.slots[0].stop_pending;
            calibrated = snapshot.calibrated;
        } else {
            snapshot.node_online = snapshot.rf_valid = 0;
            snapshot.observation_quality |= NODE_QUALITY_STALE;
            snapshot.calibrated = 0;
            calibrated = false;
            ++snapshot.spi_errors;
            ++failed_polls;
        }
        taskEXIT_CRITICAL(&state_lock);
        if (error != ESP_OK) {
            if (timer.running && game.state!=GAME_PAUSED) {
                lap_timer_stop(&timer);
                save_results();
                taskENTER_CRITICAL(&state_lock);
                snapshot.running = 0;
                taskEXIT_CRITICAL(&state_lock);
            }
            lap_timer_invalidate(&timer);
        }
        if(error==ESP_OK)++poll_completed;
        xSemaphoreGive(node_lock);
        if (recovered_polls)
            ESP_LOGI(TAG, "C5 link recovered after %" PRIu32 " failed polls", recovered_polls);
        if (error != ESP_OK && (failed_polls == 1 || failed_polls % 25 == 0))
            ESP_LOGW(TAG, "C5 unavailable: %s (%" PRIu32 " failed polls)",
                     esp_err_to_name(error), failed_polls);
        if (error == ESP_OK) ++successful_polls;
        if (esp_timer_get_time() >= next_health_log_us) {
            controller_snapshot_t health;
            controller_get_snapshot(&health);
            ESP_LOGI(TAG, "LINK online=%u rf_valid=%u ok_polls=%" PRIu32
                     " spi_errors=%" PRIu32 " windows=%" PRIu32
                     " valid=%" PRIu32 " rx_errors=%" PRIu32,
                     health.node_online, health.rf_valid, successful_polls,
                     health.spi_errors, health.windows, health.valid_windows,
                     health.receive_errors);
            next_health_log_us += 10000000;
        }
        vTaskDelay(pdMS_TO_TICKS(error == ESP_OK ? 200 : 1000));
    }
}

static int32_t additional_slot_exchange(void *ctx,unsigned slot,uint8_t command,const uint8_t *input,uint8_t length,uint8_t expected,uint8_t *out){
 if(ctx)return node_spi_exchange_until(slot,command,input,length,expected,out,*(const int64_t*)ctx);
 return node_spi_exchange_slot(slot,command,input,length,expected,out);
}
static uint64_t additional_slot_now(void *ctx){(void)ctx;return esp_timer_get_time();}
/* One bounded exchange per step; the settle interval owns no mutex. */
static void flag_scan_step(void){
 bool running=game.state==GAME_RUNNING && game.match_config.mode!=GAME_LAPS;
 if(game_preparing)return;
 if(!running){flag_scan.pending=false;return;}
 uint64_t now=esp_timer_get_time();
 if(flag_scan.pending && flag_scan.session!=game.session)flag_scan.pending=false;
 if(!flag_scan.pending){
  unsigned pilot;if(!game_signal_next(&flag_signal,&game.match_config,&pilot))return;
  unsigned slot=game.match_config.pilots[pilot].slot;receiver_slot_t *r=&receivers.slots[slot];
  node_config_t trial=flag_original[slot];trial.channel=game.match_config.pilots[pilot].channel;
  int64_t deadline=now+100000;
  esp_err_t e=flag_have_original[slot] && r->enabled && r->online && r->rf_valid && !r->stop_pending?ESP_OK:ESP_ERR_INVALID_STATE;
  if(!e)e=node_spi_exchange_until(slot,4,(const uint8_t*)&trial,8,0,NULL,deadline);
  if(e){uint8_t empty[22]={0};game_signal_record(&flag_signal,pilot,game.session,empty,GAME_SIGNAL_TRANSPORT,GAME_SIGNAL_IO,0,e,esp_timer_get_time());r->config_error=e;game_pause(&game,3,esp_timer_get_time());return;}
  r->observed=trial;r->have_observed=true;game_signal_scan_begin(&flag_scan,game.session,pilot,esp_timer_get_time());return;
 }
 if(!game_signal_scan_due(&flag_scan,game.session,running,now))return;
 unsigned pilot=flag_scan.pilot,slot=game.match_config.pilots[pilot].slot;receiver_slot_t *r=&receivers.slots[slot];
 uint8_t measured[22]={0};int64_t deadline=now+100000;
 esp_err_t e=node_spi_exchange_until(slot,12,NULL,0,22,measured,deadline);
 if(!e && (measured[20]&(NODE_QUALITY_SETTLING|NODE_QUALITY_NO_SAMPLE|NODE_QUALITY_STALE)) && game_signal_scan_retry(&flag_scan,esp_timer_get_time())){
  game_signal_record(&flag_signal,pilot,game.session,measured,GAME_SIGNAL_RETRY,game_signal_reason(&game,pilot,&r->observed,r->session,measured),flag_scan.attempts,0,esp_timer_get_time());return;
 }
 flag_scan.pending=false;
 if(e){game_signal_record(&flag_signal,pilot,game.session,measured,GAME_SIGNAL_TRANSPORT,GAME_SIGNAL_IO,flag_scan.attempts,e,esp_timer_get_time());r->config_error=e;game_pause(&game,3,esp_timer_get_time());}
 else {
  bool valid=game_signal_sample(&flag_signal,&game,pilot,&r->observed,r->session,measured,esp_timer_get_time());
  if(!valid)ESP_LOGW(TAG,"SIGNAL rejected pilot=%u slot=%u channel=%u power=%u clip=%u quality=0x%02x reason=0x%02x retries=%u",pilot,slot,measured[21],measured[8]|((unsigned)measured[9]<<8),measured[10]|((unsigned)measured[11]<<8),measured[20],flag_signal.detail[pilot].reason,flag_scan.attempts);
  game_tick(&game,esp_timer_get_time());
 }
 if(game.state==GAME_FINISHED)game_save_result();
}
static void additional_slots_task(void *arg){
 (void)arg;uint64_t next[RECEIVER_SLOTS]={0};uint8_t event_next=0;
 receiver_poll_step_t polls[RECEIVER_SLOTS]={0};uint64_t revisions[RECEIVER_SLOTS]={0};
 bool clock_due[RECEIVER_SLOTS]={0};
 uint8_t restore_next=0;uint64_t restore_due[RECEIVER_SLOTS]={0},restore_revisions[RECEIVER_SLOTS]={0};
 uint8_t stop_next=0;uint64_t stop_due[RECEIVER_SLOTS]={0};
 for(;;){
  if(xSemaphoreTake(node_lock,0)==pdTRUE){
   uint8_t excluded=(primary_reconnecting || eventwatch_owned || protocol_owned || clocktest_owned)?1:0;
   /* Secondary poll owns its stop phase. A second worker issuing STOP
    * between INFO and RP_STOP changes the revision, restarts INFO and can
    * prevent discovery from ever reaching its final health commit. Pause
    * with retained calibration still uses this dedicated stop path. */
   for(unsigned slot=1;slot<RECEIVER_SLOTS;slot++)if(!receivers.slots[slot].stop_keep_calibrated)excluded|=1u<<slot;
   for(unsigned slot=0;slot<RECEIVER_SLOTS;slot++)if(flag_have_original[slot] || esp_timer_get_time()<stop_due[slot])excluded|=1u<<slot;
   int selected=receiver_pending_stop(&receivers,&stop_next,excluded);
   if(selected>=0){
    unsigned slot=(unsigned)selected;
    bool keep=receivers.slots[slot].stop_keep_calibrated && receivers.slots[slot].online && receivers.slots[slot].rf_valid;
    uint8_t stop[2]={0,keep};int64_t deadline=esp_timer_get_time()+100000;
    esp_err_t e=node_spi_exchange_until(slot,8,stop,2,0,NULL,deadline);
    receiver_stop_result(&receivers.slots[slot],e);
    receivers.slots[slot].calibrated=!e && keep;
    if(!slot){event_capture_active=false;event_capture_error=e;calibrated=!e && keep;}
    stop_due[slot]=esp_timer_get_time()+(e?1000000:0);
   }
   xSemaphoreGive(node_lock);
  }
  if(xSemaphoreTake(node_lock,0)==pdTRUE){
   if(game.state!=GAME_RUNNING && !scan_owned && !game_preparing){
    for(unsigned n=0;n<RECEIVER_SLOTS;n++){
     unsigned slot=(restore_next+n)%RECEIVER_SLOTS;
     if(!flag_have_original[slot] || esp_timer_get_time()<restore_due[slot])continue;
     restore_next=(slot+1)%RECEIVER_SLOTS;node_spi_slot_stats_t stats;
     if(!node_spi_slot_stats(slot,&stats))break;
     if(stats.mutation_revision!=restore_revisions[slot])flag_restore_steps[slot].phase=0;
     bool done=false;int64_t deadline=esp_timer_get_time()+100000;
     esp_err_t e=receiver_restore_step(&receivers,slot,&flag_original[slot],&flag_restore_steps[slot],additional_slot_exchange,&deadline,ESP_ERR_INVALID_RESPONSE,&done);
     if(node_spi_slot_stats(slot,&stats))restore_revisions[slot]=stats.mutation_revision;
     if(done)flag_have_original[slot]=false;
     restore_due[slot]=esp_timer_get_time()+(e?1000000:0);
     break;
    }
   }
   xSemaphoreGive(node_lock);
  }
  if(xSemaphoreTake(node_lock,0)==pdTRUE){
   int selected=(game_preparing || eventwatch_owned || protocol_owned || clocktest_owned)?-1:receiver_event_next(&receivers,&event_next);
   if(selected>=0){
    unsigned slot=(unsigned)selected,drained=0;int64_t deadline=esp_timer_get_time()+100000;
    int32_t e=receiver_drain_events(&receivers,slot,additional_slot_exchange,additional_slot_now,&deadline,1,&drained);
    /* Process the journal even when ACK failed: an edge already published
     * before ACK must never disappear from a consumer's view. */
    game_receiver_consume(&game_receivers,&game,&receivers,slot,16);
    if(!slot && timer.running){
     for(unsigned p=0;p<GAME_PILOTS;p++)if(game.match_config.pilots[p].enabled && game.match_config.pilots[p].slot==0)timer=game.laps[p];
     taskENTER_CRITICAL(&state_lock);snapshot.total_laps=timer.total_laps;snapshot.fastest_lap_ms=timer.fastest_us/1000;
     if(timer.recent_count)snapshot.last_lap_ms=timer.recent_us[(timer.recent_next+LAP_RECENT_MAX-1)%LAP_RECENT_MAX]/1000;
     taskEXIT_CRITICAL(&state_lock);
    }
    if(e){
     receiver_slot_t *r=&receivers.slots[slot];r->capture_requested=false;r->stop_pending=true;r->capture_error=e;
     if(!slot){event_capture_active=false;event_capture_error=e;node_time.valid=false;}
     receiver_health(&receivers,slot,e,r->session,false,false,false,esp_timer_get_time());
     if(game.state==GAME_RUNNING && game_session_config(&game)->mode==GAME_LAPS && (game_receiver_mask(game_session_config(&game))&(1u<<slot)))game_receiver_fault(3);
    }
   }
   xSemaphoreGive(node_lock);
  }
  if(xSemaphoreTake(node_lock,0)==pdTRUE){flag_scan_step();xSemaphoreGive(node_lock);}
  for(unsigned slot=1;slot<RECEIVER_SLOTS;slot++){
   if(!node_spi_slot_enabled(slot) || esp_timer_get_time()<next[slot])continue;
   if(xSemaphoreTake(node_lock,0)!=pdTRUE)continue;
   if(receivers.slots[slot].stop_pending && receivers.slots[slot].stop_keep_calibrated){xSemaphoreGive(node_lock);continue;}
   if(game_preparing || flag_have_original[slot]){polls[slot].phase=RP_INFO;clock_due[slot]=false;xSemaphoreGive(node_lock);continue;}
   node_spi_slot_stats_t stats;
   if(!node_spi_slot_stats(slot,&stats)){xSemaphoreGive(node_lock);continue;}
   if(stats.mutation_revision!=revisions[slot]){polls[slot].phase=RP_INFO;clock_due[slot]=false;}
   if(receivers.slots[slot].stop_pending && polls[slot].phase!=RP_STOP && polls[slot].phase!=RP_STOP_OBS && polls[slot].phase!=RP_STOP_WRITE){polls[slot].phase=RP_INFO;clock_due[slot]=false;}
   if(clock_due[slot]){
    int64_t clock_deadline=esp_timer_get_time()+100000;
    receiver_poll_clock(&receivers,slot,additional_slot_exchange,additional_slot_now,&clock_deadline);
    clock_due[slot]=false;next[slot]=esp_timer_get_time()+200000;xSemaphoreGive(node_lock);continue;
   }
   int32_t e;bool done=false;int64_t deadline=esp_timer_get_time()+100000;
   /* READY may be low while the C5 queues its next transaction. Let the
    * transport wait within this step's deadline instead of declaring a fault
    * from a single GPIO sample between successful requests. */
   e=receiver_poll_step(&receivers,slot,&polls[slot],additional_slot_exchange,&deadline,esp_timer_get_time(),&done);
   if(node_spi_slot_stats(slot,&stats))revisions[slot]=stats.mutation_revision;
   if(done && game.state==GAME_RUNNING && game_session_config(&game)->mode==GAME_LAPS && (game_receiver_mask(game_session_config(&game))&(1u<<slot))){
    if(e || !receivers.slots[slot].active || !receivers.slots[slot].capture_requested)game_receiver_fault(3);
    else game_receiver_consume(&game_receivers,&game,&receivers,slot,16);
   }
   clock_due[slot]=done && !e;
   next[slot]=esp_timer_get_time()+(e?1000000:0);xSemaphoreGive(node_lock);
  }
  vTaskDelay(pdMS_TO_TICKS(40));
 }
}

/* Run isolated rule checks before radio/task heaps fragment. The serial
 * query reports this boot's result and never borrows the live match state. */
static unsigned game_selftest_failures;
static esp_err_t game_selftest_error=ESP_ERR_INVALID_STATE;
static void game_boot_selftest(void){
            game_t *g=malloc(sizeof(*g));unsigned failures=0;
            if(!g){game_selftest_error=ESP_ERR_NO_MEM;return;}
            failures=game_rule_selftest(g);
            free(g);game_selftest_failures=failures;game_selftest_error=failures?ESP_FAIL:ESP_OK;
 }

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(node_spi_init());
    node_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(node_lock ? ESP_OK : ESP_ERR_NO_MEM);
    if (!lap_timer_init(&timer, 1000000u)) {
        ESP_LOGE(TAG, "lap timer initialization failed");
        abort();
    }
    game_init(&game);game_load();
    load_results();
    load_config();
    slot_configs_load();
    for(unsigned i=0;i<RECEIVER_SLOTS;i++)receivers.slots[i].enabled=node_spi_slot_enabled(i);
    if(have_saved_config)receiver_desire(&receivers,0,&saved_config);
    game_boot_selftest();
    ESP_ERROR_CHECK(network_start());
    ESP_ERROR_CHECK(io_service_start());
    if(network_role()!=NET_MEMBER)ESP_ERROR_CHECK(ble_control_start());
    ESP_ERROR_CHECK(xTaskCreate(poll_task, "c5_poll", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(additional_slots_task,"rf_slots",3072,NULL,3,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "C3 controller ready; M1 timing protocol enabled");
    usb_serial_jtag_driver_config_t serial = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&serial));
    /* Use the installed driver's TX buffer as well as its command RX path. */
    usb_serial_jtag_vfs_use_driver();
    configASSERT(xTaskCreate(serial_diag_task, "serial_diag", 8192, NULL, 3, NULL) == pdPASS);
}

/* Uses the same controller APIs as BLE. Test changes are restored before return. */
/* 0 idle, 1 preparing, 2 monitoring, 3 stopping, 4 restoring, 5 done. */
static void eventwatch_progress(uint8_t phase,int32_t error,uint32_t events){
 xSemaphoreTake(node_lock,portMAX_DELAY);
 eventwatch_status.phase=phase;eventwatch_status.error=error;eventwatch_status.events=events;
 if(error && eventwatch_status.retries<UINT32_MAX)++eventwatch_status.retries;
 xSemaphoreGive(node_lock);
}
static bool eventwatch_cancelled(void){
 xSemaphoreTake(node_lock,portMAX_DELAY);bool cancel=eventwatch_cancel;xSemaphoreGive(node_lock);return cancel;
}
#define EW_IO(result,expression) do{ xSemaphoreTake(node_lock,portMAX_DELAY);result=(expression);xSemaphoreGive(node_lock);taskYIELD(); }while(0)
static void eventwatch_task(void *arg){
 (void)arg;
            uint8_t saved_limits[12], trial_limits[12];
            node_info_t initial;
            esp_err_t err;EW_IO(err,node_spi_get_info(&initial));
            if (err == ESP_OK) EW_IO(err,node_spi_exchange(NODE_CMD_GET_LIMITS, NULL, 0, 12, saved_limits));
            bool have_limits = err == ESP_OK;
            if (have_limits) {
                memcpy(trial_limits, saved_limits, 12);
                /* Keep a stationary ON signal from hold-rearming every 3s. */
                trial_limits[8] = 0x60; trial_limits[9] = 0xea;
                trial_limits[10] = trial_limits[11] = 0; /* 60000 ms */
                EW_IO(err,node_spi_exchange(NODE_CMD_SET_LIMITS, trial_limits, 12, 0, NULL));
            }
            if (err == ESP_OK && eventwatch_cancelled())err=ESP_ERR_INVALID_STATE;
            if (err == ESP_OK) EW_IO(err,node_spi_set_active(true, true));
            ESP_LOGI(TAG, "EVENTWATCH armed=%s duration_s=120 hold_ms=60000", esp_err_to_name(err));
            int64_t deadline = esp_timer_get_time() + 120000000;
            unsigned events = 0, polls = 0;
            eventwatch_progress(2,err,events);
            while (err == ESP_OK && !eventwatch_cancelled() && esp_timer_get_time() < deadline) {
                node_info_t info;
                EW_IO(err,node_spi_get_info(&info));
                if (err == ESP_OK && (info.session != initial.session || info.faults)) err = ESP_ERR_INVALID_STATE;
                node_event_t event = {0};
                bool irq = node_spi_interrupt();
                if (err == ESP_OK) EW_IO(err,node_spi_get_event(&event));
                if (err == ESP_OK && event.sequence) {
                    ESP_LOGI(TAG, "EVENTWATCH event seq=%" PRIu32 " node_us=%" PRIu64 " host_us=%" PRIi64 " edge=%u valid=%u power=%u clip=%u irq=%u",
                        event.sequence,event.timestamp_us,esp_timer_get_time(),event.edge,event.valid,event.power,event.clip_pm,irq);
                    EW_IO(err,node_spi_ack_event(event.sequence)); ++events;
                }
                if (err == ESP_OK && ++polls % 5 == 0) {
                    node_observation_detail_t obs;
                    EW_IO(err,node_spi_get_observation_detail(&obs));
                    if (err == ESP_OK) ESP_LOGI(TAG, "EVENTWATCH sample power=%u clip=%u quality=%u",obs.power,obs.clip_pm,obs.quality_flags);
                }
                eventwatch_progress(2,err,events);
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            /* Keep ownership until STOP and limits readback are confirmed.
             * Lost replies are uncertain outcomes and retry indefinitely. */
            esp_err_t stopped,restored=ESP_OK;uint8_t confirmed[12];
            eventwatch_progress(3,0,events);
            do{
                EW_IO(stopped,node_spi_set_active(false,false));
                eventwatch_progress(3,stopped,events);
                if(stopped)vTaskDelay(pdMS_TO_TICKS(1000));
            }while(stopped);
            if(have_limits)do{
                eventwatch_progress(4,0,events);
                EW_IO(restored,node_spi_exchange(NODE_CMD_SET_LIMITS,saved_limits,12,0,NULL));
                if(!restored)EW_IO(restored,node_spi_exchange(NODE_CMD_GET_LIMITS,NULL,0,12,confirmed));
                if(!restored && memcmp(confirmed,saved_limits,12))restored=ESP_ERR_INVALID_RESPONSE;
                eventwatch_progress(4,restored,events);
                if(restored)vTaskDelay(pdMS_TO_TICKS(1000));
            }while(restored);
            xSemaphoreTake(node_lock,portMAX_DELAY);
            calibrated=false;eventwatch_owned=false;eventwatch_cancel=false;
            eventwatch_status.phase=5;eventwatch_status.error=err;eventwatch_status.events=events;
            receivers.slots[0].calibrated=false;
            taskENTER_CRITICAL(&state_lock);snapshot.calibrated=0;taskEXIT_CRITICAL(&state_lock);
            xSemaphoreGive(node_lock);
            ESP_LOGI(TAG,"EVENTWATCH DONE result=%s events=%u stop=%s restore=%s",esp_err_to_name(err),events,esp_err_to_name(stopped),esp_err_to_name(restored));
            vTaskDelete(NULL);
}
#undef EW_IO

/* Check current ownership only after acquiring node_lock. A cached snapshot
 * cannot protect against a start accepted while diagnostics waited for it. */
static bool diagnostic_lock(void){
 if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))!=pdTRUE)return false;
 if(game_busy() || game_preparing || timer.running || primary_capture_busy() ||
    additional_capture_busy() || flag_restore_busy() || snapshot.auto_gain_state==1){
  xSemaphoreGive(node_lock);return false;
 }
 return true;
}

static void protocol_test_task(void *arg){
 (void)arg;esp_err_t error=ESP_OK;bool done=false;
 node_capabilities_t caps;node_clock_t clock={0};node_observation_detail_t obs;
 while(!done){
  xSemaphoreTake(node_lock,portMAX_DELAY);
  if(protocol_cancel){error=ESP_ERR_INVALID_STATE;xSemaphoreGive(node_lock);break;}
  if(protocol_test.kind==3){
   switch(protocol_test.step){
    case 0:error=node_spi_get_capabilities(&caps);break;
    case 1:error=node_spi_get_clock(&clock);break;
    case 2:error=node_spi_get_observation_detail(&obs);done=true;
     if(!error && clock.session!=obs.session)error=ESP_ERR_INVALID_RESPONSE;
     break;
    default:error=ESP_ERR_INVALID_STATE;break;
   }
   if(!error)++protocol_test.step;
  }else error=node_spi_protocol_step(&protocol_test,&done);
  protocol_steps=protocol_test.step;
  xSemaphoreGive(node_lock);
  if(error)break;
  if(protocol_test.kind==3 && protocol_steps==1)ESP_LOGI(TAG,"NODE contract=%u firmware=%u.%u channels=%u gain_max=%u fifo=%u features=%"PRIu32" observe_ms=%u settle_ms=%u clip_limit=%u stale_ms=%u bandwidth=%u",
   caps.contract,caps.firmware_major,caps.firmware_minor,caps.channels,caps.max_gain,caps.event_capacity,caps.features,caps.observation_ms,caps.settle_ms,caps.clip_limit_pm,caps.stale_ms,caps.bandwidth_mhz);
  if(protocol_test.kind==3 && done)ESP_LOGI(TAG,"NODE clock_us=%"PRIu64" session=%"PRIu32" observed_us=%"PRIu64" observed_session=%"PRIu32" ch=%u power=%u clip=%u windows=%"PRIu32" quality=0x%02x",
   clock.timestamp_us,clock.session,obs.timestamp_us,obs.session,obs.channel,obs.power,obs.clip_pm,obs.sample_count,obs.quality_flags);
  vTaskDelay(pdMS_TO_TICKS(20));
 }
 /* Scan-start may have executed even if its reply was lost. Always cancel
  * and confirm terminal status before releasing the reservation. */
 if(protocol_test.kind==2){
  receiver_scan_stop_t stop={0};bool stopped=false;
  while(!stopped){
   xSemaphoreTake(node_lock,portMAX_DELAY);protocol_error=error;
   int64_t deadline=esp_timer_get_time()+100000;
   esp_err_t cleanup=receiver_scan_stop_step(&stop,additional_slot_exchange,&deadline,0,ESP_ERR_INVALID_STATE,ESP_ERR_INVALID_RESPONSE,&stopped);
   xSemaphoreGive(node_lock);
   if(cleanup)vTaskDelay(pdMS_TO_TICKS(1000));
   else if(!stopped)vTaskDelay(pdMS_TO_TICKS(20));
  }
 }
 xSemaphoreTake(node_lock,portMAX_DELAY);protocol_error=error;protocol_owned=false;
 const char *name=protocol_test.kind==3?"node":protocol_test.kind==2?"retrytest":protocol_test.kind?"shorttest":"frametest";xSemaphoreGive(node_lock);
 ESP_LOGI(TAG,"PROTOCOL_TEST command=%s result=%s",name,esp_err_to_name(error));vTaskDelete(NULL);
}

static void clocktest_task(void *arg){
 (void)arg;uint64_t previous=0;uint32_t session=0;esp_err_t error=ESP_OK;
 for(unsigned i=0;i<100;i++){
  uint8_t data[12];int64_t start,end;
  xSemaphoreTake(node_lock,portMAX_DELAY);
  if(clocktest_cancel){error=ESP_ERR_INVALID_STATE;xSemaphoreGive(node_lock);break;}
  start=esp_timer_get_time();
  error=node_spi_exchange_until(0,NODE_CMD_CLOCK,NULL,0,sizeof(data),data,start+100000);
  end=esp_timer_get_time();xSemaphoreGive(node_lock);
  if(error!=ESP_OK)break;
  uint64_t now;uint32_t current;memcpy(&now,data,8);memcpy(&current,data+8,4);
  if(i && (now<=previous || current!=session)){error=ESP_ERR_INVALID_RESPONSE;break;}
  xSemaphoreTake(node_lock,portMAX_DELAY);clocktest_samples=i+1;xSemaphoreGive(node_lock);
  ESP_LOGI("node_spi","CLOCK_SAMPLE index=%u c3_start=%lld c3_end=%lld c5=%llu session=%lu",i,(long long)start,(long long)end,(unsigned long long)now,(unsigned long)current);
  previous=now;session=current;vTaskDelay(pdMS_TO_TICKS(20));
 }
 xSemaphoreTake(node_lock,portMAX_DELAY);clocktest_error=error;clocktest_owned=false;xSemaphoreGive(node_lock);
 ESP_LOGI(TAG,"PROTOCOL_TEST command=clocktest result=%s",esp_err_to_name(error));vTaskDelete(NULL);
}
static void serial_diag_task(void *arg)
{
    (void)arg;
    char line[96]; size_t used = 0;
    for (;;) {
        char ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (ch != '\r' && ch != '\n') {
            if (used < sizeof(line)-1) line[used++] = ch;
            else used = 0;
            continue;
        }
        line[used] = 0; used = 0;
        if (!line[0]) continue;
        controller_snapshot_t s; controller_get_snapshot(&s);
        if (!strcmp(line, "diag")) {
            ESP_LOGI(TAG, "DIAG online=%u rf=%u session=%" PRIu32 " ch=%u gain=%u enter=%u exit=%u confirm=%u running=%u calibrated=%u power=%u clip=%u overflow=%" PRIu32,
                s.node_online,s.rf_valid,s.boot_session,s.channel,s.gain,s.enter_power,s.exit_power,s.confirm_ms,s.running,s.calibrated,s.observed_power,s.observed_clip_pm,s.event_overflows);
        } else if (!strncmp(line,"storagefail ",12)) {
            unsigned target;char extra;uint8_t state;uint32_t session;uint8_t schedule[32];network_status(schedule,45);
            esp_err_t e=ESP_ERR_INVALID_ARG;
            if(sscanf(line+12,"%u %c",&target,&extra)==1 && target<=STORAGE_FAULT_NETWORK_RESULT){
                if(!target){storage_fault_arm(0);e=ESP_OK;}
                else if(controller_game_phase(&state,&session) && state!=GAME_RUNNING && state!=GAME_PAUSED && !schedule[1] && !s.running && s.auto_gain_state!=1){storage_fault_arm(target);e=ESP_OK;}
                else e=ESP_ERR_INVALID_STATE;
            }
            ESP_LOGI(TAG,"STORAGEFAIL result=%s",esp_err_to_name(e));
        } else if (!strcmp(line,"resultclear")) {
            if(game_busy()){ESP_LOGI(TAG,"RESULTCLEAR result=ESP_ERR_INVALID_STATE");continue;}
            nvs_handle_t h;esp_err_t e=nvs_open("game",NVS_READWRITE,&h);
            if(e==ESP_OK){e=nvs_erase_key(h,"result");if(e==ESP_ERR_NVS_NOT_FOUND)e=ESP_OK;if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
            ESP_LOGI(TAG,"RESULTCLEAR result=%s",esp_err_to_name(e));
        } else if (!strncmp(line,"control ",8)) {
            uint8_t command[16];unsigned length=0;char *cursor=line+8;bool valid=true;
            while(*cursor){
                while(*cursor==' ')++cursor;
                if(!*cursor)break;
                unsigned byte;int consumed=0;
                if(length==sizeof(command) || sscanf(cursor,"%2x%n",&byte,&consumed)!=1 || (cursor[consumed] && cursor[consumed]!=' ')){valid=false;break;}
                command[length++]=byte;cursor+=consumed;
            }
            ESP_LOGI(TAG,"CONTROL result=%s",esp_err_to_name(valid?network_control(command,length):ESP_ERR_INVALID_ARG));
        } else if (!strncmp(line,"net ",4)) {
            unsigned role,channel,id;char key[33]={0},host[13]={0};
            network_config_t c={0};
            if(sscanf(line+4,"%u %u %u %32s %12s",&role,&channel,&id,key,host)==5 && strlen(key)==32 && strlen(host)==12 && role<=2 && channel<=13){
                c.role=role;c.channel=channel;c.network=id;bool valid=true;
                for(unsigned i=0;i<16;i++){unsigned b;if(sscanf(key+2*i,"%2x",&b)!=1){valid=false;break;}c.key[i]=b;}
                for(unsigned i=0;i<6;i++){unsigned b;if(sscanf(host+2*i,"%2x",&b)!=1){valid=false;break;}c.host[i]=b;}
                esp_err_t e=valid?network_configure(&c):ESP_ERR_INVALID_ARG;ESP_LOGI(TAG,"NETWORK save=%s reboot_required=1",esp_err_to_name(e));
            }else ESP_LOGW(TAG,"net ROLE CHANNEL NETWORK_ID KEY32HEX HOST12HEX");
        } else if (!strcmp(line,"network")) {
            uint8_t p[32];network_status(p,0);ESP_LOGI(TAG,"NETWORK role=%u channel=%u members=%u mac=%02x:%02x:%02x:%02x:%02x:%02x",p[1],p[2],p[3],p[8],p[9],p[10],p[11],p[12],p[13]);
        } else if (!strcmp(line,"netsync")) {
            ESP_LOGI(TAG,"NETSYNC result=%s",esp_err_to_name(network_sync()));
        } else if (!strcmp(line,"netready")) {
            uint8_t cmd[3]={1,23,0};ESP_LOGI(TAG,"NETREADY result=%s",esp_err_to_name(network_control(cmd,3)));
        } else if (!strcmp(line,"netdropack")) {
            network_drop_next_ack();ESP_LOGI(TAG,"NETDROP armed=1 expiry_ms=30000");
        } else if (!strcmp(line,"netbenchdrop")) {
            ESP_LOGI(TAG,"BENCHDROP result=%s",esp_err_to_name(network_bench_drop_ack()));
        } else if (!strncmp(line,"netbench ",9)) {
            unsigned count;if(sscanf(line+9,"%u",&count)==1)ESP_LOGI(TAG,"BENCHSTART result=%s",esp_err_to_name(network_event_bench(count)));
        } else if (!strcmp(line,"spideadline")) {
            if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE){ESP_LOGI(TAG,"SPIDEADLINE busy");continue;}
            uint8_t info[17];int64_t start=esp_timer_get_time();
            esp_err_t expired=node_spi_exchange_until(0,1,NULL,0,17,info,start-1);
            int64_t rejected_us=esp_timer_get_time()-start;start=esp_timer_get_time();
            esp_err_t normal=node_spi_exchange_until(0,1,NULL,0,17,info,start+100000);
            ESP_LOGI(TAG,"SPIDEADLINE expired=%s reject_us=%"PRIi64" normal=%s elapsed_us=%"PRIi64,esp_err_to_name(expired),rejected_us,esp_err_to_name(normal),esp_timer_get_time()-start);
            xSemaphoreGive(node_lock);
        } else if (!strcmp(line,"slotcheck")) {
            if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))!=pdTRUE){ESP_LOGI(TAG,"SLOTCHECK busy");continue;}
            uint8_t before[20],after[20];node_spi_slot_stats_t stats_before={0},stats_after={0};
            /* INFO is 17 bytes; compare identity/session, not transaction count. */
            esp_err_t e=node_spi_exchange_slot(0,1 /* INFO */,NULL,0,17,before);
            node_spi_slot_stats(0,&stats_before);bool rejected=true;
            for(unsigned i=1;i<NODE_SPI_SLOTS;i++)if(!node_spi_slot_enabled(i))rejected&=node_spi_exchange_slot(i,1 /* INFO */,NULL,0,17,after)==ESP_ERR_INVALID_STATE;
            rejected&=node_spi_exchange_slot(NODE_SPI_SLOTS,1 /* INFO */,NULL,0,17,after)==ESP_ERR_INVALID_ARG;
            node_spi_slot_stats(0,&stats_after);
            if(e==ESP_OK)e=node_spi_exchange_slot(0,1 /* INFO */,NULL,0,17,after);
            bool identity=e==ESP_OK && !memcmp(before,after,12);
            ESP_LOGI(TAG,"SLOTCHECK result=%s identity=%u rejected=%u isolated=%u",esp_err_to_name(e),identity,rejected,stats_before.requests==stats_after.requests && stats_before.errors==stats_after.errors);
            xSemaphoreGive(node_lock);
        } else if (!strcmp(line,"slots")) {
            for(unsigned i=0;i<NODE_SPI_SLOTS;i++){
                node_spi_slot_stats_t st={0};bool available=node_spi_slot_stats(i,&st);
                ESP_LOGI(TAG,"SLOT index=%u enabled=%u ready=%u stats=%u requests=%"PRIu32" errors=%"PRIu32" invalid=%"PRIu32" last=%"PRIi32,i,node_spi_slot_enabled(i),node_spi_slot_ready(i),available,st.requests,st.errors,st.invalid_responses,st.last_error);
            }
        } else if (!strcmp(line,"netdiag")) {
            network_diagnostics();
        } else if (!strcmp(line,"blebonds")) {
            ESP_LOGI(TAG,"BLE bonds queued=%s",esp_err_to_name(ble_control_bonds(NULL)));
        } else if (!strncmp(line,"bleforget ",10)) {
            ESP_LOGI(TAG,"BLE forget queued=%s",esp_err_to_name(ble_control_bonds(line+10)));
        } else if (!strcmp(line,"blerepair")) {
            ESP_LOGI(TAG,"BLE repair authorization: %s",esp_err_to_name(ble_control_open_repair()));
        } else if (!strcmp(line,"blepair")) {
            ESP_LOGI(TAG,"BLE authorization: %s",esp_err_to_name(ble_control_open_authorization()));
        } else if (!strcmp(line,"io")) {
            io_service_diagnostics();
        } else if (!strcmp(line,"netbadconfig")) {
            ESP_LOGI(TAG,"NETBADCONFIG result=%s",esp_err_to_name(network_bad_config_test()));
        } else if (!strncmp(line,"gametune ",9)) {
            unsigned fresh,drain,recover;if(sscanf(line+9,"%u %u %u",&fresh,&drain,&recover)==3 && fresh<=65535 && drain<=65535 && recover<=65535){uint8_t cmd[8]={1,31,fresh,fresh>>8,drain,drain>>8,recover,recover>>8};ESP_LOGI(TAG,"GAMETUNE result=%s",esp_err_to_name(network_control(cmd,8)));}
        } else if (!strcmp(line,"gametuning")) {
            game_config_t config;if(controller_game_config_get(&config))ESP_LOGI(TAG,"GAMETUNING freshness=%"PRIu32" drain=%u recover=%u",config.freshness_ms,config.solo_drain_percent,config.solo_recover_percent);else ESP_LOGW(TAG,"GAMETUNING busy");
        } else if (!strncmp(line,"gamepage ",9)) {
            unsigned page,offset=0;uint8_t bytes[32];
            if(sscanf(line+9,"%u %u",&page,&offset)>=1 && controller_game_page(page,offset,bytes)){
                char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",bytes[i]);
                ESP_LOGI(TAG,"GAMEPAGE data=%s",hex);
            }else ESP_LOGI(TAG,"GAMEPAGE invalid or busy");
        } else if (!strncmp(line,"gameconfigraw ",14)) {
            unsigned offset;game_config_t config;
            if(sscanf(line+14,"%u",&offset)==1 && offset<sizeof(config) && controller_game_config_get(&config)){
                unsigned size=sizeof(config)-offset;if(size>12)size=12;
                char hex[25];for(unsigned i=0;i<size;i++)snprintf(hex+2*i,3,"%02x",((uint8_t*)&config)[offset+i]);
                ESP_LOGI(TAG,"GAMECONFIGRAW offset=%u data=%s",offset,hex);
            }else ESP_LOGW(TAG,"GAMECONFIGRAW invalid or busy");
        } else if (!strcmp(line,"gamefault")) {
            ESP_LOGI(TAG,"GAMEFAULT result=%s",esp_err_to_name(controller_game_fault_pause(3)));
        } else if (!strncmp(line,"gamefault ",10)) {
            uint32_t session=0;
            esp_err_t error=sscanf(line+10,"%"SCNu32,&session)==1 && session?
                controller_game_fault_pause_session(3,session):ESP_ERR_INVALID_ARG;
            ESP_LOGI(TAG,"GAMEFAULT result=%s",esp_err_to_name(error));
        } else if (!strcmp(line,"pollpath")) {
            if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1000))==pdTRUE){
                ESP_LOGI(TAG,"POLLPATH completed=%"PRIu32" discarded=%"PRIu32,poll_completed,poll_discarded);
                xSemaphoreGive(node_lock);
            }else ESP_LOGW(TAG,"POLLPATH busy");
        } else if (!strcmp(line,"gameconfig")) {
            game_config_t config;
            if(controller_game_config_get(&config))ESP_LOGI(TAG,"GAMECONFIG checksum=%08"PRIx32" bytes=%u mode=%u min=%"PRIu32" duration=%"PRIu32" capture=%"PRIu32,checksum_bytes((uint8_t*)&config,sizeof(config)),(unsigned)sizeof(config),config.mode,config.minimum_lap_ms,config.duration_ms,config.capture_ms);
            else ESP_LOGW(TAG,"GAMECONFIG busy");
        } else if (!strcmp(line,"netcapture")) {
            ESP_LOGI(TAG,"NETCAPTURE result=%s",esp_err_to_name(network_capture_result_pages()));
        } else if (!strncmp(line,"netraw ",7)) {
            unsigned page;uint8_t bytes[32];
            if(sscanf(line+7,"%u",&page)==1 && page<=416){network_status(bytes,page);char hex[65];for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",bytes[i]);ESP_LOGI(TAG,"NETRAW page=%u data=%s",page,hex);}
        } else if (!strncmp(line,"netpage ",8)) {
            unsigned page;if(sscanf(line+8,"%u",&page)==1 && page<=94){uint8_t p[32];network_status(p,page);ESP_LOGI(TAG,"NETPAGE page=%u format=%u online=%u state=%u mode=%u fault=%u rf_online=%u rf_valid=%u calibrated=%u",page,p[0],p[8],p[9],p[10],p[11],p[12],p[13],p[14]);}
        } else if (!strncmp(line,"netrf ",6)) {
            unsigned index;if(sscanf(line+6,"%u",&index)==1 && index<8){uint8_t p[32];network_status(p,37+index);ESP_LOGI(TAG,"NETRF member=%u fresh=%u channel=%u gain=%u enter=%u exit=%u confirm=%u",index,p[8],p[10],p[11],p[12]|p[13]<<8,p[14]|p[15]<<8,p[16]|p[17]<<8);}
        } else if (!strncmp(line,"nettiming ",10)) {
            unsigned index;if(sscanf(line+10,"%u",&index)==1 && index<8){uint8_t p[32];network_status(p,46+index);uint32_t uncertainty,session;memcpy(&uncertainty,p+12,4);memcpy(&session,p+16,4);ESP_LOGI(TAG,"NETTIMING member=%u fresh=%u valid=%u phase=%u uncertainty_us=%"PRIu32" session=%"PRIu32,index,p[8],p[9],p[10],uncertainty,session);}
        } else if (!strcmp(line,"netclock")) {
            uint8_t p[32];network_status(p,26);int64_t offset;uint32_t uncertainty;memcpy(&offset,p+4,8);memcpy(&uncertainty,p+12,4);ESP_LOGI(TAG,"NETCLOCK valid=%u offset_us=%"PRIi64" uncertainty_us=%"PRIu32,p[1],offset,uncertainty);
        } else if (!strncmp(line,"netgain ",8)) {
            unsigned gain;char mac[13]={0};
            if(sscanf(line+8,"%12s %u",mac,&gain)==2 && strlen(mac)==12 && gain<=63){
                uint8_t cmd[16]={1,26};bool valid=true;
                for(unsigned i=0;i<6;i++){unsigned value;if(sscanf(mac+2*i,"%2x",&value)!=1){valid=false;break;}cmd[2+i]=value;}
                bool found=false;
                for(unsigned i=0;i<8;i++){uint8_t page[32];network_status(page,37+i);if(page[8] && !memcmp(page+2,cmd+2,6)){memcpy(cmd+8,page+10,8);cmd[9]=gain;found=true;break;}}
                valid=valid && found;
                ESP_LOGI(TAG,"NETGAIN result=%s",esp_err_to_name(valid?network_control(cmd,16):ESP_ERR_INVALID_ARG));
            }
        } else if (!strcmp(line,"netsave")) {
            ESP_LOGI(TAG,"NETSAVE result=%s",esp_err_to_name(network_save_result()));
        } else if (!strncmp(line,"netmember ",10)) {
            unsigned action=0;char old[13]={0},next[13]={0};
            int fields=sscanf(line+10,"%u %12s %12s",&action,old,next);
            uint8_t cmd[14]={1,action};bool valid=(action==34 && fields==3) || ((action==35 || action==36) && fields==2);
            unsigned macs=action==34?2:1;
            for(unsigned n=0;valid && n<macs;n++){const char *value=n?next:old;if(strlen(value)!=12){valid=false;break;}for(unsigned i=0;i<12;i++)if(!((value[i]>='0' && value[i]<='9') || (value[i]>='a' && value[i]<='f') || (value[i]>='A' && value[i]<='F')))valid=false;for(unsigned i=0;valid && i<6;i++){unsigned byte;sscanf(value+i*2,"%2x",&byte);cmd[2+n*6+i]=byte;}}
            ESP_LOGI(TAG,"NETMEMBER result=%s",esp_err_to_name(valid?network_control(cmd,action==34?14:8):ESP_ERR_INVALID_ARG));
        } else if (!strcmp(line,"netrostersave") || !strcmp(line,"netrosterunlock")) {
            uint8_t cmd[2]={1,!strcmp(line,"netrostersave")?32:33};ESP_LOGI(TAG,"NETROSTER result=%s",esp_err_to_name(network_control(cmd,2)));
        } else if (!strcmp(line,"netroster")) {
            uint8_t p[32];network_status(p,94);uint32_t error;memcpy(&error,p+4,4);ESP_LOGI(TAG,"NETROSTER fixed=%u count=%u error=%"PRIu32,p[1],p[2],error);
        } else if (!strcmp(line,"nethistory")) {
            for(unsigned i=0;i<4;i++){uint8_t p[32];network_status(p,54+i*10);uint32_t revision,session;memcpy(&revision,p+4,4);memcpy(&session,p+16,4);ESP_LOGI(TAG,"NETHISTORY index=%u valid=%u count=%u revision=%"PRIu32" session=%"PRIu32,i,p[1],p[3],revision,session);}
        } else if (!strcmp(line,"netresult")) {
            uint8_t p[32];network_status(p,27);uint32_t revision,session,error;memcpy(&revision,p+4,4);memcpy(&error,p+12,4);memcpy(&session,p+16,4);ESP_LOGI(TAG,"NETRESULT valid=%u revision=%"PRIu32" session=%"PRIu32" error=%"PRIu32,p[1],revision,session,error);
        } else if (!strcmp(line,"netscores")) {
            uint8_t p[32];network_status(p,17);uint32_t session;uint16_t included,missing;uint64_t red,blue;memcpy(&session,p+4,4);memcpy(&included,p+10,2);memcpy(&missing,p+12,2);memcpy(&red,p+16,8);memcpy(&blue,p+24,8);ESP_LOGI(TAG,"NETSCORES session=%"PRIu32" included=%u missing=%u red_ms=%"PRIu64" blue_ms=%"PRIu64,session,included,missing,red,blue);
        } else if (!strncmp(line,"gameaction ",11)) {
            unsigned action;if(sscanf(line+11,"%u",&action)==1 && action<=3){uint8_t cmd[3]={1,10,action};ESP_LOGI(TAG,"GAMEACTION result=%s",esp_err_to_name(network_control(cmd,3)));}
        } else if (!strncmp(line,"netmode ",8)) {
            unsigned mode;game_config_t c;esp_err_t e=ESP_ERR_INVALID_ARG;
            if(sscanf(line+8,"%u",&mode)==1 && mode<GAME_MODE_COUNT){
                if(!controller_game_config_get(&c))e=ESP_ERR_TIMEOUT;
                else {uint8_t cmd[15]={1,52,mode};memcpy(cmd+3,&c.minimum_lap_ms,4);memcpy(cmd+7,&c.duration_ms,4);memcpy(cmd+11,&c.capture_ms,4);e=network_control(cmd,sizeof(cmd));}
            }
            ESP_LOGI(TAG,"NETMODE result=%s",esp_err_to_name(e));
        } else if (!strcmp(line, "game")) {
            if(xSemaphoreTake(node_lock,pdMS_TO_TICKS(1500))==pdTRUE){
                ESP_LOGI(TAG,"GAME mode=%u state=%u session=%" PRIu32 " owner=%u fault=%u elapsed_ms=%" PRIu64 " red_ms=%" PRIu64 " blue_ms=%" PRIu64,
                    game_session_config(&game)->mode,game.state,game.session,game.owner,game.fault,game.elapsed_us/1000,game.score_us[0]/1000,game.score_us[1]/1000);
                xSemaphoreGive(node_lock);
            }
        } else if (!strcmp(line,"gametest")) {
            ESP_LOGI(TAG,"GAMETEST failures=%u result=%s source=boot modes=6 scope=synthetic-rule-core-no-RF",game_selftest_failures,esp_err_to_name(game_selftest_error));
        } else if (!strcmp(line, "service")) {
            uint64_t age = s.observation_updated_us ? (esp_timer_get_time() - s.observation_updated_us) / 1000u : UINT32_MAX;
            ESP_LOGI(TAG, "SERVICE revision=1 confirm=%u quality=%u samples=%" PRIu32 " age_ms=%" PRIu64 " save_error=%" PRIi32 " online=%u rf=%u",
                s.confirm_ms,s.observation_quality,s.observation_samples,age,s.config_save_error,s.node_online,s.rf_valid);
        } else if (!strcmp(line,"protocoltest status")) {
            xSemaphoreTake(node_lock,portMAX_DELAY);
            ESP_LOGI(TAG,"PROTOCOLTEST active=%u cancel=%u steps=%u error=%"PRIi32,protocol_owned,protocol_cancel,protocol_steps,protocol_error);
            xSemaphoreGive(node_lock);
        } else if (!strcmp(line,"protocoltest cancel")) {
            xSemaphoreTake(node_lock,portMAX_DELAY);if(protocol_owned)protocol_cancel=true;
            ESP_LOGI(TAG,"PROTOCOLTEST cancel pending=%u",protocol_owned);xSemaphoreGive(node_lock);
        } else if (!strcmp(line,"clocktest status")) {
            xSemaphoreTake(node_lock,portMAX_DELAY);
            ESP_LOGI(TAG,"CLOCKTEST active=%u cancel=%u samples=%u error=%"PRIi32,clocktest_owned,clocktest_cancel,clocktest_samples,clocktest_error);
            xSemaphoreGive(node_lock);
        } else if (!strcmp(line,"clocktest cancel")) {
            xSemaphoreTake(node_lock,portMAX_DELAY);if(clocktest_owned)clocktest_cancel=true;
            ESP_LOGI(TAG,"CLOCKTEST cancel pending=%u",clocktest_owned);xSemaphoreGive(node_lock);
        } else if (!strcmp(line, "frametest") || !strcmp(line, "clocktest") || !strcmp(line, "shorttest") || !strcmp(line,"retrytest") || !strcmp(line,"node")) {
            if (!diagnostic_lock()) {
                ESP_LOGW(TAG, "PROTOCOL_TEST busy"); continue;
            }
            if(!strcmp(line,"clocktest")){
                clocktest_owned=true;clocktest_cancel=false;clocktest_samples=0;clocktest_error=ESP_OK;
                if(xTaskCreate(clocktest_task,"clocktest",3072,NULL,3,NULL)!=pdPASS){clocktest_owned=false;clocktest_error=ESP_ERR_NO_MEM;ESP_LOGW(TAG,"PROTOCOL_TEST command=clocktest result=ESP_ERR_NO_MEM");}
                else ESP_LOGI(TAG,"PROTOCOL_TEST command=clocktest started");
                xSemaphoreGive(node_lock);continue;
            }
            protocol_test=(node_spi_protocol_test_t){.kind=!strcmp(line,"node")?3:!strcmp(line,"retrytest")?2:!strcmp(line,"shorttest")};
            protocol_owned=true;protocol_cancel=false;protocol_steps=0;protocol_error=ESP_OK;
            if(protocol_test.kind==2){calibrated=false;receivers.slots[0].calibrated=false;
                taskENTER_CRITICAL(&state_lock);snapshot.calibrated=0;taskEXIT_CRITICAL(&state_lock);}
            if(xTaskCreate(protocol_test_task,"protocoltest",3072,NULL,3,NULL)!=pdPASS){
                protocol_owned=false;protocol_error=ESP_ERR_NO_MEM;
                ESP_LOGW(TAG,"PROTOCOL_TEST command=%s result=ESP_ERR_NO_MEM",line);
            }else ESP_LOGI(TAG,"PROTOCOL_TEST command=%s started",line);
            xSemaphoreGive(node_lock);
        } else if (!strncmp(line, "spi ", 4)) {
            unsigned command, expected; int consumed = 0;
            uint8_t input[22], output[22]; unsigned length = 0;
            bool valid = sscanf(line + 4, "%u %u %n", &command, &expected, &consumed) == 2 &&
                command <= 255 && expected <= 22;
            char *cursor = line + 4 + consumed;
            while (valid && *cursor) {
                unsigned byte; int count = 0;
                if (length == 22 || sscanf(cursor, "%2x%n", &byte, &count) != 1) { valid = false; break; }
                input[length++] = byte; cursor += count;
                while (*cursor == ' ') ++cursor;
            }
            if (!valid) { ESP_LOGW(TAG,"SPI invalid or busy"); continue; }
            if (!diagnostic_lock()) { ESP_LOGW(TAG,"SPI busy"); continue; }
            esp_err_t err = node_spi_exchange(command, input, length, expected, output);
            char hex[45] = {0};
            if (err == ESP_OK) for (unsigned i = 0; i < expected; ++i) snprintf(hex + i * 2, 3, "%02x", output[i]);
            ESP_LOGI(TAG,"SPI cmd=%u result=%s data=%s irq=%u",command,esp_err_to_name(err),hex,node_spi_interrupt());
            xSemaphoreGive(node_lock);
        } else if (!strcmp(line,"eventwatch status")) {
            xSemaphoreTake(node_lock,portMAX_DELAY);
            ESP_LOGI(TAG,"EVENTWATCH STATUS active=%u cancel=%u phase=%u error=%"PRIi32" retries=%"PRIu32" events=%"PRIu32,
                eventwatch_owned,eventwatch_cancel,eventwatch_status.phase,eventwatch_status.error,eventwatch_status.retries,eventwatch_status.events);
            xSemaphoreGive(node_lock);
        } else if (!strcmp(line,"eventwatch cancel")) {
            xSemaphoreTake(node_lock,portMAX_DELAY);eventwatch_cancel=eventwatch_owned;
            ESP_LOGI(TAG,"EVENTWATCH cancel pending=%u",eventwatch_owned);xSemaphoreGive(node_lock);
        } else if (!strcmp(line, "eventwatch")) {
            /* Bounded bench diagnostic: owns the node so normal polling cannot
             * auto-stop or consume test events. No lap/business state changes. */
            if (!diagnostic_lock()) {
                ESP_LOGW(TAG, "EVENTWATCH busy"); continue;
            }
            eventwatch_owned=true;eventwatch_cancel=false;
            memset(&eventwatch_status,0,sizeof(eventwatch_status));eventwatch_status.phase=1;
            if(xTaskCreate(eventwatch_task,"eventwatch",3072,NULL,3,NULL)!=pdPASS){eventwatch_owned=false;eventwatch_status.phase=5;eventwatch_status.error=ESP_ERR_NO_MEM;ESP_LOGW(TAG,"EVENTWATCH allocation failed");}
            else ESP_LOGI(TAG,"EVENTWATCH started; cancel with eventwatch cancel");
            xSemaphoreGive(node_lock);
        } else if (!strcmp(line, "autogain") || !strcmp(line, "autogain cancel")) {
            esp_err_t err = controller_auto_gain(!strcmp(line, "autogain"));
            ESP_LOGI(TAG, "AUTO_GAIN command=%s result=%s", line, esp_err_to_name(err));
        } else if (!strncmp(line, "gain ", 5)) {
            unsigned gain; char extra;
            if (sscanf(line + 5, "%u %c", &gain, &extra) != 1 || gain > 63) {
                ESP_LOGW(TAG, "GAIN invalid: use gain 0..63");
                continue;
            }
            node_config_t cfg = {s.channel, (uint8_t)gain, s.enter_power, s.exit_power, s.confirm_ms};
            esp_err_t err = controller_set_config(&cfg);
            ESP_LOGI(TAG, "GAIN value=%u result=%s; calibration cleared on success", gain, esp_err_to_name(err));
        } else if (!strcmp(line, "check")) {
            if (s.running || !s.node_online || s.total_laps || s.auto_gain_state == 1) { ESP_LOGW(TAG,"CHECK refused: running, offline, auto gain active or saved laps present"); continue; }
            node_config_t original = {s.channel,s.gain,s.enter_power,s.exit_power,s.confirm_ms};
            unsigned failures = 0;
            for (unsigned channel=0; channel<64; ++channel) {
                node_config_t cfg = original; cfg.channel=channel;
                esp_err_t err = controller_set_config(&cfg);
                ESP_LOGI(TAG,"CHECK channel=%u result=%s",channel,esp_err_to_name(err));
                failures += err != ESP_OK;
            }
            node_config_t cfg=original; cfg.gain=original.gain == 51 ? 52 : 51;
            cfg.enter_power=30; cfg.exit_power=15; cfg.confirm_ms=40;
            esp_err_t err=controller_set_config(&cfg);
            ESP_LOGI(TAG,"CHECK config_fields=%s",esp_err_to_name(err)); failures += err != ESP_OK;
            cfg=original; cfg.channel=64;
            err=controller_set_config(&cfg);
            ESP_LOGI(TAG,"CHECK invalid_channel_rejected=%u",err != ESP_OK); failures += err == ESP_OK;
            err=controller_set_config(&original); failures += err != ESP_OK;
            err=controller_set_calibrated(true); failures += err != ESP_OK;
            ESP_LOGI(TAG,"CHECK calibrated=%s",esp_err_to_name(err));
            err=controller_set_running(true); failures += err != ESP_OK;
            ESP_LOGI(TAG,"CHECK start=%s",esp_err_to_name(err));
            err=controller_set_running(false); failures += err != ESP_OK;
            ESP_LOGI(TAG,"CHECK stop=%s",esp_err_to_name(err));
            err=controller_scan(); failures += err != ESP_OK;
            ESP_LOGI(TAG,"CHECK scan=%s",esp_err_to_name(err));
            controller_get_snapshot(&s);
            if (err == ESP_OK) for(unsigned i=0;i<64;i++) ESP_LOGI(TAG,"SCAN channel=%u power=%u",i,s.scan_power[i]);
            err=controller_set_config(&original); failures += err != ESP_OK;
            err=controller_set_calibrated(false); failures += err != ESP_OK;
            ESP_LOGI(TAG,"CHECK DONE failures=%u restored_channel=%u; calibration cleared",failures,original.channel);
        } else ESP_LOGI(TAG,"Commands: diag, gain 0..63 (saved; clears calibration), check (64 channels/config/calibration/start/stop/scan; clears calibration)");
    }
}
