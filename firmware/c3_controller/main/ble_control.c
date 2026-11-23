/* SPDX-License-Identifier: GPL-3.0-only */
#include "ble_control.h"
#include "network.h"
#include "io_service.h"
#include <string.h>
#include <stdlib.h>
#include "controller_state.h"
#include "config_transfer.h"
#include <stddef.h>
/* Full draft transfer layout consumed by the current miniapp encoder. Fail the
 * build on ABI drift instead of silently accepting another field arrangement. */
_Static_assert(sizeof(game_pilot_t)==28 && offsetof(game_pilot_t,color)==4 && offsetof(game_pilot_t,name)==8,"pilot transfer layout");
_Static_assert(sizeof(game_config_t)==248 && offsetof(game_config_t,minimum_lap_ms)==4 && offsetof(game_config_t,freshness_ms)==16 && offsetof(game_config_t,solo_drain_percent)==20 && offsetof(game_config_t,solo_recover_percent)==22 && offsetof(game_config_t,pilots)==24,"config transfer layout");
#include "ble_connection.h"
#include "ble_authorization.h"
#include "ble_bond_command.h"
#include "network_provision.h"
#include "lap_timer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
void ble_store_config_init(void);
#include "os/os_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_control";
static uint8_t own_addr_type;
static uint32_t connection_generation;
static portMUX_TYPE command_lock = portMUX_INITIALIZER_UNLOCKED;
static bool command_authorized;
static void revoke_command_authorization(void){
    taskENTER_CRITICAL(&command_lock);
    command_authorized=false;
    if(!++connection_generation)++connection_generation;
    taskEXIT_CRITICAL(&command_lock);
}
static ble_authorization_t authorization,repair_window;
static bool authorized;
static bool admission_local;
static bool admission_known;
static bool ble_started;
/* Fail closed for this boot: the store may retain failed writes in RAM. */
static bool security_store_failed;
static ble_store_write_fn *security_store_write;
static int checked_security_store_write(int type,const union ble_store_value *value){
    int error=security_store_write?security_store_write(type,value):BLE_HS_ENOTSUP;
    if(error && (type==BLE_STORE_OBJ_TYPE_OUR_SEC || type==BLE_STORE_OBJ_TYPE_PEER_SEC)){
        taskENTER_CRITICAL(&command_lock);security_store_failed=true;taskEXIT_CRITICAL(&command_lock);
        revoke_command_authorization();
        ESP_LOGE(TAG,"BLE key persistence failed type=%d error=%d; BLE authorization blocked until reboot",type,error);
    }
    return error;
}
bool ble_control_generation_authorized(uint32_t generation){
    taskENTER_CRITICAL(&command_lock);
    bool allowed=ble_command_admitted(generation,connection_generation,command_authorized,security_store_failed);
    taskEXIT_CRITICAL(&command_lock);
    return allowed;
}
static uint16_t connected = BLE_HS_CONN_HANDLE_NONE;
static uint16_t lap_offset;
static uint8_t scan_offset, game_page;
static uint16_t game_lap_offset;
static network_provision_t provisioning;
static uint16_t network_page;
static game_config_t incoming_config;
static config_transfer_t incoming_transfer;
void controller_config_transfer_reset(void){config_transfer_reset(&incoming_transfer);}
/* Called under network control_lock, never from the GAP callback. */
void controller_ble_transaction_reset(void){
 controller_config_transfer_reset();memset(&incoming_config,0,sizeof(incoming_config));
 network_provision_reset(&provisioning);
}
static QueueHandle_t command_queue;

static struct ble_npl_callout security_timeout;
static int64_t security_started_us;
static void security_timeout_event(struct ble_npl_event *event){
    (void)event;
    if(connected==BLE_HS_CONN_HANDLE_NONE || authorized)return;
    int64_t now=esp_timer_get_time();
    if(ble_authorization_expired(security_started_us,now,authorized)){
        ESP_LOGW(TAG,"BLE encryption handshake timed out; disconnecting unauthorized client");
        ble_gap_terminate(connected,BLE_ERR_REM_USER_CONN_TERM);
    }else{
        /* A queued old timer must not shorten a newer connection's deadline. */
        uint32_t remaining=(uint32_t)((BLE_AUTH_HANDSHAKE_US-(now-security_started_us)+999)/1000);
        if(ble_npl_callout_reset(&security_timeout,ble_npl_time_ms_to_ticks32(remaining)))
            ble_gap_terminate(connected,BLE_ERR_REM_USER_CONN_TERM);
    }
}
static struct ble_npl_event bond_event;
static bool bond_busy,bond_delete;
static ble_addr_t bond_target;
static void bond_maintenance(struct ble_npl_event *event){
    (void)event;
    bool maintenance=false;
    if(bond_delete && connected!=BLE_HS_CONN_HANDLE_NONE){
        ESP_LOGW(TAG,"BLE bond deletion refused: finish match and disconnect phone");goto done;
    }
    if(bond_delete){
        esp_err_t admitted=controller_maintenance_begin();
        if(admitted!=ESP_OK){ESP_LOGW(TAG,"BLE bond deletion refused: maintenance=%s",esp_err_to_name(admitted));goto done;}
        maintenance=true;
    }
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];int count=0;
    int error=ble_store_util_bonded_peers(peers,&count,CONFIG_BT_NIMBLE_MAX_BONDS);
    if(error){ESP_LOGE(TAG,"BLE bond list error=%d",error);goto done;}
    if(!bond_delete){
        ESP_LOGI(TAG,"BLE bonds count=%d capacity=%d",count,CONFIG_BT_NIMBLE_MAX_BONDS);
        for(int i=0;i<count;i++)ESP_LOGI(TAG,"BLE bond identity=%u %02x:%02x:%02x:%02x:%02x:%02x",peers[i].type,peers[i].val[5],peers[i].val[4],peers[i].val[3],peers[i].val[2],peers[i].val[1],peers[i].val[0]);
    }else{
        bool found=false;
        for(int i=0;i<count;i++)if(peers[i].type==bond_target.type && !memcmp(peers[i].val,bond_target.val,6))found=true;
        if(!found){ESP_LOGW(TAG,"BLE bond deletion refused: identity not found");goto done;}
        error=ble_store_util_delete_peer(&bond_target);
        if(!error){
            /* Close any existing local permission; a new phone needs explicit admission. */
            taskENTER_CRITICAL(&command_lock);authorization.open=false;repair_window.open=false;taskEXIT_CRITICAL(&command_lock);
        }
        ESP_LOGI(TAG,"BLE bond delete result=%d; use blepair before pairing again",error);
    }
 done:
    if(maintenance)controller_maintenance_end();
    taskENTER_CRITICAL(&command_lock);bond_busy=false;taskEXIT_CRITICAL(&command_lock);
}
esp_err_t ble_control_bonds(const char *identity){
    if(!ble_started || network_role()==NET_MEMBER)return ESP_ERR_INVALID_STATE;
    ble_addr_t target={0};
    if(identity && !ble_bond_parse(identity,&target.type,target.val))return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&command_lock);
    if(bond_busy){taskEXIT_CRITICAL(&command_lock);return ESP_ERR_INVALID_STATE;}
    bond_busy=true;bond_delete=identity!=NULL;bond_target=target;
    taskEXIT_CRITICAL(&command_lock);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&bond_event);
    return ESP_OK;
}
esp_err_t ble_control_open_authorization(void){
    if(!ble_started || security_store_failed || network_role()==NET_MEMBER)return ESP_ERR_INVALID_STATE;
    uint8_t state,owner,fault;
    if(!controller_game_feedback(&state,&owner,&fault))return ESP_ERR_TIMEOUT;
    if(state==1 || state==2)return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&command_lock);
    ble_authorization_open(&authorization,esp_timer_get_time());
    repair_window.open=false;
    taskEXIT_CRITICAL(&command_lock);
    ESP_LOGI(TAG,"Local BLE authorization open for 120s; reconnect phone");
    return ESP_OK;
}
esp_err_t ble_control_open_repair(void){
    esp_err_t error=ble_control_open_authorization();
    if(error!=ESP_OK)return error;
    taskENTER_CRITICAL(&command_lock);
    ble_authorization_open(&repair_window,esp_timer_get_time());
    taskEXIT_CRITICAL(&command_lock);
    ESP_LOGW(TAG,"Local BLE repair open for 120s; next idle repeat pairing may replace that peer's keys");
    return ESP_OK;
}
static bool connection_authorized(uint16_t conn){
    struct ble_gap_conn_desc desc;
    if(security_store_failed || !authorized || !ble_connection_matches(connected,conn) || ble_gap_conn_find(conn,&desc))return false;
    return ble_authorization_secure(true,desc.sec_state.encrypted,desc.sec_state.bonded,desc.sec_state.key_size);
}
static uint8_t last_request[20], last_length, command_state;
static uint32_t last_request_generation;
static esp_err_t command_error;
static uint16_t telemetry_handle;
static bool telemetry_subscribed;
static uint32_t telemetry_sequence;
typedef struct { uint8_t bytes[20], length; uint32_t generation; } command_job_t;
/* Exactly 16 little-endian bytes, shared by every characteristic. */
#define C3_UUID(suffix) BLE_UUID128_INIT(suffix,0x7e,0xc5,0x00,0x00,0x00,0x40,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0xc5,0xc3)
static const ble_uuid128_t service_uuid = C3_UUID(0x10);
static const ble_uuid128_t status_uuid = C3_UUID(0x11);
static const ble_uuid128_t command_uuid = C3_UUID(0x12);
static const ble_uuid128_t extended_uuid = C3_UUID(0x13);
static const ble_uuid128_t laps_uuid = C3_UUID(0x14);
static const ble_uuid128_t scan_uuid = C3_UUID(0x15);
static const ble_uuid128_t device_uuid = C3_UUID(0x16);

static const ble_uuid128_t result_uuid = C3_UUID(0x17);
static const ble_uuid128_t telemetry_uuid = C3_UUID(0x18);

static const ble_uuid128_t game_uuid = C3_UUID(0x19);
static const ble_uuid128_t network_uuid = C3_UUID(0x1a);
static void put32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}

static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    controller_snapshot_t snapshot;
    controller_get_snapshot(&snapshot);
    uint8_t packet[40] = {0};
    packet[0] = snapshot.protocol_version;
    packet[1] = snapshot.node_online;
    packet[2] = snapshot.rf_valid;
    packet[3] = snapshot.capabilities; /* Bit 0: timing available; currently zero. */
    put32(packet + 4, snapshot.boot_session);
    put32(packet + 8, snapshot.windows);
    put32(packet + 12, snapshot.valid_windows);
    put32(packet + 16, snapshot.receive_errors);
    put32(packet + 20, snapshot.raw_power);
    put32(packet + 24, snapshot.clip_pm);
    put32(packet + 28, snapshot.spi_errors);
    return os_mbuf_append(ctxt->om, packet, 32) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

esp_err_t controller_execute_command(const uint8_t *command, uint16_t length)
{
    if(!command || length<2 || length>16 || command[0]!=1)return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if(command[1]==53)return length==4 && command[2]<=1?controller_finder(command[2],command[3]):ESP_ERR_INVALID_ARG;
    if(command[1]==50 && length==4)return controller_slot_capture(command[2],command[3]);
    if(command[1]==47 && length==11){node_config_t config;memcpy(&config,command+3,8);return controller_slot_config(command[2],&config);}
    if (command[1] == 1 && length == 10) {
        node_config_t config = {
            .channel = command[2], .gain = command[3],
            .enter_power = command[4] | ((uint16_t)command[5] << 8),
            .exit_power = command[6] | ((uint16_t)command[7] << 8),
            .confirm_ms = command[8] | ((uint16_t)command[9] << 8),
        };
        err = controller_set_config(&config);
    } else if (command[1] == 2 && length == 3 && command[2] <= 1) {
        err = controller_set_calibrated(command[2] != 0);
    } else if (command[1] == 3 && length == 3 && command[2] <= 1) {
        err = controller_set_running(command[2] != 0);
    } else if (command[1] == 4 && length == 4) {
        uint16_t offset = command[2] | ((uint16_t)command[3] << 8);
        if (offset < LAP_RECENT_MAX) { lap_offset = offset; err = ESP_OK; }
    } else if (command[1] == 5 && length == 2) {
        err = controller_scan();
    } else if (command[1] == 6 && length == 3 && command[2] < 64 && command[2] % 8 == 0) {
        scan_offset = command[2]; err = ESP_OK;
    } else if (command[1] == 7 && length == 3 && command[2] <= 1) {
        err = controller_auto_gain(command[2] != 0);
    }
    if(command[1]==8 && length==11) {
        uint32_t duration=(uint32_t)command[5]|((uint32_t)command[6]<<8)|((uint32_t)command[7]<<16)|((uint32_t)command[8]<<24);
        err=controller_game_rules(command[2],command[3]|((uint16_t)command[4]<<8),duration,command[9]|((uint16_t)command[10]<<8));
    } else if(command[1]==52 && length==15){
        uint32_t minimum,duration,capture;memcpy(&minimum,command+3,4);memcpy(&duration,command+7,4);memcpy(&capture,command+11,4);
        err=controller_game_rules(command[2],minimum,duration,capture);
    } else if(command[1]==9 && length==10 && command[2]<GAME_PILOTS) {
        game_config_t config;
        if(controller_game_config_get(&config)) {
            game_pilot_t p=config.pilots[command[2]];p.enabled=command[3];p.channel=command[4];p.team=command[5];p.slot=command[6];
            p.color=command[7]|((uint32_t)command[8]<<8)|((uint32_t)command[9]<<16);err=controller_game_pilot(command[2],&p);
        } else err=ESP_ERR_TIMEOUT;
    } else if(command[1]==10 && length==3)err=controller_game_action(command[2]);
    else if(command[1]==11 && length==3 && command[2]<131){game_page=command[2];err=ESP_OK;}
    else if(command[1]==13 && length==4){uint16_t offset=command[2]|((uint16_t)command[3]<<8);if(offset<200){game_lap_offset=offset;err=ESP_OK;}}
    else if(command[1]==12 && length>=4 && length<=15 && command[2]<GAME_PILOTS){
        char name[13]={0};memcpy(name,command+3,length-3);err=memchr(command+3,0,length-3)?ESP_ERR_INVALID_ARG:controller_game_name(command[2],name);
    }
    if(command[1]==14 && length==8){
        uint32_t network;memcpy(&network,command+4,4);
        if(network_provision_begin(&provisioning,command[2],command[3],network,esp_timer_get_time()))err=ESP_OK;
    }else if(command[1]==15 && length==11){
        if(network_provision_key(&provisioning,command[2],command+3,esp_timer_get_time()))err=ESP_OK;
    }else if(command[1]==16 && length==8){
        if(network_provision_host(&provisioning,command+2,esp_timer_get_time()))err=ESP_OK;
    }else if(command[1]==17 && length==2){
        network_config_t target={0};
        if(network_provision_take(&provisioning,&target,esp_timer_get_time()))err=network_configure_locked(&target);
        memset(&target,0,sizeof(target));
    }
    else if(command[1]==18 && length==3 && command[2]<=244){network_page=command[2];err=ESP_OK;}
    else if(command[1]==46 && length==4){unsigned page=command[2]|((unsigned)command[3]<<8);if(page>=256 && page<=416){network_page=page;err=ESP_OK;}}
    else if(command[1]==38 && length==5 && command[2]<=1 && command[3]<=1)err=io_service_configure(command[2],command[3],command[4]);
    else if(command[1]==37 && length==2)err=network_capture_result_pages();
    else if(command[1]==19 && length==2)err=network_sync();
    else if(command[1]==20 && length>=5 && length<=16){
        unsigned offset=command[2]|((unsigned)command[3]<<8),size=length-4;
        if(config_transfer_chunk(&incoming_transfer,&incoming_config,sizeof(incoming_config),offset,command+4,size,esp_timer_get_time()))err=ESP_OK;
    } else if(command[1]==21 && length==6){
        uint32_t expected;memcpy(&expected,command+2,4);
        if(config_transfer_commit(&incoming_transfer,&incoming_config,sizeof(incoming_config),expected,esp_timer_get_time()))err=controller_game_config(&incoming_config);
    }
    if(command[1]==22 && length==10){node_config_t config={.channel=command[2],.gain=command[3],.enter_power=command[4]|((uint16_t)command[5]<<8),.exit_power=command[6]|((uint16_t)command[7]<<8),.confirm_ms=command[8]|((uint16_t)command[9]<<8)};err=controller_stage_config(&config);}
    else if(command[1]==23 && length==3 && command[2]<=1)err=controller_game_ready(command[2]);
    if(command[1]==24 && length==6){uint32_t session;memcpy(&session,command+2,4);err=controller_game_start_session(session);}
    if(command[1]==31 && length==8)err=controller_game_tuning(command[2]|command[3]<<8,command[4]|command[5]<<8,command[6]|command[7]<<8);
    if(command[1]==43 && length==3 && command[2]<=1)err=controller_event_acquisition(command[2]);
    if(command[1]==42 && length==2)err=controller_event_capture();
    if(command[1]==41 && length==2)err=controller_game_save_result();
    return err;
}

/* Only the latest accepted ID may be retried. A client keeps one outstanding
 * command and never automatically replays an uncertain command after reconnect. */
static int command_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    command_job_t job = {0};
    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length < 6 || length > sizeof(job.bytes) ||
        ble_hs_mbuf_to_flat(ctxt->om, job.bytes, length, NULL)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (job.bytes[0] != 2 || !(job.bytes[2] | job.bytes[3] | job.bytes[4] | job.bytes[5])) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    job.length = length;
    taskENTER_CRITICAL(&command_lock);
    job.generation=connection_generation;
    bool same_id = ble_result_current(last_request_generation,job.generation) && last_length && !memcmp(last_request + 2, job.bytes + 2, 4);
    if (same_id) {
        bool identical = last_length == length && !memcmp(last_request, job.bytes, length);
        taskEXIT_CRITICAL(&command_lock);
        return identical ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    if (command_state == 1) { taskEXIT_CRITICAL(&command_lock); return BLE_ATT_ERR_INSUFFICIENT_RES; }
    memcpy(last_request, job.bytes, length); last_length = length;last_request_generation=job.generation;
    command_state = 1; command_error = ESP_OK;
    taskEXIT_CRITICAL(&command_lock);
    if (xQueueSend(command_queue, &job, 0) != pdTRUE) {
        taskENTER_CRITICAL(&command_lock); command_state = 3; command_error = ESP_ERR_NO_MEM; taskEXIT_CRITICAL(&command_lock);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int result_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t packet[12] = {2};
    taskENTER_CRITICAL(&command_lock);
    if(ble_result_current(last_request_generation,connection_generation)){
        packet[1] = command_state; packet[2] = last_request[1];
        memcpy(packet + 4, last_request + 2, 4); put32(packet + 8, command_error);
    }
    taskEXIT_CRITICAL(&command_lock);
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static void command_worker(void *arg)
{
    (void)arg; command_job_t job;
    for (;;) {
        if (xQueueReceive(command_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        uint8_t command[16] = {1, job.bytes[1]};
        memcpy(command + 2, job.bytes + 6, job.length - 6);
        bool current=ble_control_generation_authorized(job.generation);
        /* Queued old commands are refused; an operation already admitted to
         * network_control may finish after disconnect with uncertain outcome. */
        esp_err_t error=current?network_ble_control(command,job.length-4,job.generation):ESP_ERR_INVALID_STATE;
        taskENTER_CRITICAL(&command_lock);
        command_error = error; command_state = error == ESP_OK ? 2 : 3;
        taskEXIT_CRITICAL(&command_lock);
        ESP_LOGI(TAG, "COMMAND opcode=%u state=%u error=%s", command[1], error == ESP_OK ? 2 : 3, esp_err_to_name(error));
    }
}

static void telemetry_packet(uint8_t packet[20])
{
    controller_snapshot_t s; controller_get_snapshot(&s);
    memset(packet, 0, 20); packet[0] = 2; packet[1] = s.observation_quality;
    packet[2] = s.node_online | (s.rf_valid << 1) | (s.running << 2); packet[3] = s.channel;
    put32(packet + 4, s.boot_session); put32(packet + 8, telemetry_sequence);
    packet[12] = s.observed_power; packet[13] = s.observed_power >> 8;
    packet[14] = s.observed_clip_pm; packet[15] = s.observed_clip_pm >> 8;
    uint64_t age = s.observation_updated_us ? (esp_timer_get_time() - s.observation_updated_us) / 1000 : UINT32_MAX;
    put32(packet + 16, age > UINT32_MAX ? UINT32_MAX : age);
}
static int telemetry_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t packet[20]; telemetry_packet(packet);
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}
static void telemetry_worker(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250)); ++telemetry_sequence;
        if (security_store_failed || connected == BLE_HS_CONN_HANDLE_NONE || !authorized || !telemetry_subscribed) continue;
        uint8_t packet[20]; telemetry_packet(packet);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, sizeof(packet));
        if (om) ble_gatts_notify_custom(connected, telemetry_handle, om);
    }
}

static int scan_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    controller_snapshot_t s; controller_get_snapshot(&s);
    uint8_t packet[18] = {1, s.scan_valid};
    for (unsigned i = 0; i < 8; ++i) { packet[2 + i * 2] = s.scan_power[scan_offset + i]; packet[3 + i * 2] = s.scan_power[scan_offset + i] >> 8; }
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int extended_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    controller_snapshot_t s;
    controller_get_snapshot(&s);
    uint8_t packet[32] = {2, s.running, s.calibrated, s.channel, s.gain};
    packet[5] = s.auto_gain_state;
    packet[6] = s.auto_gain_progress;
    put32(packet + 8, s.total_laps);
    put32(packet + 12, s.last_lap_ms);
    put32(packet + 16, s.fastest_lap_ms);
    put32(packet + 20, s.event_overflows);
    packet[24] = s.observed_power; packet[25] = s.observed_power >> 8;
    packet[26] = s.observed_clip_pm; packet[27] = s.observed_clip_pm >> 8;
    packet[28] = s.enter_power; packet[29] = s.enter_power >> 8;
    packet[30] = s.exit_power; packet[31] = s.exit_power >> 8;
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Device-service status fits the default ATT MTU. */
static int device_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    controller_snapshot_t s; controller_get_snapshot(&s);
    uint8_t packet[20] = {1, s.observation_quality, s.node_online, s.rf_valid};
    packet[4] = s.confirm_ms; packet[5] = s.confirm_ms >> 8;
    uint64_t elapsed = s.observation_updated_us ?
        (esp_timer_get_time() - s.observation_updated_us) / 1000u : UINT32_MAX;
    put32(packet + 6, elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
    put32(packet + 10, s.observation_samples);
    put32(packet + 14, (uint32_t)s.config_save_error);
    packet[18] = 5; /* C5 contract required by this controller. */
    packet[19] = 2; /* C3 device-service revision. */
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int laps_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t packet[22] = {2, 0};
    controller_snapshot_t snapshot;
    controller_get_snapshot(&snapshot);
    packet[2] = lap_offset; packet[3] = lap_offset >> 8;
    put32(packet + 4, snapshot.total_laps);
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t lap_ms = 0;
        if (!controller_get_lap(lap_offset + i, &lap_ms)) break;
        put32(packet + 8 + i * 4, lap_ms);
        ++packet[1];
    }
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int game_access(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    (void)attr;(void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if(ctxt->op!=BLE_GATT_ACCESS_OP_READ_CHR)return BLE_ATT_ERR_UNLIKELY;
    uint8_t p[32];
    if(!controller_game_page(game_page,game_lap_offset,p))return BLE_ATT_ERR_INSUFFICIENT_RES;
    return os_mbuf_append(ctxt->om,p,sizeof(p))?BLE_ATT_ERR_INSUFFICIENT_RES:0;
}

static int network_access(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg){
    (void)attr;(void)arg;
    if(!connection_authorized(conn))return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    if(ctxt->op!=BLE_GATT_ACCESS_OP_READ_CHR)return BLE_ATT_ERR_UNLIKELY;
    uint8_t packet[32];network_status(packet,network_page);return os_mbuf_append(ctxt->om,packet,sizeof(packet))?BLE_ATT_ERR_INSUFFICIENT_RES:0;
}
static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {.uuid = &status_uuid.u, .access_cb = status_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &command_uuid.u, .access_cb = command_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
            {.uuid = &extended_uuid.u, .access_cb = extended_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &laps_uuid.u, .access_cb = laps_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &scan_uuid.u, .access_cb = scan_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &device_uuid.u, .access_cb = device_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &result_uuid.u, .access_cb = result_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &telemetry_uuid.u, .access_cb = telemetry_access, .val_handle = &telemetry_handle, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY},
            {.uuid = &game_uuid.u, .access_cb = game_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {.uuid = &network_uuid.u, .access_cb = network_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
            {0},
        },
    },
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        ESP_LOGI(TAG,"GAP connect status=%d handle=%u",event->connect.status,event->connect.conn_handle);
        if (event->connect.status == 0) {
            uint16_t handle=event->connect.conn_handle;
            if(security_store_failed || !ble_connection_accept(connected,handle)){
                ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);return 0;
            }
            revoke_command_authorization();
            struct ble_gap_conn_desc desc;
            struct ble_store_value_sec security={0};
            bool known=false;
            if(!ble_gap_conn_find(handle,&desc)){
                struct ble_store_key_sec key={.peer_addr=desc.peer_id_addr};
                known=!ble_store_read_our_sec(&key,&security) && security.ltk_present && security.key_size==16 && security.sc;
                memset(&security,0,sizeof(security));
            }
            taskENTER_CRITICAL(&command_lock);
            bool local=known?false:ble_authorization_take(&authorization,esp_timer_get_time());
            taskEXIT_CRITICAL(&command_lock);
            if(!known && !local){ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);return 0;}
            connected=handle;admission_local=local;admission_known=known;authorized=false;telemetry_subscribed=false;
            security_started_us=esp_timer_get_time();
            if(ble_npl_callout_reset(&security_timeout,ble_npl_time_ms_to_ticks32(30000))){
                ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);return 0;
            }
            int security_error=ble_gap_security_initiate(handle);
            if(security_error && security_error!=BLE_HS_EALREADY){ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);return 0;}
            lap_offset=0;scan_offset=0;game_page=0;game_lap_offset=0;network_page=0;
        }else advertise();
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGW(TAG,"GAP disconnect reason=%d",event->disconnect.reason);
        if(ble_connection_matches(connected,event->disconnect.conn.conn_handle)){
            ble_npl_callout_stop(&security_timeout);
            revoke_command_authorization();
            telemetry_subscribed=false;authorized=false;admission_local=false;admission_known=false;connected=BLE_HS_CONN_HANDLE_NONE;
        }
        if(connected==BLE_HS_CONN_HANDLE_NONE)advertise();

    }
    if(event->type==BLE_GAP_EVENT_REPEAT_PAIRING){
        uint16_t handle=event->repeat_pairing.conn_handle;
        uint8_t state,owner,fault;
        if(!ble_connection_matches(connected,handle) || !controller_game_feedback(&state,&owner,&fault))return BLE_GAP_REPEAT_PAIRING_IGNORE;
        /* Retain the controller lease across key deletion. A phase snapshot
         * alone permits a match start between the check and the mutation. */
        if(controller_maintenance_begin()!=ESP_OK)return BLE_GAP_REPEAT_PAIRING_IGNORE;
        taskENTER_CRITICAL(&command_lock);
        bool repair=ble_authorization_repair(&repair_window,esp_timer_get_time(),true,state);
        taskEXIT_CRITICAL(&command_lock);
        if(!repair){controller_maintenance_end();return BLE_GAP_REPEAT_PAIRING_IGNORE;}
        revoke_command_authorization();
        authorized=false;
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(handle,&desc) || ble_store_util_delete_peer(&desc.peer_id_addr)){
            controller_maintenance_end();
            ESP_LOGE(TAG,"BLE repair could not remove peer keys; pairing refused");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        controller_maintenance_end();
        authorized=false;admission_known=false;admission_local=true;
        security_started_us=esp_timer_get_time();
        if(ble_npl_callout_reset(&security_timeout,ble_npl_time_ms_to_ticks32(30000))){
            ble_gap_terminate(handle,BLE_ERR_REM_USER_CONN_TERM);return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ESP_LOGW(TAG,"BLE repair consumed; re-pairing current peer");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    if(event->type==BLE_GAP_EVENT_ENC_CHANGE && ble_connection_matches(connected,event->enc_change.conn_handle)){
        struct ble_gap_conn_desc desc;
        authorized=ble_authorization_storage_ok(security_store_failed,!event->enc_change.status && !ble_gap_conn_find(connected,&desc) &&
            ble_authorization_secure(admission_local || admission_known,desc.sec_state.encrypted,desc.sec_state.bonded,desc.sec_state.key_size));
        taskENTER_CRITICAL(&command_lock);command_authorized=authorized;taskEXIT_CRITICAL(&command_lock);
        if(!authorized)revoke_command_authorization();
        ESP_LOGI(TAG,"BLE encrypted authorization=%u local=%u",authorized,admission_local);
        ble_npl_callout_stop(&security_timeout);
        if(!authorized)ble_gap_terminate(connected,BLE_ERR_REM_USER_CONN_TERM);
    }
    if (event->type == BLE_GAP_EVENT_CONN_UPDATE){
        struct ble_gap_conn_desc desc;
        if(!ble_gap_conn_find(event->conn_update.conn_handle,&desc))ESP_LOGI(TAG,"GAP update status=%d interval=%u latency=%u timeout=%u",event->conn_update.status,desc.conn_itvl,desc.conn_latency,desc.supervision_timeout);
    }
    if (event->type == BLE_GAP_EVENT_SUBSCRIBE && event->subscribe.attr_handle == telemetry_handle && ble_connection_matches(connected,event->subscribe.conn_handle)){
        telemetry_subscribed = event->subscribe.cur_notify;
        ESP_LOGI(TAG,"GAP telemetry notify=%u",telemetry_subscribed);
    }
    return 0;
}

static void advertise(void)
{
    if (connected != BLE_HS_CONN_HANDLE_NONE) return;
    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name = (const uint8_t *)name,
        .name_len = strlen(name),
        .name_is_complete = 1,
    };
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "advertisement fields: %d", rc); return; }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "advertisement start: %d", rc);
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &own_addr_type) == 0) advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_control_start(void)
{
    command_queue = xQueueCreate(1, sizeof(command_job_t));
    if (!command_queue) return ESP_ERR_NO_MEM;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_svc_gap_device_name_set("C5-VRX-CTF") != 0) return ESP_FAIL;
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0) return ESP_FAIL;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap=BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding=1;
    ble_hs_cfg.sm_mitm=0; /* Physical admission + Just Works; no MITM claim. */
    ble_hs_cfg.sm_sc=1;ble_hs_cfg.sm_sc_only=1;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    /* No round-robin store callback: a full bond table must not evict an owner. */
    ble_store_config_init();
    security_store_write=ble_hs_cfg.store_write_cb;
    ble_hs_cfg.store_write_cb=checked_security_store_write;
    if (xTaskCreate(command_worker, "ble_command", 8192, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(telemetry_worker, "ble_telemetry", 3072, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ble_npl_event_init(&bond_event,bond_maintenance,NULL);
    ble_npl_callout_init(&security_timeout,nimble_port_get_dflt_eventq(),security_timeout_event,NULL);
    ble_started=true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
