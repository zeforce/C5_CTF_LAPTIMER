#include "ble_control.h"
#include "ble_connection.h"
#include "network.h"
#include "command_policy.h"
#include "event_transport.h"
#include "event_mux.h"
#include "network_events.h"
#include "member_stop.h"
#include "storage_fault.h"
#include "io_service.h"
#include "network_roster.h"
#include "controller_state.h"
#include "match.h"
#include "match_snapshot.h"
#include "match_verdict.h"
#include "result_record.h"
#include "result_settle.h"
#include "network_guard.h"
#include "network_sequence.h"
#include "network_identity.h"
#include "network_score.h"
#include "network_clock.h"
#include "start_schedule.h"
#include "schedule_abort.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <inttypes.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_coexist.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
typedef struct __attribute__((packed)) {
 uint8_t version,type;uint32_t network,boot,seq;uint8_t length,data[32],tag[16];
} packet_t;
typedef struct {uint8_t mac[6];packet_t p;int64_t received_us;} received_t;
typedef struct {uint8_t mac[6];uint32_t boot,last_seq;uint64_t seen;int32_t error;uint8_t state,mode,owner,fault,online,rf,calibrated;uint32_t session,red_ms,blue_ms,pilot_ms[8],pilot_laps[8],pilot_fastest_ms[8],pilot_captures[8];uint64_t snapshot_seen,pilot_seen[8];uint32_t pilot_session;node_config_t rf_config;uint64_t config_seen,timing_seen;uint32_t timing_seq,timing_session,timing_uncertainty;uint8_t timing_valid,timing_phase;uint8_t acquisition[12];uint32_t acquisition_seq;uint64_t acquisition_seen;uint8_t feedback[32];uint32_t feedback_seq;uint64_t feedback_seen;uint8_t outcome[24];uint32_t outcome_seq,outcome_session;uint64_t outcome_seen;packet_t reply,request;} member_t;
static network_config_t config;
static network_identity_t identities[8];
/* Public committed settings only; never duplicate or expose the shared key. */
static uint8_t saved_network_page[32]={31};

static event_receiver_t event_receivers[8];
static event_host_clock_t event_clocks[8];
static uint32_t event_clock_sequence[8];
static packet_t event_pending;
static event_sender_t event_sender;
static event_sender_t bench_sender;
static event_receiver_t bench_receivers[8];
static packet_t bench_packet;
static uint16_t bench_requested,bench_total;
static int64_t bench_deadline,bench_drop_until;
static uint32_t bench_dropped,bench_unknown,bench_boot_mismatch,bench_stale,bench_ack_invalid;
static uint32_t diagnostic_tx_errors,heartbeat_skips,rx_delay_max_us;
static bool bench_running;
static member_stop_t member_stop;
static network_events_t network_events;
static network_events_snapshot_t network_events_snapshot;
static uint64_t local_event_cursor[4];
static unsigned local_event_next;
static event_mux_t member_events;
static uint8_t member_slot_config[8][4][32];
static uint32_t member_slot_seq[8][4];
static uint64_t member_slot_seen[8][4];
static member_t members[8];static unsigned count;
static bool roster_fixed;static esp_err_t roster_error;
static QueueHandle_t rx;static uint32_t boot_id,sequence;static uint8_t local_mac[6];
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static void saved_network_update(const network_config_t *c,bool pending){
 uint8_t page[32]={31,1,c->role,c->channel};memcpy(page+4,&c->network,4);memcpy(page+8,c->host,6);page[14]=pending;
 taskENTER_CRITICAL(&lock);memcpy(saved_network_page,page,32);taskEXIT_CRITICAL(&lock);
}
static mbedtls_svc_key_id_t auth_key;
static uint64_t last_host_seen,last_host_heartbeat_seen;
static uint32_t last_host_boot, last_host_heartbeat_seq;
static SemaphoreHandle_t control_lock;
static int64_t operation_deadline;
static bool reboot_pending;
static void reboot_worker(void *arg){(void)arg;vTaskDelay(pdMS_TO_TICKS(3000));esp_restart();}
static uint32_t queue_drops;
static int64_t drop_ack_until;
static uint32_t command_executed, duplicate_replies, ack_dropped, stale_commands;
static uint32_t clock_probes,clock_replies,clock_selected,clock_rejected,clock_last_rtt,clock_max_rtt;
void network_drop_next_ack(void){taskENTER_CRITICAL(&lock);drop_ack_until=esp_timer_get_time()+30000000;taskEXIT_CRITICAL(&lock);}
esp_err_t network_event_bench(unsigned count){
 controller_snapshot_t status;controller_get_snapshot(&status);
 if(status.running || status.auto_gain_state==1 || config.role!=NET_MEMBER)return ESP_ERR_INVALID_STATE;
 if(count<1 || count>100)return ESP_ERR_INVALID_ARG;
 taskENTER_CRITICAL(&lock);
 bool busy=bench_requested || bench_running;
 if(!busy)bench_requested=count;
 taskEXIT_CRITICAL(&lock);return busy?ESP_ERR_INVALID_STATE:ESP_OK;
}
esp_err_t network_bench_drop_ack(void){
 if(config.role!=NET_HOST)return ESP_ERR_INVALID_STATE;
 taskENTER_CRITICAL(&lock);bench_drop_until=esp_timer_get_time()+30000000;taskEXIT_CRITICAL(&lock);return ESP_OK;
}
void network_diagnostics(void){
 ESP_LOGI("network","CLOCKPATH probes=%"PRIu32" replies=%"PRIu32" selected=%"PRIu32" rejected=%"PRIu32" last_rtt_us=%"PRIu32" max_rtt_us=%"PRIu32,clock_probes,clock_replies,clock_selected,clock_rejected,clock_last_rtt,clock_max_rtt);
 for(unsigned i=0;i<count;i++){
  event_host_clock_t c;taskENTER_CRITICAL(&lock);c=event_clocks[i];taskEXIT_CRITICAL(&lock);
  uint64_t sampled=0;bool mapped=c.valid && event_host_add(c.sampled_us,c.offset_us,&sampled);uint64_t now=esp_timer_get_time();
  ESP_LOGI("network","EVENTCLOCK member=%u valid=%u boot=%"PRIu32" host=%"PRIu32" offset_us=%"PRIi64" uncertainty_us=%"PRIu32" age_ms=%"PRIu64,i,mapped && c.host_boot==boot_id && c.uncertainty_us<=20000 && now>=sampled && now-sampled<=10000000,c.member_boot,c.host_boot,c.offset_us,c.uncertainty_us,mapped && now>=sampled?(now-sampled)/1000:UINT64_MAX);
 }

 ESP_LOGI("network","NETPATH unknown=%"PRIu32" boot=%"PRIu32" stale=%"PRIu32" ack_invalid=%"PRIu32" tx_errors=%"PRIu32" heartbeat_skips=%"PRIu32" rx_delay_max_us=%"PRIu32,bench_unknown,bench_boot_mismatch,bench_stale,bench_ack_invalid,diagnostic_tx_errors,heartbeat_skips,rx_delay_max_us);
 ESP_LOGI("network","BENCH running=%u total=%u acked=%"PRIu32" sent=%"PRIu32" retries=%"PRIu32" dropped=%"PRIu32,bench_running,bench_total,bench_sender.acked,bench_sender.sent,bench_sender.retries,bench_dropped);
 for(unsigned i=0;i<count;i++)ESP_LOGI("network","BENCHRX member=%u accepted=%"PRIu32" duplicates=%"PRIu32" rejected=%"PRIu32,i,bench_receivers[i].accepted,bench_receivers[i].duplicates,bench_receivers[i].rejected);
 ESP_LOGI("network","MEMBERSTOP capture=%u game=%u requests=%"PRIu32" attempts=%"PRIu32,member_stop.capture_pending,member_stop.game_pending,member_stop.requests,member_stop.attempts);
 ESP_LOGI("network","EVENTTX waiting=%u sent=%"PRIu32" retries=%"PRIu32" acked=%"PRIu32,event_sender.waiting,event_sender.sent,event_sender.retries,event_sender.acked);
 ESP_LOGI("network","NETDIAG executed=%"PRIu32" duplicates=%"PRIu32" dropped=%"PRIu32" stale=%"PRIu32" queue_drops=%"PRIu32,command_executed,duplicate_replies,ack_dropped,stale_commands,queue_drops);
}
static uint16_t match_roster;
static uint32_t roster_session;
static result_record_t saved_result;
static result_history_t history={.version=1};
static esp_err_t result_save_error;
static uint32_t roster_boot[8];
static uint8_t roster_mac[8][6];
static int64_t roster_started;
static uint32_t heartbeat_sequence[8], config_sequence[8];
static uint32_t pilot_sequence[8][8];
static uint32_t terminal_sequence[8];
static uint8_t read_result_pages[RESULT_PAGES][32];
static uint32_t read_result_revision;
static int64_t read_result_captured;
static start_schedule_t scheduled;
/* Repeat aborts in heartbeats without waiting for the control mutex. */
static uint32_t cancelled_schedule;
static uint32_t cancelled_fence,prepared_sequence;
static int32_t scheduled_error;
static int64_t clock_offset,clock_updated,clock_request;
static uint32_t clock_uncertainty;
static uint32_t rx_count, auth_fail, tx_count, raw_rx, send_ok, send_fail;
static void sent(const esp_now_send_info_t *info,esp_now_send_status_t status){(void)info;if(status==ESP_NOW_SEND_SUCCESS)++send_ok;else ++send_fail;}
static void sign(packet_t *p,uint8_t digest[32]) {
 size_t size=0;
 if(psa_mac_compute(auth_key,PSA_ALG_HMAC(PSA_ALG_SHA_256),(uint8_t*)p,offsetof(packet_t,tag),digest,32,&size)!=PSA_SUCCESS || size!=32)abort();
}
static bool verify(packet_t *p){uint8_t digest[32];sign(p,digest);uint8_t difference=0;for(unsigned i=0;i<16;i++)difference|=digest[i]^p->tag[i];return !difference;}
static void peer(const uint8_t *mac){if(esp_now_is_peer_exist(mac))return;esp_now_peer_info_t p={0};memcpy(p.peer_addr,mac,6);p.channel=config.channel;p.ifidx=WIFI_IF_STA;esp_now_add_peer(&p);}
static void send_packet(const uint8_t *mac,packet_t *p){uint8_t digest[32];sign(p,digest);memcpy(p->tag,digest,16);peer(mac);++tx_count;esp_err_t e=esp_now_send(mac,(uint8_t*)p,sizeof(*p));if(e!=ESP_OK){if(p->type==15 || p->type==16)++diagnostic_tx_errors;ESP_LOGW("network","send type=%u error=%s",p->type,esp_err_to_name(e));}}
static packet_t make_packet(uint8_t type){packet_t p={.version=1,.type=type,.network=config.network,.boot=boot_id};taskENTER_CRITICAL(&lock);p.seq=++sequence;if(!p.seq)p.seq=++sequence;taskEXIT_CRITICAL(&lock);return p;}
static void receive(const esp_now_recv_info_t *info,const uint8_t *bytes,int length){
 ++raw_rx;
 if(length!=sizeof(packet_t))return;
 received_t r;r.received_us=esp_timer_get_time();memcpy(r.mac,info->src_addr,6);memcpy(&r.p,bytes,length);if(xQueueSend(rx,&r,0)!=pdTRUE)++queue_drops;
}
uint8_t network_role(void){return config.role;}
static esp_err_t control_begin(void);
static esp_err_t network_configuration_begin(void);
esp_err_t network_configure(const network_config_t *c){
 esp_err_t e=control_begin();if(e!=ESP_OK)return e;
 e=network_configure_locked(c);xSemaphoreGive(control_lock);return e;
}
esp_err_t network_configure_locked(const network_config_t *c){
 esp_err_t ready=network_configuration_begin();if(ready!=ESP_OK)return ready;
 if(!network_config_valid(c)){controller_maintenance_end();return ESP_ERR_INVALID_ARG;}
 nvs_handle_t h;esp_err_t e=nvs_open("network",NVS_READWRITE,&h);
 if(e!=ESP_OK){controller_maintenance_end();return e;}
 e=nvs_set_blob(h,"config",c,sizeof(*c));
 if(e==ESP_OK && (c->network!=config.network || c->role!=config.role || memcmp(c->key,config.key,16))){esp_err_t erased=nvs_erase_key(h,"roster");if(erased!=ESP_OK && erased!=ESP_ERR_NVS_NOT_FOUND)e=erased;}
 if(e==ESP_OK)e=nvs_commit(h);
 nvs_close(h);
 if(e==ESP_OK)saved_network_update(c,c->role!=config.role || c->channel!=config.channel || c->network!=config.network || memcmp(c->host,config.host,6) || memcmp(c->key,config.key,16));
 controller_maintenance_end();
 return e;
}
static int find_member(const uint8_t *mac){for(unsigned i=0;i<count;i++)if(!memcmp(members[i].mac,mac,6))return i;return -1;}
/* Authenticated nonce exchange admits a boot; no persistent flash writes.
 * The response sequence is a floor, so pre-proof heartbeats cannot admit it. */
static bool identity_admitted(received_t *r,uint32_t established){
 uint64_t now=r->received_us;
 network_identity_t *proof=network_identity_slot(identities,8,r->mac,now);
 if(proof && !memcmp(proof->mac,r->mac,6) && network_identity_admitted(proof,r->p.boot,r->p.seq,now)){proof->verified=false;return true;}
 if(established && established==r->p.boot)return true;
 if(!proof)return false;
 if(!proof->used || memcmp(proof->mac,r->mac,6) || proof->boot!=r->p.boot || now<proof->issued || now-proof->issued>=1000000){
  uint8_t nonce[16];esp_fill_random(nonce,sizeof(nonce));
  network_identity_issue(proof,r->mac,r->p.boot,nonce,now);
  packet_t challenge=make_packet(19);challenge.length=24;
  memcpy(challenge.data,&r->p.boot,4);memcpy(challenge.data+4,&boot_id,4);memcpy(challenge.data+8,nonce,16);
  send_packet(r->mac,&challenge);
 }
 return false;
}
static void process(received_t *r){
 ++rx_count;
 uint64_t delay=esp_timer_get_time()-r->received_us;if(delay>rx_delay_max_us)rx_delay_max_us=delay>UINT32_MAX?UINT32_MAX:delay;
 packet_t *p=&r->p;if(p->version!=1 || p->network!=config.network || !p->seq || p->length>32 || !verify(p)){++auth_fail;return;}
 /* Session proof packets cannot discover members or update liveness. */
 if(p->type==19 || p->type==20){
  if(p->length!=24 || !p->boot)return;
  if(config.role==NET_MEMBER && memcmp(r->mac,config.host,6))return;
  if(config.role==NET_HOST && roster_fixed && find_member(r->mac)<0)return;
  uint32_t target,requester;memcpy(&target,p->data,4);memcpy(&requester,p->data+4,4);
  if(p->type==19){
   if(target!=boot_id || requester!=p->boot)return;
   packet_t response=make_packet(20);response.length=24;memcpy(response.data,p->data,24);send_packet(r->mac,&response);
  }else if(requester==boot_id && target==p->boot){
   for(unsigned i=0;i<8;i++)if(identities[i].used && !memcmp(identities[i].mac,r->mac,6)){
    network_identity_accept(&identities[i],p->boot,p->seq,p->data+8,r->received_us);break;
   }
  }
  return;
 }
 if(config.role==NET_HOST){
  if(p->type==1 && p->length==24){
   int index=find_member(r->mac);if(index<0 && (roster_fixed || count>=8))return;
   if(!identity_admitted(r,index<0?0:members[index].boot))return;
  }
  if(p->type==5 && p->length==8){
   packet_t reply=make_packet(6);reply.length=24;memcpy(reply.data,p->data,8);memcpy(reply.data+8,&r->received_us,8);
   int64_t sent=esp_timer_get_time();memcpy(reply.data+16,&sent,8);send_packet(r->mac,&reply);return;
  }
  if(p->type==18 && p->length==32){
   unsigned slot=p->data[1];
   if(p->data[0]!=25 || slot>=4 || p->data[2]>1 || p->data[3]>1 || p->data[24]>1)return;
   taskENTER_CRITICAL(&lock);int i=find_member(r->mac);
   if(i>=0 && members[i].boot==p->boot && members[i].snapshot_seen && r->received_us-members[i].snapshot_seen<5000000 && network_sequence_after(p->seq,member_slot_seq[i][slot])){
    memcpy(member_slot_config[i][slot],p->data,32);member_slot_seq[i][slot]=p->seq;member_slot_seen[i][slot]=r->received_us;
   }taskEXIT_CRITICAL(&lock);return;
  }
  if(p->type==17 && p->length==24){
   uint32_t target;memcpy(&target,p->data,4);
   taskENTER_CRITICAL(&lock);int i=find_member(r->mac);
   if(i>=0 && target==boot_id && members[i].boot==p->boot && members[i].snapshot_seen && r->received_us-members[i].snapshot_seen<5000000 && network_sequence_after(p->seq,event_clock_sequence[i])){
    event_host_clock_t *c=&event_clocks[i];c->member_boot=p->boot;c->host_boot=target;memcpy(&c->offset_us,p->data+4,8);memcpy(&c->sampled_us,p->data+12,8);memcpy(&c->uncertainty_us,p->data+20,4);c->valid=true;event_clock_sequence[i]=p->seq;
   }taskEXIT_CRITICAL(&lock);return;
  }
  /* Diagnostic types never enter RF journals or business state. */
  if(p->type==15 && p->length==32){
   taskENTER_CRITICAL(&lock);int i=find_member(r->mac);int accepted=-1;
   if(i<0)++bench_unknown;
   else if(members[i].boot!=p->boot)++bench_boot_mismatch;
   else if(!members[i].snapshot_seen || esp_timer_get_time()-members[i].snapshot_seen>=5000000)++bench_stale;
   else accepted=event_transport_receive(&bench_receivers[i],p->boot,p->seq,p->data);
   bool drop=accepted>=0 && bench_drop_until && esp_timer_get_time()<bench_drop_until;
   if(drop){bench_drop_until=0;++bench_dropped;}
   taskEXIT_CRITICAL(&lock);
   if(accepted>=0 && !drop){packet_t ack=make_packet(16);ack.length=8;memcpy(ack.data,&p->boot,4);memcpy(ack.data+4,&p->seq,4);send_packet(r->mac,&ack);}return;
  }
  if(p->type==12 && p->length==32){
   taskENTER_CRITICAL(&lock);int index=find_member(r->mac);
   /* Discovery is heartbeat driven; event packets never admit unknown peers
    * or establish member liveness. Bind to the current heartbeat boot. */
   int accepted=-1;
   if(index>=0 && members[index].boot==p->boot && members[index].snapshot_seen && esp_timer_get_time()-members[index].snapshot_seen<5000000)
    {accepted=event_transport_receive(&event_receivers[index],p->boot,p->seq,p->data);
     if(accepted==1){network_events_push(&network_events,r->mac,p->boot,p->seq,p->data,r->received_us);
      network_events.ring[(network_events.next+31)%32].time=event_host_map(p->data,&event_clocks[index],p->boot,boot_id,r->received_us,false);}}
   taskEXIT_CRITICAL(&lock);
   if(accepted>=0){packet_t ack=make_packet(13);ack.length=8;memcpy(ack.data,&p->boot,4);memcpy(ack.data+4,&p->seq,4);send_packet(r->mac,&ack);}
   return;
  }
  if((p->type!=1 || p->length!=24) && (p->type!=3 || p->length!=8) && (p->type!=7 || p->length!=21) && (p->type!=8 || p->length!=8) && (p->type!=9 || p->length!=12) && (p->type!=10 || p->length!=32 || p->data[0]!=13) && (p->type!=11 || p->length!=28) && (p->type!=14 || p->length!=12))return;
  taskENTER_CRITICAL(&lock);int i=find_member(r->mac);
  /* Replies and telemetry never discover peers or roll boot identity back.
   * ACKs can complete an in-flight command even after heartbeat expiry. */
  if(p->type!=1 && (i<0 || members[i].boot!=p->boot || !members[i].snapshot_seen ||
     (p->type!=3 && !network_telemetry_current(p->boot,members[i].boot,members[i].snapshot_seen,r->received_us)))){
   taskEXIT_CRITICAL(&lock);return;
  }
  if(i<0 && p->type==1 && !roster_fixed && count<8){i=count++;memcpy(members[i].mac,r->mac,6);}
  if(i>=0){if(members[i].boot!=p->boot){memset(member_slot_seq[i],0,sizeof(member_slot_seq[i]));memset(member_slot_seen[i],0,sizeof(member_slot_seen[i]));memset(&event_clocks[i],0,sizeof(event_clocks[i]));event_clock_sequence[i]=0;memset(pilot_sequence[i],0,sizeof(pilot_sequence[i]));terminal_sequence[i]=0;heartbeat_sequence[i]=0;config_sequence[i]=0;members[i].outcome_seq=0;members[i].outcome_seen=0;members[i].acquisition_seq=0;members[i].acquisition_seen=0;members[i].feedback_seq=0;members[i].feedback_seen=0;members[i].config_seen=0;members[i].timing_seen=0;members[i].timing_seq=0;members[i].snapshot_seen=0;members[i].pilot_session=0;memset(members[i].pilot_seen,0,sizeof(members[i].pilot_seen));}
   /* Repeated telemetry must not keep a disconnected member alive. ACK IDs
    * belong to host commands, so they cannot share the member TX sequence. */
   bool fresh=p->type==3;
   if(p->type==1)fresh=network_sequence_after(p->seq,heartbeat_sequence[i]);
   if(p->type==8)fresh=network_sequence_after(p->seq,config_sequence[i]);
   if(p->type==14)fresh=network_sequence_after(p->seq,members[i].acquisition_seq);
   if(p->type==10)fresh=network_sequence_after(p->seq,members[i].feedback_seq);
   if(p->type==9)fresh=network_sequence_after(p->seq,members[i].timing_seq);
   network_score_t pilot_score={0};
   if(p->type==7)fresh=network_score_decode(p->data,p->length,&pilot_score) && network_score_current(&pilot_score,members[i].session,p->seq,pilot_sequence[i][pilot_score.pilot]);
   if(p->type==11){uint32_t session;memcpy(&session,p->data,4);fresh=network_report_fresh(p->seq,members[i].outcome_seq,session==members[i].session);}
   if(!fresh){taskEXIT_CRITICAL(&lock);return;}
   if(p->type!=3)members[i].seen=esp_timer_get_time();
   members[i].boot=p->boot;
   if(p->type==3)members[i].reply=*p;
   if(p->type==11){members[i].outcome_seq=p->seq;members[i].outcome_seen=esp_timer_get_time();memcpy(&members[i].outcome_session,p->data,4);memcpy(members[i].outcome,p->data+4,24);}
   if(p->type==14){members[i].acquisition_seq=p->seq;members[i].acquisition_seen=esp_timer_get_time();memcpy(members[i].acquisition,p->data,12);}
   if(p->type==10){members[i].feedback_seq=p->seq;members[i].feedback_seen=esp_timer_get_time();memcpy(members[i].feedback,p->data,32);}
   if(p->type==9 && network_sequence_after(p->seq,members[i].timing_seq)){member_t *m=&members[i];m->timing_seq=p->seq;m->timing_seen=esp_timer_get_time();m->timing_valid=p->data[0];m->timing_phase=p->data[1];memcpy(&m->timing_uncertainty,p->data+4,4);memcpy(&m->timing_session,p->data+8,4);}
   if(p->type==8 && network_sequence_after(p->seq,config_sequence[i])){config_sequence[i]=p->seq;memcpy(&members[i].rf_config,p->data,8);members[i].config_seen=esp_timer_get_time();}
   if(p->type==7){
    unsigned pilot=pilot_score.pilot;uint32_t session=pilot_score.session;
    if(members[i].pilot_session!=session){members[i].pilot_session=session;memset(members[i].pilot_seen,0,sizeof(members[i].pilot_seen));}
    pilot_sequence[i][pilot]=p->seq;members[i].pilot_ms[pilot]=pilot_score.owned_ms;
    members[i].pilot_captures[pilot]=pilot_score.captures;members[i].pilot_laps[pilot]=pilot_score.laps;
    members[i].pilot_fastest_ms[pilot]=pilot_score.fastest_ms;members[i].pilot_seen[pilot]=esp_timer_get_time();
   }
   if(p->type==1 && p->length==24 && network_sequence_after(p->seq,heartbeat_sequence[i])){uint32_t incoming_session;memcpy(&incoming_session,p->data+12,4);
    if(p->data[0]==GAME_FINISHED){if(members[i].state!=GAME_FINISHED || members[i].session!=incoming_session)terminal_sequence[i]=p->seq;}else terminal_sequence[i]=0;
    heartbeat_sequence[i]=p->seq;members[i].snapshot_seen=esp_timer_get_time();members[i].state=p->data[0];members[i].mode=p->data[1];members[i].owner=p->data[2];memcpy(&members[i].red_ms,p->data+4,4);memcpy(&members[i].blue_ms,p->data+8,4);members[i].fault=p->data[3];memcpy(&members[i].session,p->data+12,4);members[i].online=p->data[16];members[i].rf=p->data[17];members[i].calibrated=p->data[18];}}
  taskEXIT_CRITICAL(&lock);return;
 }
 if(config.role!=NET_MEMBER || memcmp(r->mac,config.host,6))return;
 if(p->type==16 && p->length==8){
  uint32_t sender,seq;memcpy(&sender,p->data,4);memcpy(&seq,p->data+4,4);
  if(p->boot!=last_host_boot || !event_sender_ack(&bench_sender,p->boot,boot_id,sender,seq))++bench_ack_invalid;
  return;
 }
 if(p->type==13 && p->length==8){
  uint32_t sender,seq;memcpy(&sender,p->data,4);memcpy(&seq,p->data+4,4);
  if(p->boot==last_host_boot)event_mux_ack(&member_events,&event_sender,p->boot,boot_id,sender,seq);
  return;
 }
 if(p->type==6 && p->length==24){
  int64_t t1,t2,t3;memcpy(&t1,p->data,8);memcpy(&t2,p->data+8,8);memcpy(&t3,p->data+16,8);
  int64_t delay=(r->received_us-t1)-(t3-t2);
  if(p->boot==last_host_boot && clock_request>0 && t1==clock_request && t3>=t2 && delay>=0 && delay<200000){
   ++clock_replies;clock_last_rtt=delay;if(clock_last_rtt>clock_max_rtt)clock_max_rtt=clock_last_rtt;
   uint32_t candidate=(delay+1)/2;
   packet_t report=make_packet(17);report.length=24;
   taskENTER_CRITICAL(&lock);
   if(network_clock_select(clock_updated,r->received_us,clock_uncertainty,candidate)){
    ++clock_selected;
    clock_offset=((t2-t1)+(t3-r->received_us))/2;clock_uncertainty=candidate;clock_updated=r->received_us;
   }
   clock_request=0;
   memcpy(report.data,&last_host_boot,4);memcpy(report.data+4,&clock_offset,8);memcpy(report.data+12,&clock_updated,8);memcpy(report.data+20,&clock_uncertainty,4);
   taskEXIT_CRITICAL(&lock);
   send_packet(config.host,&report);
  }else ++clock_rejected;
  return;
 }
 if(p->type==4 && (p->length==0 || p->length==8)){
  if(!identity_admitted(r,last_host_boot))return;
  if(last_host_boot==p->boot && !network_sequence_after(p->seq,last_host_heartbeat_seq))return;
  if(last_host_boot && last_host_boot!=p->boot){taskENTER_CRITICAL(&lock);memset(&scheduled,0,sizeof(scheduled));taskEXIT_CRITICAL(&lock);member_stop_request(&member_stop);}
  if(last_host_boot!=p->boot){controller_config_transfer_reset();clock_updated=0;clock_request=0;taskENTER_CRITICAL(&lock);cancelled_schedule=cancelled_fence=prepared_sequence=0;memset(&scheduled,0,sizeof(scheduled));taskEXIT_CRITICAL(&lock);}
  last_host_boot=p->boot;event_sender_bind(&event_sender,last_host_boot);last_host_heartbeat_seq=p->seq;last_host_seen=esp_timer_get_time();last_host_heartbeat_seen=r->received_us;
  if(p->length==8){
   uint32_t cancelled,fence;memcpy(&cancelled,p->data,4);memcpy(&fence,p->data+4,4);
   taskENTER_CRITICAL(&lock);cancelled_schedule=cancelled;cancelled_fence=fence;
   if(scheduled.phase && schedule_abort_matches(cancelled,fence,scheduled.session,prepared_sequence)){start_schedule_cancel(&scheduled,cancelled);scheduled_error=ESP_ERR_INVALID_STATE;}
   taskEXIT_CRITICAL(&lock);
  }
  return;
 }
 if(p->type!=2)return;
 if(!network_command_target(p->data,p->length,boot_id)){++stale_commands;return;}
 p->length-=4;
 if(!command_member_frame(p->data,p->length))return;
 if(!network_command_current(p->boot,last_host_boot,last_host_heartbeat_seen,r->received_us)){
  ++stale_commands;return;
 }
 member_t *host=&members[0];
 /* Host reboot invalidates cached command IDs; lower/old IDs never execute. */
 if(host->boot!=p->boot){host->boot=p->boot;host->last_seq=0;}
 if(p->seq!=host->last_seq && !network_sequence_after(p->seq,host->last_seq)){++stale_commands;return;}
 if(p->seq==host->last_seq){
  if(p->length==host->request.length && !memcmp(p->data,host->request.data,p->length)){++duplicate_replies;send_packet(config.host,&host->reply);}
  return;
 }
 last_host_seen=esp_timer_get_time();
 ++command_executed;esp_err_t error;
 uint32_t command_session=0;if(p->length>=6)memcpy(&command_session,p->data+2,4);
 if(reboot_pending || member_stop_pending(&member_stop) || ((p->data[1]==28 || p->data[1]==29) && schedule_abort_matches(cancelled_schedule,cancelled_fence,command_session,p->seq))){error=ESP_ERR_INVALID_STATE;}
 else if(p->data[1]>=28 && p->data[1]<=30){
  uint32_t session=0;memcpy(&session,p->data+2,4);error=ESP_ERR_INVALID_ARG;
  if(p->data[1]==28 && p->length==15 && (p->data[14]==1 || p->data[14]==2)){
   bool resume=p->data[14]==2;uint8_t state;uint32_t current_session;
   int64_t at;memcpy(&at,p->data+6,8);int64_t now=esp_timer_get_time();
   taskENTER_CRITICAL(&lock);bool valid=network_clock_error(clock_updated,now,clock_uncertainty)<=20000;int64_t offset=clock_offset;taskEXIT_CRITICAL(&lock);
   if(resume)valid=valid && controller_game_phase(&state,&current_session) && state==GAME_PAUSED && current_session==session;
   error=valid?controller_game_ready(resume):ESP_ERR_INVALID_STATE;
   if(error==ESP_OK){taskENTER_CRITICAL(&lock);error=start_schedule_prepare_action(&scheduled,session,at-offset,now,p->data[14])?ESP_OK:ESP_ERR_INVALID_STATE;if(error==ESP_OK)prepared_sequence=p->seq;taskEXIT_CRITICAL(&lock);}
  }else if((p->data[1]==29 || p->data[1]==30) && p->length==6){taskENTER_CRITICAL(&lock);bool ok=p->data[1]==29?start_schedule_arm(&scheduled,session,esp_timer_get_time()):start_schedule_cancel(&scheduled,session);taskEXIT_CRITICAL(&lock);error=ok?ESP_OK:ESP_ERR_INVALID_STATE;}
 }else {
  taskENTER_CRITICAL(&lock);bool pending=scheduled.phase!=0;
  if(pending && command_is_halt(p->data,p->length)){start_schedule_cancel(&scheduled,scheduled.session);pending=false;}
  taskEXIT_CRITICAL(&lock);
  bool satisfied=false;
  if(command_is_halt(p->data,p->length)){
   uint8_t state;uint32_t session;
   if(controller_game_phase(&state,&session))satisfied=
    (state!=GAME_RUNNING && state!=GAME_PAUSED) ||
    (state==GAME_PAUSED && p->data[1]==10 && p->data[2]==3);
  }
  error=pending?ESP_ERR_INVALID_STATE:satisfied?ESP_OK:controller_execute_command(p->data,p->length);
 }
 ESP_LOGI("network","APPLY seq=%"PRIu32" opcode=%u result=%s",p->seq,p->data[1],esp_err_to_name(error));
 host->last_seq=p->seq;host->request=*p;host->reply=make_packet(3);host->reply.seq=p->seq;host->reply.length=8;
 memcpy(host->reply.data,&p->boot,4);memcpy(host->reply.data+4,&error,4);
 taskENTER_CRITICAL(&lock);bool drop=drop_ack_until && esp_timer_get_time()<drop_ack_until;drop_ack_until=0;taskEXIT_CRITICAL(&lock);
 if(drop){++ack_dropped;}else send_packet(config.host,&host->reply);
 host->seen=esp_timer_get_time();
}
static esp_err_t dispatch_target_locked(const uint8_t *command,uint8_t length,const uint8_t *target){
 if(config.role!=NET_HOST)return config.role==NET_STANDALONE?ESP_OK:ESP_ERR_INVALID_STATE;
 if(!command || length<2 || length>16 || command[0]!=1)return ESP_ERR_INVALID_ARG;
 packet_t p=make_packet(2);p.length=length;memcpy(p.data,command,length);
 unsigned peers;taskENTER_CRITICAL(&lock);peers=count;taskEXIT_CRITICAL(&lock);
 /* An explicitly empty host roster is a valid local-only installation. */
 if(!peers)return target?ESP_ERR_NOT_FOUND:ESP_OK;
 bool failed=false,found=false;
 for(unsigned i=0;i<peers;i++){
  taskENTER_CRITICAL(&lock);member_t member=members[i];taskEXIT_CRITICAL(&lock);
  if(target && memcmp(target,member.mac,6))continue;
  found=true;
  /* Bind each copy, including retries, to the member boot sampled here. */
  p.length=length+4;memcpy(p.data+length,&member.boot,4);
  if(!member.seen || esp_timer_get_time()-member.seen>5000000){taskENTER_CRITICAL(&lock);members[i].error=ESP_ERR_TIMEOUT;taskEXIT_CRITICAL(&lock);ESP_LOGW("network","RESULT seq=%"PRIu32" opcode=%u member=%u offline=1",p.seq,command[1],i);failed=true;continue;}
  bool ack=false;
  for(unsigned retry=0;retry<3 && !ack;retry++){
   if(esp_timer_get_time()>=operation_deadline)return ESP_ERR_TIMEOUT;
   ESP_LOGI("network","DISPATCH seq=%"PRIu32" opcode=%u member=%u attempt=%u",p.seq,command[1],i,retry+1);
   send_packet(member.mac,&p);int64_t deadline=esp_timer_get_time()+3000000;if(deadline>operation_deadline)deadline=operation_deadline;
   while(esp_timer_get_time()<deadline){
    taskENTER_CRITICAL(&lock);packet_t reply=members[i].reply;uint32_t current_boot=members[i].boot;taskEXIT_CRITICAL(&lock);
    if(current_boot!=member.boot){taskENTER_CRITICAL(&lock);members[i].error=ESP_ERR_INVALID_STATE;taskEXIT_CRITICAL(&lock);ESP_LOGW("network","RESULT seq=%"PRIu32" opcode=%u member=%u reboot=1",p.seq,command[1],i);ack=true;failed=true;break;}
    uint32_t sender=0;memcpy(&sender,reply.data,4);
    if(reply.type==3 && reply.length==8 && network_reply_current(member.boot,current_boot,reply.boot,boot_id,sender,p.seq,reply.seq)){int32_t error;memcpy(&error,reply.data+4,4);taskENTER_CRITICAL(&lock);members[i].error=error;taskEXIT_CRITICAL(&lock);ESP_LOGI("network","RESULT seq=%"PRIu32" opcode=%u member=%u error=%s",p.seq,command[1],i,esp_err_to_name(error));ack=true;failed|=error!=ESP_OK;break;}
    vTaskDelay(pdMS_TO_TICKS(30));
   }
  }
  if(!ack){taskENTER_CRITICAL(&lock);members[i].error=ESP_ERR_TIMEOUT;taskEXIT_CRITICAL(&lock);ESP_LOGW("network","RESULT seq=%"PRIu32" opcode=%u member=%u error=ACK_TIMEOUT",p.seq,command[1],i);failed=true;}
 }
 return !found?ESP_ERR_NOT_FOUND:failed?ESP_FAIL:ESP_OK;
}
static esp_err_t dispatch_locked(const uint8_t *command,uint8_t length){return dispatch_target_locked(command,length,NULL);}

static bool capture_scores(score_snapshot_t *snapshot,bool finalized){
 memset(snapshot,0,sizeof(*snapshot));
 controller_game_summary_t g;if(!controller_game_summary(&g))return false;
 snapshot->session=g.session;snapshot->mode=g.mode;snapshot->state=g.state;
 match_node_t *local=&snapshot->nodes[0];local->session=g.session;local->mode=g.mode;local->fault=g.fault;
 for(unsigned j=0;j<2;j++)local->team_ms[j]=g.team_us[j]/1000;
 for(unsigned j=0;j<8;j++){local->pilot_captures[j]=g.pilot_captures[j];local->pilot_ms[j]=g.pilot_us[j]/1000;local->pilot_laps[j]=g.pilot_laps[j];local->pilot_fastest_ms[j]=g.pilot_fastest_ms[j];}
 for(unsigned j=0;j<8;j++){
  const game_pilot_t *p=&g.match_pilots[j];uint8_t *id=snapshot->pilot_identity[j];
  id[0]=p->enabled;id[1]=p->channel;id[2]=p->team;id[3]=p->slot;memcpy(id+4,&p->color,3);memcpy(id+7,p->name,17);
 }
 memcpy(snapshot->node_mac[0],local_mac,6);
 if(g.state==GAME_FINISHED){snapshot->outcome_mask=1;memcpy(snapshot->node_outcome[0],g.outcome,24);}
 snapshot->pilot_valid[0]=255;
 taskENTER_CRITICAL(&lock);
 snapshot->roster=roster_session && roster_session==snapshot->session?match_roster:(1u<<(count+1))-1;
 int64_t now=esp_timer_get_time();
 bool frozen=roster_session && roster_session==snapshot->session;
 for(unsigned slot=0;slot<8;slot++){
  if(!(snapshot->roster&(1u<<(slot+1))))continue;
  const uint8_t *identity=frozen?roster_mac[slot]:members[slot].mac;
  memcpy(snapshot->node_mac[slot+1],identity,6);
  match_node_t *n=&snapshot->nodes[slot+1];n->age_ms=UINT32_MAX;
  int found=find_member(identity);if(found<0)continue;unsigned i=found;
  member_t *m=&members[i];
  if(!match_member_identity(frozen,roster_mac[slot],roster_boot[slot],m->mac,m->boot))continue;
  n->session=m->session;n->mode=m->mode;n->fault=m->fault;
  if(m->state==GAME_FINISHED && m->outcome_seen && now-m->outcome_seen<5000000 && m->outcome_session==snapshot->session && m->outcome[0]==GAME_FINISHED && m->outcome[1]==snapshot->mode && result_terminal_score(terminal_sequence[i],m->outcome_seq)){
   snapshot->outcome_mask|=1u<<(slot+1);memcpy(snapshot->node_outcome[slot+1],m->outcome,24);
  }
  n->age_ms=m->snapshot_seen?(now-m->snapshot_seen)/1000:UINT32_MAX;
  if(finalized && (m->state!=GAME_FINISHED || !terminal_sequence[i]))n->age_ms=UINT32_MAX;
  n->team_ms[0]=m->red_ms;n->team_ms[1]=m->blue_ms;
  for(unsigned j=0;j<8;j++)if(m->pilot_session==m->session && m->pilot_seen[j] && now-m->pilot_seen[j]<5000000 && (!finalized || result_terminal_score(terminal_sequence[i],pilot_sequence[i][j]))){
   n->pilot_captures[j]=m->pilot_captures[j];n->pilot_ms[j]=m->pilot_ms[j];n->pilot_laps[j]=m->pilot_laps[j];n->pilot_fastest_ms[j]=m->pilot_fastest_ms[j];snapshot->pilot_valid[slot+1]|=1u<<j;
  }
 }
 taskEXIT_CRITICAL(&lock);return true;
}
static void summary_packet(uint8_t out[32],unsigned page,bool finalized){
 score_snapshot_t snapshot;if(!capture_scores(&snapshot,finalized)){memset(out,0,32);return;}
 encode_scores(&snapshot,out,page-17);
}

esp_err_t network_capture_result_pages(void){
 score_snapshot_t snapshot;if(!capture_scores(&snapshot,false))return ESP_ERR_TIMEOUT;
 uint8_t pages[RESULT_PAGES][32];
 for(unsigned i=0;i<RESULT_PAGES;i++)encode_scores(&snapshot,pages[i],i);
 taskENTER_CRITICAL(&lock);
 memcpy(read_result_pages,pages,sizeof(pages));
 if(!++read_result_revision)++read_result_revision;
 read_result_captured=esp_timer_get_time();
 taskEXIT_CRITICAL(&lock);return ESP_OK;
}

static esp_err_t save_result_snapshot(const score_snapshot_t *snapshot){
 if(config.role==NET_MEMBER)return ESP_ERR_INVALID_STATE;
 if(snapshot->state!=GAME_FINISHED || !snapshot->session)return ESP_ERR_INVALID_STATE;
 result_record_t candidate={.network=config.network};
 taskENTER_CRITICAL(&lock);
 bool frozen=roster_session && roster_session==snapshot->session;
 uint32_t revision=saved_result.revision+1;
 bool restored=false;
 if(!frozen)for(unsigned i=0;i<history.count;i++){
  uint32_t session;memcpy(&session,history.records[i].pages[0]+4,4);
  if(history.records[i].network==config.network && session==snapshot->session){candidate=history.records[i];restored=true;break;}
 }
 taskEXIT_CRITICAL(&lock);
 /* After a host reboot the old members' boot identities are unavailable.
  * Recommit the sealed record; never reconstruct it from today's roster.
  * A never-saved old session cannot be reconstructed safely. */
 if(!frozen && !restored)return ESP_ERR_INVALID_STATE;
 candidate.revision=revision?revision:1;
 if(frozen){
  for(unsigned i=0;i<RESULT_PAGES;i++)encode_scores(snapshot,candidate.pages[i],i);
  memcpy(candidate.pilot_identity,snapshot->pilot_identity,sizeof(candidate.pilot_identity));
  memcpy(candidate.node_mac,snapshot->node_mac,sizeof(candidate.node_mac));
  candidate.outcome_mask=snapshot->outcome_mask&snapshot->roster;memcpy(candidate.node_outcome,snapshot->node_outcome,sizeof(candidate.node_outcome));
 }
 result_record_seal(&candidate);if(!result_record_valid(&candidate))return ESP_ERR_INVALID_RESPONSE;
 result_history_t *next=malloc(sizeof(*next));if(!next)return ESP_ERR_NO_MEM;
 taskENTER_CRITICAL(&lock);*next=history;taskEXIT_CRITICAL(&lock);
 if(!result_history_push(next,&candidate)){free(next);return ESP_ERR_INVALID_RESPONSE;}
 nvs_handle_t h;esp_err_t error=storage_fault_take(STORAGE_FAULT_NETWORK_RESULT)?ESP_ERR_NVS_NOT_ENOUGH_SPACE:nvs_open("net_results",NVS_READWRITE,&h);
 if(error==ESP_OK){error=nvs_set_blob(h,"history",next,sizeof(*next));if(error==ESP_OK)error=nvs_commit(h);nvs_close(h);}
 taskENTER_CRITICAL(&lock);result_save_error=error;if(error==ESP_OK){history=*next;saved_result=candidate;}taskEXIT_CRITICAL(&lock);
 free(next);
 return error;
}
static esp_err_t save_result_locked(void){
 score_snapshot_t snapshot;esp_err_t e;
 if(!capture_scores(&snapshot,true))e=ESP_ERR_TIMEOUT;
 else e=save_result_snapshot(&snapshot);
 taskENTER_CRITICAL(&lock);result_save_error=e;taskEXIT_CRITICAL(&lock);return e;
}
void network_status(uint8_t out[32],unsigned page){
 if(page==243){taskENTER_CRITICAL(&lock);memcpy(out,saved_network_page,32);taskEXIT_CRITICAL(&lock);return;}
 if(page>=238 && page<=242){unsigned index=page-238;taskENTER_CRITICAL(&lock);
  const result_record_t *r=index==0?&saved_result:index-1<history.count?&history.records[index-1]:NULL;
  match_verdict_page(r,index,out);taskEXIT_CRITICAL(&lock);return;}

 if(page>=353 && page<=416){
  memset(out,0,32);unsigned member=(page-353)/8,slot=((page-353)%8)/2;
  taskENTER_CRITICAL(&lock);
  if((page-353)%2){if(member<count && member_slot_seen[member][slot])memcpy(out,member_slot_config[member][slot],32);}
  else {out[0]=26;out[1]=member;out[2]=slot;if(member<count){memcpy(out+4,members[member].mac,6);memcpy(out+12,&members[member].boot,4);memcpy(out+16,&member_slot_seq[member][slot],4);
   uint64_t now=esp_timer_get_time(),seen=member_slot_seen[member][slot];uint32_t age=seen && now>=seen?(now-seen)/1000:UINT32_MAX;out[3]=seen && age<5000;memcpy(out+20,&age,4);}}
  taskEXIT_CRITICAL(&lock);return;
 }
 if(page>=256 && page<=352){taskENTER_CRITICAL(&lock);network_events_page(&network_events_snapshot,page,esp_timer_get_time(),out);taskEXIT_CRITICAL(&lock);return;}
 if(page>=214 && page<=229){
  unsigned index=(page-214)/2;memset(out,0,32);taskENTER_CRITICAL(&lock);
  if(index<count){event_receiver_t *e=&event_receivers[index];
   if((page-214)%2){memcpy(out,e->payload,32);}
   else {out[0]=18;out[1]=index;memcpy(out+2,members[index].mac,6);memcpy(out+8,&e->boot,4);memcpy(out+12,&e->sequence,4);memcpy(out+16,&e->accepted,4);memcpy(out+20,&e->duplicates,4);memcpy(out+24,&e->rejected,4);uint32_t lost=e->lost>UINT32_MAX?UINT32_MAX:e->lost;memcpy(out+28,&lost,4);}
  }
  taskEXIT_CRITICAL(&lock);return;
 }

 if(page>=169 && page<=213){
  unsigned index=(page-169)/9,node=(page-169)%9;memset(out,0,32);
  taskENTER_CRITICAL(&lock);const result_record_t *r=index==0?&saved_result:index-1<history.count?&history.records[index-1]:NULL;
  if(r)result_record_outcome_page(r,node,out);
  taskEXIT_CRITICAL(&lock);return;
 }

 if(page>=114 && page<=168){
  unsigned index=(page-114)/11,part=(page-114)%11;memset(out,0,32);
  taskENTER_CRITICAL(&lock);
  const result_record_t *r=index==0?&saved_result:index-1<history.count?&history.records[index-1]:NULL;
  if(r)result_record_identity_page(r,part,out);
  taskEXIT_CRITICAL(&lock);return;
 }

 if(page>=230 && page<=237){
  memset(out,0,32);out[0]=19;out[1]=page-230;taskENTER_CRITICAL(&lock);
  if(page-230<count){member_t *m=&members[page-230];memcpy(out+2,m->mac,6);
   uint32_t age=m->acquisition_seen?(esp_timer_get_time()-m->acquisition_seen)/1000:UINT32_MAX;
   out[8]=age<5000;out[9]=m->acquisition[0];out[10]=m->acquisition[1];
   memcpy(out+12,m->acquisition+2,8);memcpy(out+20,&age,4);memcpy(out+24,&m->boot,4);}
  taskEXIT_CRITICAL(&lock);return;
 }

 if(page>=106 && page<=113){
  memset(out,0,32);out[0]=14;out[1]=page-106;taskENTER_CRITICAL(&lock);
  if(page-106<count){member_t *m=&members[page-106];memcpy(out+2,m->mac,6);out[8]=m->feedback_seen && esp_timer_get_time()-m->feedback_seen<5000000;memcpy(out+9,m->feedback+1,19);}
  taskEXIT_CRITICAL(&lock);return;
 }

 if(page==105){io_service_status(out);return;}
 if(page==244){io_service_finder_status(out);return;}
 if(page>=95 && page<=104){
  memset(out,0,32);taskENTER_CRITICAL(&lock);
  if(page==95){out[0]=12;out[1]=read_result_revision!=0;memcpy(out+4,&read_result_revision,4);uint32_t age=read_result_revision?(esp_timer_get_time()-read_result_captured)/1000:UINT32_MAX;memcpy(out+8,&age,4);memcpy(out+12,read_result_pages[0]+4,4);}
  else if(read_result_revision)memcpy(out,read_result_pages[page-96],32);
  taskEXIT_CRITICAL(&lock);return;
 }

 if(page==94){memset(out,0,32);out[0]=11;taskENTER_CRITICAL(&lock);out[1]=roster_fixed;out[2]=count;memcpy(out+4,&roster_error,4);taskEXIT_CRITICAL(&lock);return;}
 if(page>=54 && page<=93){
  unsigned index=(page-54)/10,part=(page-54)%10;memset(out,0,32);taskENTER_CRITICAL(&lock);
  if(!part){out[0]=10;out[1]=index<history.count;out[2]=index;out[3]=history.count;memcpy(out+12,&result_save_error,4);
   if(out[1]){result_record_t *r=&history.records[index];memcpy(out+4,&r->revision,4);memcpy(out+8,&r->network,4);memcpy(out+16,r->pages[0]+4,4);out[20]=r->pages[0][2];out[21]=3;memcpy(out+24,&r->outcome_mask,2);uint16_t missing=result_record_missing(r);memcpy(out+22,&missing,2);}
  }else if(index<history.count)memcpy(out,history.records[index].pages[part-1],32);
  taskEXIT_CRITICAL(&lock);return;
 }
 if(page>=46 && page<=53){memset(out,0,32);out[0]=9;out[1]=page-46;taskENTER_CRITICAL(&lock);
  if(page-46<count){member_t *m=&members[page-46];memcpy(out+2,m->mac,6);out[8]=m->timing_seen && esp_timer_get_time()-m->timing_seen<3000000;out[9]=m->timing_valid;out[10]=m->timing_phase;memcpy(out+12,&m->timing_uncertainty,4);memcpy(out+16,&m->timing_session,4);}
  taskEXIT_CRITICAL(&lock);return;
 }
 if(page==45){memset(out,0,32);out[0]=8;taskENTER_CRITICAL(&lock);out[1]=scheduled.phase;out[2]=scheduled.action;memcpy(out+4,&scheduled.session,4);memcpy(out+8,&scheduled.at_us,8);memcpy(out+16,&scheduled_error,4);uint32_t remaining=scheduled.phase && scheduled.at_us>esp_timer_get_time()?(scheduled.at_us-esp_timer_get_time())/1000:0;memcpy(out+20,&remaining,4);taskEXIT_CRITICAL(&lock);return;}
 if(page>=37 && page<=44){
  memset(out,0,32);out[0]=7;out[1]=page-37;taskENTER_CRITICAL(&lock);
  if(page-37<count){member_t *m=&members[page-37];memcpy(out+2,m->mac,6);out[8]=m->config_seen && esp_timer_get_time()-m->config_seen<5000000;memcpy(out+10,&m->rf_config,8);}
  taskEXIT_CRITICAL(&lock);return;
 }
 if(page>=27 && page<=36){
  memset(out,0,32);taskENTER_CRITICAL(&lock);
  if(page==27){out[0]=6;out[1]=saved_result.revision!=0;memcpy(out+4,&saved_result.revision,4);memcpy(out+8,&saved_result.network,4);memcpy(out+12,&result_save_error,4);memcpy(out+16,saved_result.pages[0]+4,4);out[20]=3;memcpy(out+24,&saved_result.outcome_mask,2);}
  else if(saved_result.revision)memcpy(out,saved_result.pages[page-28],32);
  taskEXIT_CRITICAL(&lock);return;
 }

 if(page==26){memset(out,0,32);out[0]=5;taskENTER_CRITICAL(&lock);uint32_t error=network_clock_error(clock_updated,esp_timer_get_time(),clock_uncertainty);out[1]=error<=20000;memcpy(out+4,&clock_offset,8);memcpy(out+12,&error,4);taskEXIT_CRITICAL(&lock);return;}
 if(page>=17 && page<=25){summary_packet(out,page,false);return;}
 memset(out,0,32);out[0]=1;out[1]=config.role;out[2]=config.channel;
 ESP_LOGI("network","rx=%"PRIu32" tx=%"PRIu32" rejected=%"PRIu32" id=%"PRIu32,rx_count,tx_count,auth_fail,config.network);
 uint8_t channel=0;wifi_second_chan_t secondary;esp_wifi_get_channel(&channel,&secondary);
 ESP_LOGI("network","raw=%"PRIu32" send_ok=%"PRIu32" send_fail=%"PRIu32" actual_channel=%u",raw_rx,send_ok,send_fail,channel);
 taskENTER_CRITICAL(&lock);out[3]=count;memcpy(out+4,&config.network,4);memcpy(out+8,local_mac,6);
 if(page< count){member_t *m=&members[page];memcpy(out+14,m->mac,6);out[20]=m->seen && esp_timer_get_time()-m->seen<5000000;out[21]=m->state;out[22]=m->mode;out[23]=m->owner;memcpy(out+24,&m->error,4);}
 if(page>=8 && page<16 && page-8<count){member_t *m=&members[page-8];memset(out,0,32);out[0]=2;out[1]=page-8;memcpy(out+2,m->mac,6);out[8]=m->seen && esp_timer_get_time()-m->seen<5000000;out[9]=m->state;out[10]=m->mode;out[11]=m->fault;out[12]=m->online;out[13]=m->rf;out[14]=m->calibrated;memcpy(out+16,&m->session,4);memcpy(out+20,&m->red_ms,4);memcpy(out+24,&m->blue_ms,4);memcpy(out+28,&m->error,4);}
 if(page==16){memset(out,0,32);out[0]=3;memcpy(out+4,&queue_drops,4);memcpy(out+8,&rx_count,4);memcpy(out+12,&auth_fail,4);}
 taskEXIT_CRITICAL(&lock);
}
static esp_err_t sync_locked(void){
 if(config.role!=NET_HOST)return ESP_ERR_INVALID_STATE;
 uint8_t state;uint32_t session;
 if(!controller_game_phase(&state,&session))return ESP_ERR_TIMEOUT;
 if(state==GAME_RUNNING || state==GAME_PAUSED)return ESP_ERR_INVALID_STATE;
 game_config_t game_config;if(!controller_game_config_get(&game_config))return ESP_ERR_TIMEOUT;
 esp_err_t e=ESP_OK;
 for(unsigned offset=0;offset<sizeof(game_config) && e==ESP_OK;offset+=12){
  unsigned size=sizeof(game_config)-offset;if(size>12)size=12;
  uint8_t cmd[16]={1,20,offset,offset>>8};memcpy(cmd+4,(uint8_t*)&game_config+offset,size);e=dispatch_locked(cmd,size+4);
 }
 if(e==ESP_OK){uint32_t checksum=2166136261u;for(unsigned i=0;i<sizeof(game_config);i++)checksum=(checksum^((uint8_t*)&game_config)[i])*16777619u;uint8_t cmd[6]={1,21};memcpy(cmd+2,&checksum,4);e=dispatch_locked(cmd,6);}
 return e;
}

/* Caller holds control_lock, preventing concurrent scheduled match control. */
static esp_err_t network_configuration_begin(void){
 taskENTER_CRITICAL(&lock);bool pending=scheduled.phase || reboot_pending;taskEXIT_CRITICAL(&lock);
 if(pending)return ESP_ERR_INVALID_STATE;
 return controller_maintenance_begin();
}
static esp_err_t control_begin(void){
 if(!control_lock || xSemaphoreTake(control_lock,0)!=pdTRUE)return ESP_ERR_INVALID_STATE;
 operation_deadline=esp_timer_get_time()+120000000;return ESP_OK;
}
esp_err_t network_save_result(void){
 esp_err_t e=control_begin();if(e!=ESP_OK)return e;
 e=save_result_locked();xSemaphoreGive(control_lock);return e;
}
esp_err_t network_dispatch(const uint8_t *command,uint8_t length){
 esp_err_t e=control_begin();if(e!=ESP_OK)return e;
 e=dispatch_locked(command,length);xSemaphoreGive(control_lock);return e;
}
esp_err_t network_sync(void){
 esp_err_t e=control_begin();if(e!=ESP_OK)return e;
 e=sync_locked();xSemaphoreGive(control_lock);return e;
}
esp_err_t network_bad_config_test(void){
 esp_err_t e=control_begin();if(e!=ESP_OK)return e;
 if(config.role!=NET_HOST){xSemaphoreGive(control_lock);return ESP_ERR_INVALID_STATE;}
 /* These packets must be rejected and must not apply any configuration. */
 uint8_t unordered[5]={1,20,12,0,0};uint8_t incomplete[6]={1,21,0,0,0,0};
 esp_err_t first=dispatch_locked(unordered,sizeof(unordered));
 bool rejected=true;taskENTER_CRITICAL(&lock);for(unsigned i=0;i<count;i++)rejected&=members[i].error==ESP_ERR_INVALID_ARG;taskEXIT_CRITICAL(&lock);
 esp_err_t second=dispatch_locked(incomplete,sizeof(incomplete));
 taskENTER_CRITICAL(&lock);for(unsigned i=0;i<count;i++)rejected&=members[i].error==ESP_ERR_INVALID_ARG;taskEXIT_CRITICAL(&lock);
 xSemaphoreGive(control_lock);
 return rejected && first==ESP_FAIL && second==ESP_FAIL?ESP_OK:ESP_FAIL;
}
static esp_err_t scheduled_start_locked(bool resume){
 bool ready=true;taskENTER_CRITICAL(&lock);
 for(unsigned i=0;i<count;i++)ready&=members[i].timing_seen && esp_timer_get_time()-members[i].timing_seen<3000000 && members[i].timing_valid && members[i].timing_uncertainty<=20000;
 taskEXIT_CRITICAL(&lock);if(!ready)return ESP_ERR_INVALID_STATE;
 uint32_t session=0;uint8_t state;
 if(resume){if(!controller_game_phase(&state,&session) || state!=GAME_PAUSED || !session)return ESP_ERR_INVALID_STATE;}
 else {session=esp_random();if(!session)session=1;}
 taskENTER_CRITICAL(&lock);bool same=!resume || (roster_session==session && match_roster==((1u<<(count+1))-1));
 for(unsigned i=0;resume && i<count;i++)same&=members[i].session==session && members[i].state==GAME_PAUSED && members[i].boot==roster_boot[i];
 taskEXIT_CRITICAL(&lock);if(!same)return ESP_ERR_INVALID_STATE;
 int64_t at=esp_timer_get_time()+25000000,old_deadline=operation_deadline;
 uint8_t prepare[15]={1,28};prepare[14]=resume?2:1;memcpy(prepare+2,&session,4);memcpy(prepare+6,&at,8);
 uint8_t commit[6]={1,29};memcpy(commit+2,&session,4);
 operation_deadline=at-2000000;
 esp_err_t e=dispatch_locked(prepare,sizeof(prepare));
 if(e==ESP_OK)e=dispatch_locked(commit,sizeof(commit));
 if(e==ESP_OK){
  taskENTER_CRITICAL(&lock);
  bool ok=start_schedule_prepare_action(&scheduled,session,at,esp_timer_get_time(),resume?2:1) && start_schedule_arm(&scheduled,session,esp_timer_get_time());
  if(ok){roster_session=session;match_roster=(1u<<(count+1))-1;for(unsigned i=0;i<count;i++){roster_boot[i]=members[i].boot;memcpy(roster_mac[i],members[i].mac,6);}roster_started=at;scheduled_error=ESP_OK;}
  taskEXIT_CRITICAL(&lock);if(!ok)e=ESP_ERR_TIMEOUT;
 }
 operation_deadline=old_deadline;
 if(e!=ESP_OK){taskENTER_CRITICAL(&lock);cancelled_schedule=session;cancelled_fence=sequence;taskEXIT_CRITICAL(&lock);uint8_t cancel[6]={1,30};memcpy(cancel+2,&session,4);dispatch_locked(cancel,6);taskENTER_CRITICAL(&lock);start_schedule_cancel(&scheduled,session);scheduled_error=e;taskEXIT_CRITICAL(&lock);}
 ESP_LOGI("network","SCHEDULE session=%"PRIu32" at_us=%"PRIi64" result=%s",session,at,esp_err_to_name(e));
 return e;
}
/* One entry for BLE and serial; local-only member execution never re-enters it. */
static esp_err_t network_control_scoped(const uint8_t *command,uint8_t length,uint32_t generation);
esp_err_t network_control(const uint8_t *command,uint8_t length){return network_control_scoped(command,length,0);}
esp_err_t network_ble_control(const uint8_t *command,uint8_t length,uint32_t generation){
 if(!generation)return ESP_ERR_INVALID_ARG;
 return network_control_scoped(command,length,generation);
}
static esp_err_t network_control_scoped(const uint8_t *command,uint8_t length,uint32_t generation){
 if(!command || length<2 || length>16 || command[0]!=1)return ESP_ERR_INVALID_ARG;
 esp_err_t e=control_begin();if(e!=ESP_OK)return e;
 /* Recheck after transaction admission: authorization can be revoked between
  * the worker check and this entry. Do not clear staging or mutate devices. */
 if(generation && !ble_control_generation_authorized(generation)){
  xSemaphoreGive(control_lock);return ESP_ERR_INVALID_STATE;
 }
 static uint32_t ble_generation;
 if(ble_transaction_boundary(&ble_generation,generation))controller_ble_transaction_reset();
 if(reboot_pending && command[1]!=18 && command[1]!=11 && command[1]!=13){e=ESP_ERR_INVALID_STATE;goto done;}
 if(command[1]==40){
  uint8_t state;uint32_t session;controller_snapshot_t status;controller_get_snapshot(&status);
  taskENTER_CRITICAL(&lock);bool pending=scheduled.phase!=0;taskEXIT_CRITICAL(&lock);
  if(length!=2){e=ESP_ERR_INVALID_ARG;goto done;}
  if(pending || status.running || !controller_game_phase(&state,&session) || state==GAME_RUNNING || state==GAME_PAUSED){e=ESP_ERR_INVALID_STATE;goto done;}
  e=controller_maintenance_ready();if(e!=ESP_OK)goto done;
  reboot_pending=true;
  if(xTaskCreate(reboot_worker,"device_reboot",2048,NULL,2,NULL)!=pdPASS){reboot_pending=false;e=ESP_ERR_NO_MEM;}else e=ESP_OK;
  goto done;
 }
 bool start=(command[1]==10 && length==3 && command[2]==1) || (command[1]==3 && length==3 && command[2]==1);
 bool resume=command[1]==10 && length==3 && command[2]==2;
 bool halt=command_is_halt(command,length);
 esp_err_t cancellation=ESP_OK;
 bool forward=command_match_forward(command[1]);
 taskENTER_CRITICAL(&lock);uint32_t pending=scheduled.phase?scheduled.session:0;taskEXIT_CRITICAL(&lock);
 if(pending && command[1]!=18 && command[1]!=11 && command[1]!=13 && command[1]!=37){
  if(!halt){e=ESP_ERR_INVALID_STATE;goto done;}
  uint8_t cancel[6]={1,30};memcpy(cancel+2,&pending,4);
  taskENTER_CRITICAL(&lock);cancelled_schedule=pending;cancelled_fence=sequence;start_schedule_cancel(&scheduled,pending);taskEXIT_CRITICAL(&lock);
  if(config.role==NET_HOST)cancellation=dispatch_locked(cancel,6);
  /* A resume countdown still owns a paused match. Cancelling the timer alone
   * must not swallow the requested finish/pause, even if a peer missed cancel. */
 }
 if(command[1]>=28 && command[1]<=30){e=ESP_ERR_INVALID_ARG;goto done;}

 if(command[1]==45){
  if(length!=2){e=ESP_ERR_INVALID_ARG;goto done;}
  if(config.role==NET_MEMBER){e=ESP_ERR_INVALID_STATE;goto done;}
  taskENTER_CRITICAL(&lock);network_events_capture(&network_events_snapshot,&network_events,esp_timer_get_time());taskEXIT_CRITICAL(&lock);e=ESP_OK;goto done;
 }
 if(command[1]==51){
  if(length!=10 || command[8]>=4 || command[9]>3){e=ESP_ERR_INVALID_ARG;goto done;}
  if(config.role!=NET_HOST){e=ESP_ERR_INVALID_STATE;goto done;}
  uint8_t local[4]={1,50,command[8],command[9]};e=dispatch_target_locked(local,sizeof(local),command+2);goto done;
 }
 if(command[1]==48){
  if(length!=16){e=ESP_ERR_INVALID_ARG;goto done;}
  if(config.role!=NET_HOST){e=ESP_ERR_INVALID_STATE;goto done;}
  node_config_t desired={command[8],command[9]&63,command[10]|((uint16_t)command[11]<<8),command[12]|((uint16_t)command[13]<<8),command[14]|((uint16_t)command[15]<<8)};
  if(!receiver_config_valid(&desired)){e=ESP_ERR_INVALID_ARG;goto done;}
  uint8_t local[11]={1,47,command[9]>>6};memcpy(local+3,&desired,8);
  e=dispatch_target_locked(local,sizeof(local),command+2);goto done;
 }
 if(command[1]==44){
  if(length!=9 || command[8]>1){e=ESP_ERR_INVALID_ARG;goto done;}
  if(config.role!=NET_HOST){e=ESP_ERR_INVALID_STATE;goto done;}
  uint8_t local[3]={1,43,command[8]};e=dispatch_target_locked(local,3,command+2);goto done;
 }
 if(command[1]==39 && length==11){
  if(config.role!=NET_HOST){e=ESP_ERR_INVALID_STATE;goto done;}
  if(command[8]>1 || command[9]>1){e=ESP_ERR_INVALID_ARG;goto done;}
  uint8_t local[5]={1,38,command[8],command[9],command[10]};
  e=dispatch_target_locked(local,5,command+2);goto done;
 }
 if((command[1]==26 && length==16) || (command[1]==27 && length==9)){
  if(config.role!=NET_HOST){e=ESP_ERR_INVALID_STATE;goto done;}
  uint8_t state;uint32_t session;
  if(!controller_game_phase(&state,&session) || state==GAME_RUNNING || state==GAME_PAUSED){e=ESP_ERR_INVALID_STATE;goto done;}
  if(command[1]==27 && command[8]>1){e=ESP_ERR_INVALID_ARG;goto done;}
  uint8_t local[10]={1,command[1]==26?22:2};memcpy(local+2,command+8,length-8);
  e=dispatch_target_locked(local,length-6,command+2);goto done;
 }
 if((command[1]==34 && length==14) || ((command[1]==35 || command[1]==36) && length==8)){
  uint8_t state;uint32_t session;
  if(config.role!=NET_HOST || !controller_game_phase(&state,&session) || state==GAME_RUNNING || state==GAME_PAUSED){e=ESP_ERR_INVALID_STATE;goto done;}
  network_roster_t roster={.network=config.network};
  taskENTER_CRITICAL(&lock);roster.count=count;for(unsigned i=0;i<count;i++)memcpy(roster.mac[i],members[i].mac,6);taskEXIT_CRITICAL(&lock);
  network_roster_seal(&roster);
  const uint8_t *old=command[1]==36?NULL:command+2,*replacement=command[1]==35?NULL:command+(command[1]==34?8:2);
  if((replacement && !memcmp(replacement,local_mac,6)) || !network_roster_edit(&roster,old,replacement)){e=ESP_ERR_INVALID_ARG;goto done;}
  nvs_handle_t h;e=nvs_open("network",NVS_READWRITE,&h);
  if(e==ESP_OK){e=nvs_set_blob(h,"roster",&roster,sizeof(roster));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
  taskENTER_CRITICAL(&lock);roster_error=e;
  if(e==ESP_OK){
   /* Reset cached observations, sequence IDs and result roster after edits.
    * RX may run while NVS commits, but the committed roster wins atomically. */
   memset(member_slot_seq,0,sizeof(member_slot_seq));memset(member_slot_seen,0,sizeof(member_slot_seen));memset(event_clocks,0,sizeof(event_clocks));memset(event_clock_sequence,0,sizeof(event_clock_sequence));memset(event_receivers,0,sizeof(event_receivers));memset(members,0,sizeof(members));memset(pilot_sequence,0,sizeof(pilot_sequence));memset(terminal_sequence,0,sizeof(terminal_sequence));memset(heartbeat_sequence,0,sizeof(heartbeat_sequence));memset(config_sequence,0,sizeof(config_sequence));
   count=roster.count;for(unsigned i=0;i<count;i++)memcpy(members[i].mac,roster.mac[i],6);
   roster_fixed=true; /* Keep the last match roster until the next start. */
  }
  taskEXIT_CRITICAL(&lock);goto done;
 }
 if((command[1]==32 || command[1]==33) && length==2){
  uint8_t state;uint32_t session;
  if(config.role!=NET_HOST || !controller_game_phase(&state,&session) || state==GAME_RUNNING || state==GAME_PAUSED){e=ESP_ERR_INVALID_STATE;goto done;}
  network_roster_t roster={.network=config.network};
  taskENTER_CRITICAL(&lock);roster.count=count;for(unsigned i=0;i<count;i++)memcpy(roster.mac[i],members[i].mac,6);taskEXIT_CRITICAL(&lock);
  network_roster_seal(&roster);
  if(command[1]==32 && (!roster.count || !network_roster_valid(&roster,config.network))){e=ESP_ERR_INVALID_ARG;goto done;}
  nvs_handle_t h;e=nvs_open("network",NVS_READWRITE,&h);
  if(e==ESP_OK){e=command[1]==32?nvs_set_blob(h,"roster",&roster,sizeof(roster)):nvs_erase_key(h,"roster");if(e==ESP_ERR_NVS_NOT_FOUND)e=ESP_OK;if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
  taskENTER_CRITICAL(&lock);roster_error=e;if(e==ESP_OK)roster_fixed=command[1]==32;taskEXIT_CRITICAL(&lock);goto done;
 }
 if(command[1]==50){e=controller_execute_command(command,length);goto done;}
 if(command[1]==53){e=controller_execute_command(command,length);goto done;}
 if(command[1]==47){e=controller_execute_command(command,length);goto done;}
 if(command[1]==43){e=length==3 && command[2]<=1?controller_event_acquisition(command[2]):ESP_ERR_INVALID_ARG;goto done;}
 if(command[1]==42){e=length==2?controller_event_capture():ESP_ERR_INVALID_ARG;goto done;}
 if(command[1]==41){e=length==2?controller_game_save_result():ESP_ERR_INVALID_ARG;goto done;}
 if(command[1]==38 && length==5){e=controller_execute_command(command,length);goto done;}
 if(command[1]==25 && length==2){e=save_result_locked();goto done;}
 if(command[1]==19 && length==2){e=sync_locked();goto done;}
 if(config.role==NET_MEMBER){e=ESP_ERR_INVALID_STATE;goto done;}
 if(command[1]==23){
  if(length!=3 || command[2]>1){e=ESP_ERR_INVALID_ARG;goto done;}
  if(roster_error!=ESP_OK){e=roster_error;goto done;}
  e=controller_game_ready(command[2]);if(e!=ESP_OK)goto done;
  e=dispatch_locked(command,length);if(e!=ESP_OK)goto done;
  if(config.role==NET_HOST){
   int64_t now=esp_timer_get_time();bool ready=true;uint8_t state;uint32_t session;
   if(command[2] && (!controller_game_phase(&state,&session) || state!=GAME_PAUSED)){e=ESP_ERR_INVALID_STATE;goto done;}
   taskENTER_CRITICAL(&lock);
   if(command[2])ready=roster_session==session && match_roster==((1u<<(count+1))-1);
   for(unsigned i=0;i<count;i++){
    member_t *m=&members[i];
    ready&=m->timing_seen && now-m->timing_seen<3000000 && m->timing_valid && m->timing_uncertainty<=20000;
    if(command[2])ready&=roster_session==m->session && m->boot==roster_boot[i] && m->state==GAME_PAUSED;
   }
   taskEXIT_CRITICAL(&lock);if(!ready)e=ESP_ERR_INVALID_STATE;
  }
  goto done;
 }
 if(config.role==NET_HOST && (start || resume)){
  if(roster_error!=ESP_OK){e=roster_error;goto done;}
  e=controller_game_ready(resume);if(e!=ESP_OK)goto done;
  if(start){e=sync_locked();if(e!=ESP_OK)goto done;}
  uint8_t preflight[3]={1,23,resume};e=dispatch_locked(preflight,3);if(e!=ESP_OK)goto done;
  e=scheduled_start_locked(resume);goto done;
 }
 uint8_t session_command[6];
 if(start){
  uint32_t session=esp_random();if(!session)session=1;
  session_command[0]=1;session_command[1]=24;memcpy(session_command+2,&session,4);
  taskENTER_CRITICAL(&lock);roster_session=session;match_roster=(1u<<(count+1))-1;
  for(unsigned i=0;i<count;i++){roster_boot[i]=members[i].boot;memcpy(roster_mac[i],members[i].mac,6);}
  roster_started=esp_timer_get_time();taskEXIT_CRITICAL(&lock);
  command=session_command;length=6;
 }
 /* Cancelling a new start has no running match to finish. A pause during a
  * resume countdown is already satisfied. Still forward halt to peers that
  * may have consumed their deadline before the cancel arrived. */
 bool halt_satisfied=false;
 if(pending && halt){
  uint8_t state;uint32_t session;
  if(controller_game_phase(&state,&session))halt_satisfied=
   (state!=GAME_RUNNING && state!=GAME_PAUSED) ||
   (state==GAME_PAUSED && command[1]==10 && command[2]==3);
 }
 e=halt_satisfied?ESP_OK:controller_execute_command(command,length);
 if(e==ESP_OK && resume){taskENTER_CRITICAL(&lock);roster_started=esp_timer_get_time();taskEXIT_CRITICAL(&lock);}
 if(forward && (e==ESP_OK || halt)){esp_err_t remote=dispatch_locked(command,length);if(e==ESP_OK)e=remote;}
 if(e==ESP_OK && cancellation!=ESP_OK)e=cancellation;
 /* A partial start/resume must not leave the reachable nodes running. */
 if(e!=ESP_OK && (start || resume)){
  uint8_t pause[3]={1,10,3};controller_execute_command(pause,3);
  if(config.role==NET_HOST)dispatch_locked(pause,3);
 }
done:
 xSemaphoreGive(control_lock);return e;
}

/* Local event collection exists with or without the radio transport. */
static void collect_local_event(void){
 for(unsigned n=0;n<4;n++){
  unsigned slot=local_event_next++%4;service_event_t event;event_delivery_t delivery;
  if(controller_slot_event_peek(slot,local_event_cursor[slot],&event,&delivery)!=ESP_OK)continue;
  uint8_t payload[32];event_transport_encode_slot(&event,delivery.lost,slot,payload);packet_t identity=make_packet(12);
  taskENTER_CRITICAL(&lock);network_events_push(&network_events,local_mac,boot_id,identity.seq,payload,esp_timer_get_time());network_events.ring[(network_events.next+31)%32].time=event_host_map(payload,NULL,boot_id,boot_id,esp_timer_get_time(),true);taskEXIT_CRITICAL(&lock);local_event_cursor[slot]=delivery.position;break;
 }
}
static void standalone_event_worker(void *arg){
 (void)arg;for(;;){collect_local_event();vTaskDelay(pdMS_TO_TICKS(40));}
}
static void network_worker(void *arg){
 (void)arg;int64_t heartbeat=0,score_at=0,slot_at=0,clock_at=0,telemetry_at=0;unsigned slot_next=0;received_t r;
 controller_game_summary_t score_snapshot={0};unsigned score_next=8,telemetry_next=5;node_config_t telemetry_rf={0};
 for(;;){
  if(xQueueReceive(rx,&r,pdMS_TO_TICKS(40))==pdTRUE){
   process(&r);
   /* Bound the batch so queued replies are serviced before the next telemetry
    * burst without starving heartbeat/event production under sustained RX. */
   for(unsigned batch=1;batch<8 && xQueueReceive(rx,&r,0)==pdTRUE;batch++)process(&r);
  }
  /* Send clock probes independently, ahead of telemetry bursts. A lost probe
   * expires before replacement; one echoed timestamp remains outstanding. */
  if(config.role==NET_MEMBER && last_host_seen && esp_timer_get_time()-last_host_seen<5000000 && esp_timer_get_time()>=clock_at){
   packet_t request=make_packet(5);request.length=8;clock_request=esp_timer_get_time();++clock_probes;memcpy(request.data,&clock_request,8);send_packet(config.host,&request);clock_at=esp_timer_get_time()+250000;
  }
  if(config.role==NET_MEMBER && esp_timer_get_time()>=heartbeat){
   controller_game_summary_t game_summary;
   /* Do not publish a zero/partial heartbeat when the controller is busy. */
   if(!controller_game_summary_try(&game_summary)){++heartbeat_skips;heartbeat=esp_timer_get_time()+40000;}else {
   packet_t p=make_packet(1);p.length=24;
   p.data[0]=game_summary.state;p.data[1]=game_summary.mode;p.data[2]=game_summary.owner;p.data[3]=game_summary.fault;memcpy(p.data+12,&game_summary.session,4);uint32_t red=game_summary.team_us[0]/1000,blue=game_summary.team_us[1]/1000;memcpy(p.data+4,&red,4);memcpy(p.data+8,&blue,4);controller_snapshot_t status;controller_get_snapshot(&status);p.data[16]=status.node_online;p.data[17]=status.rf_valid;p.data[18]=status.calibrated;
   send_packet(config.host,&p);
   telemetry_rf=(node_config_t){status.channel,status.gain,status.enter_power,status.exit_power,status.confirm_ms};
   score_snapshot=game_summary;telemetry_next=0;telemetry_at=esp_timer_get_time()+40000;
   score_next=0;score_at=esp_timer_get_time()+240000;
   heartbeat=esp_timer_get_time()+1000000;
   }
  }
  /* One auxiliary report per step instead of a five-frame heartbeat burst.
   * Outcome and scores retain the same captured game summary as heartbeat. */
  if(config.role==NET_MEMBER && telemetry_next<5 && esp_timer_get_time()>=telemetry_at){
   packet_t report;
   switch(telemetry_next++){
    case 0:report=make_packet(11);report.length=28;memcpy(report.data,&score_snapshot.session,4);memcpy(report.data+4,score_snapshot.outcome,24);break;
    case 1:{report=make_packet(9);report.length=12;
     taskENTER_CRITICAL(&lock);uint32_t error=network_clock_error(clock_updated,esp_timer_get_time(),clock_uncertainty);report.data[0]=error<=20000;report.data[1]=scheduled.phase;memcpy(report.data+4,&error,4);memcpy(report.data+8,&scheduled.session,4);taskEXIT_CRITICAL(&lock);break;}
    case 2:report=make_packet(8);report.length=8;memcpy(report.data,&telemetry_rf,8);break;
    case 3:report=make_packet(14);report.length=12;memcpy(report.data,score_snapshot.acquisition,12);break;
    default:report=make_packet(10);report.length=32;io_service_status(report.data);break;
   }
   send_packet(config.host,&report);telemetry_at=esp_timer_get_time()+40000;
  }
  if(config.role==NET_MEMBER && esp_timer_get_time()>=slot_at){
   packet_t report=make_packet(18);report.length=32;
   if(controller_slot_config_page_try(slot_next,report.data)){send_packet(config.host,&report);slot_next=(slot_next+1)%4;slot_at=esp_timer_get_time()+250000;}
   else slot_at=esp_timer_get_time()+40000;
  }
  if(config.role==NET_MEMBER && score_next<8 && esp_timer_get_time()>=score_at){
   unsigned pilot=score_next++;packet_t score=make_packet(7);score.length=NETWORK_SCORE_BYTES;
   network_score_t value={.session=score_snapshot.session,.pilot=pilot,
    .owned_ms=score_snapshot.pilot_us[pilot]/1000,.captures=score_snapshot.pilot_captures[pilot],
    .laps=score_snapshot.pilot_laps[pilot],.fastest_ms=score_snapshot.pilot_fastest_ms[pilot]};
   network_score_encode(&value,score.data);
   send_packet(config.host,&score);score_at=esp_timer_get_time()+10000;
  }
  if(config.role==NET_MEMBER){
   taskENTER_CRITICAL(&lock);unsigned requested=bench_requested;bench_requested=0;
   if(requested){memset(&bench_sender,0,sizeof(bench_sender));bench_total=requested;bench_running=true;bench_deadline=esp_timer_get_time()+30000000;}
   taskEXIT_CRITICAL(&lock);
   if(bench_running){
    controller_snapshot_t status;controller_get_snapshot(&status);
    if(status.running || status.auto_gain_state==1 || esp_timer_get_time()>=bench_deadline || bench_sender.acked>=bench_total){bench_running=false;}
    else if(last_host_seen && esp_timer_get_time()-last_host_seen<5000000){
     event_sender_bind(&bench_sender,last_host_boot);
     if(!bench_sender.waiting){service_event_t e={.session=boot_id,.sequence=bench_sender.acked+1,.source_us=esp_timer_get_time(),.edge=1,.flags=1};
      bench_packet=make_packet(15);bench_packet.length=32;event_transport_encode_slot(&e,0,bench_sender.acked%4,bench_packet.data);event_sender_prepare(&bench_sender,bench_packet.seq,e.sequence,bench_packet.data);
     }
     if(event_sender_due(&bench_sender,esp_timer_get_time()))send_packet(config.host,&bench_packet);
    }
   }
  }
  if(config.role==NET_MEMBER){
   uint8_t state;uint32_t session;
   if(controller_game_phase_try(&state,&session) && state==GAME_RUNNING && schedule_abort_matches(cancelled_schedule,cancelled_fence,session,prepared_sequence) && !member_stop_pending(&member_stop))member_stop_request(&member_stop);
  }
  if(config.role==NET_MEMBER && member_stop_due(&member_stop,esp_timer_get_time())){
   bool capture_ok=!member_stop.capture_pending || controller_stop_all_capture()==ESP_OK;
   bool game_ok=!member_stop.game_pending || controller_game_fault_pause(6)==ESP_OK;
   member_stop_complete(&member_stop,capture_ok,game_ok);
  }
  if(config.role==NET_MEMBER && last_host_boot && last_host_seen && esp_timer_get_time()-last_host_seen<5000000){
   if(!event_sender.waiting){
    for(unsigned n=0;n<4;n++){
     unsigned slot=event_mux_next(&member_events);service_event_t e;event_delivery_t delivery;
     if(controller_slot_event_peek(slot,member_events.cursor[slot],&e,&delivery)!=ESP_OK)continue;
     event_pending=make_packet(12);event_pending.length=32;event_mux_prepare(&member_events,&event_sender,slot,event_pending.seq,&e,&delivery,event_pending.data);
     break;
    }
   }
   if(event_sender_due(&event_sender,esp_timer_get_time()))send_packet(config.host,&event_pending);
  }
  if(config.role==NET_HOST)collect_local_event();
  if(config.role==NET_HOST && esp_timer_get_time()>=heartbeat){
   packet_t p=make_packet(4);taskENTER_CRITICAL(&lock);p.length=8;memcpy(p.data,&cancelled_schedule,4);memcpy(p.data+4,&cancelled_fence,4);taskEXIT_CRITICAL(&lock);const uint8_t broadcast[6]={255,255,255,255,255,255};send_packet(broadcast,&p);
   for(unsigned i=0;i<count;i++)send_packet(members[i].mac,&p);
   heartbeat=esp_timer_get_time()+1000000;
  }
  if(config.role==NET_MEMBER && last_host_seen && esp_timer_get_time()-last_host_seen>5000000){
   member_stop_request(&member_stop);last_host_seen=0;
  }
 }
}
static void guard_worker(void *arg){
 (void)arg;uint8_t state;uint32_t session;uint64_t observed_fault=0;
 for(;;){
  vTaskDelay(pdMS_TO_TICKS(10));
  int64_t schedule_now=esp_timer_get_time();
  taskENTER_CRITICAL(&lock);
  bool healthy=config.role!=NET_MEMBER || (last_host_seen && schedule_now-last_host_seen<3000000 && network_clock_error(clock_updated,schedule_now,clock_uncertainty)<=20000);
  if(config.role==NET_HOST && scheduled.phase){
   for(unsigned i=0;i<count;i++)if((match_roster&(1u<<(i+1))) && (members[i].boot!=roster_boot[i] || !members[i].snapshot_seen || schedule_now-members[i].snapshot_seen>3000000))healthy=false;
  }
  uint8_t before_phase=scheduled.phase,action=scheduled.action;uint32_t before_session=scheduled.session;
  uint32_t due=start_schedule_take(&scheduled,schedule_now,healthy);
  if(before_phase && !scheduled.phase && !due){scheduled_error=ESP_ERR_TIMEOUT;if(config.role==NET_HOST){cancelled_schedule=before_session;cancelled_fence=sequence;}}
  taskEXIT_CRITICAL(&lock);
  if(due){esp_err_t e=action==2?controller_game_resume_session(due):controller_game_start_session(due);taskENTER_CRITICAL(&lock);scheduled_error=e;if(e!=ESP_OK && config.role==NET_HOST){cancelled_schedule=due;cancelled_fence=sequence;}taskEXIT_CRITICAL(&lock);ESP_LOGI("network","SCHEDULE_FIRE session=%"PRIu32" local_us=%"PRIi64" result=%s",due,schedule_now,esp_err_to_name(e));}
  controller_game_summary_t running;
  if(config.role!=NET_HOST || !controller_game_summary_try(&running))continue;
  /* Consume the controller's revision, even if no running snapshot was read.
   * Do not revoke an explicit resume already completed before observation. */
  bool new_fault=running.fault_revision!=observed_fault;
  observed_fault=running.fault_revision;
  if(new_fault && running.state==GAME_PAUSED && running.fault){
   taskENTER_CRITICAL(&lock);cancelled_schedule=running.session;cancelled_fence=sequence;taskEXIT_CRITICAL(&lock);
  }
  if(running.state!=GAME_RUNNING)continue;
  session=running.session;
  network_guard_node_t nodes[8]={0};uint32_t boots[8];uint16_t roster;
  int64_t now=esp_timer_get_time(),started;
  taskENTER_CRITICAL(&lock);roster=roster_session==session?match_roster:0;started=roster_started;memcpy(boots,roster_boot,sizeof(boots));
  for(unsigned i=0;i<count;i++){member_t *m=&members[i];nodes[i]=(network_guard_node_t){.age_ms=m->snapshot_seen?(now-m->snapshot_seen)/1000:UINT32_MAX,.session=m->session,.boot=m->boot,.state=m->state,.mode=m->mode};}
  taskEXIT_CRITICAL(&lock);
  uint16_t missing=network_guard_missing(session,running.mode,roster,(now-started)/1000,boots,nodes);
  if(!missing)continue;
  /* Local safety must not queue behind a 120s configuration transaction.
   * Session fencing rejects a stale observation after a new match starts.
   * Repeated heartbeat cancellation reaches peers without an ACK transaction. */
  if(controller_game_fault_pause_session(6,session)==ESP_OK){
   taskENTER_CRITICAL(&lock);cancelled_schedule=session;cancelled_fence=sequence;taskEXIT_CRITICAL(&lock);
   ESP_LOGW("network","GUARD paused missing_mask=%u",missing);
  }
 }
}
static void result_worker(void *arg){
 (void)arg;uint8_t state;uint32_t session;
 uint32_t pending=0,attempted=0;int64_t finished_at=0;
 for(;;){
  vTaskDelay(pdMS_TO_TICKS(200));
  if(!controller_game_phase(&state,&session))continue;
  if(state!=GAME_FINISHED || !session){pending=0;continue;}
  if(pending!=session){pending=session;finished_at=esp_timer_get_time();}
  /* Wait for terminal heartbeat + all final pilot reports; bound missing nodes.
   * A failed NVS write is exposed and can be explicitly retried by the user. */
  taskENTER_CRITICAL(&lock);uint32_t recorded=0;memcpy(&recorded,saved_result.pages[0]+4,4);bool already=saved_result.revision && recorded==pending && saved_result.network==config.network;taskEXIT_CRITICAL(&lock);
  if(already || attempted==pending || esp_timer_get_time()-finished_at<2000000)continue;
  if(control_begin()!=ESP_OK)continue;
  score_snapshot_t final;
  if(!capture_scores(&final,true) || final.session!=pending || final.state!=GAME_FINISHED){xSemaphoreGive(control_lock);continue;}
  uint16_t missing=final.roster&~final.outcome_mask;
  for(unsigned page=0;page<RESULT_PAGES;page++){uint8_t bytes[32];uint16_t mask;encode_scores(&final,bytes,page);memcpy(&mask,bytes+12,2);missing|=mask;}
  if(!result_settle_ready(esp_timer_get_time()-finished_at,missing)){xSemaphoreGive(control_lock);continue;}
  attempted=pending;esp_err_t error=save_result_snapshot(&final);
  taskENTER_CRITICAL(&lock);result_save_error=error;taskEXIT_CRITICAL(&lock);
  ESP_LOGI("network","AUTORESULT session=%"PRIu32" result=%s",pending,esp_err_to_name(error));
  xSemaphoreGive(control_lock);
 }
}
esp_err_t network_start(void){
 control_lock=xSemaphoreCreateMutex();if(!control_lock)return ESP_ERR_NO_MEM;
 esp_err_t identity_error=esp_read_mac(local_mac,ESP_MAC_WIFI_STA);if(identity_error!=ESP_OK)return identity_error;
 boot_id=esp_random();if(!boot_id)boot_id=1;
 nvs_handle_t result_handle;if(nvs_open("net_results",NVS_READONLY,&result_handle)==ESP_OK){
  result_history_t *loaded=malloc(sizeof(*loaded));
  if(!loaded){nvs_close(result_handle);return ESP_ERR_NO_MEM;}
  size_t size=sizeof(*loaded);esp_err_t e=nvs_get_blob(result_handle,"history",loaded,&size);
  if(e==ESP_OK && size==sizeof(*loaded) && result_history_valid(loaded)){history=*loaded;if(history.count)saved_result=history.records[0];}
  else if(e!=ESP_ERR_NVS_NOT_FOUND)result_save_error=ESP_ERR_INVALID_RESPONSE;
  free(loaded);nvs_close(result_handle);
 }
 config=(network_config_t){.channel=6};nvs_handle_t h;
 esp_err_t config_error=nvs_open("network",NVS_READONLY,&h);
 if(config_error==ESP_OK){
  network_config_t stored={0};size_t n=sizeof(stored);
  config_error=nvs_get_blob(h,"config",&stored,&n);nvs_close(h);
  if(config_error==ESP_OK){
   if(network_config_restore(&config,&stored,n==sizeof(stored)))saved_network_update(&config,false);
   else config_error=ESP_ERR_INVALID_RESPONSE;
  }
 }
 if(config_error==ESP_ERR_NVS_NOT_FOUND)config_error=ESP_OK;
 if(config_error!=ESP_OK){
  taskENTER_CRITICAL(&lock);memcpy(saved_network_page+16,&config_error,4);taskEXIT_CRITICAL(&lock);
  ESP_LOGW("network","Saved network rejected: %s; standalone recovery",esp_err_to_name(config_error));
 }
 if(config.role==NET_HOST){
  nvs_handle_t rh;if(nvs_open("network",NVS_READONLY,&rh)==ESP_OK){
   network_roster_t r;size_t size=sizeof(r);esp_err_t e=nvs_get_blob(rh,"roster",&r,&size);nvs_close(rh);
   if(e==ESP_OK && size==sizeof(r) && network_roster_valid(&r,config.network)){roster_fixed=true;count=r.count;for(unsigned i=0;i<count;i++)memcpy(members[i].mac,r.mac[i],6);}
   else if(e!=ESP_ERR_NVS_NOT_FOUND){roster_error=ESP_ERR_INVALID_RESPONSE;roster_fixed=true;}
  }
 }
 if(config.role!=NET_MEMBER && xTaskCreate(result_worker,"net_results",8192,NULL,2,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
 if(!config.role)return xTaskCreate(standalone_event_worker,"local_events",3072,NULL,3,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
 if(config.role>2 || config.channel<1 || config.channel>13)return ESP_ERR_INVALID_STATE;
 psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
 if(psa_crypto_init()!=PSA_SUCCESS)return ESP_FAIL;
 psa_set_key_type(&attributes,PSA_KEY_TYPE_HMAC);psa_set_key_bits(&attributes,128);
 psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_SIGN_MESSAGE);psa_set_key_algorithm(&attributes,PSA_ALG_HMAC(PSA_ALG_SHA_256));
 if(psa_import_key(&attributes,config.key,16,&auth_key)!=PSA_SUCCESS)return ESP_FAIL;
 rx=xQueueCreate(12,sizeof(received_t));if(!rx)return ESP_ERR_NO_MEM;
 esp_err_t e=esp_netif_init();if(e!=ESP_OK)return e;e=esp_event_loop_create_default();if(e!=ESP_OK && e!=ESP_ERR_INVALID_STATE)return e;
 wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();if((e=esp_wifi_init(&init))!=ESP_OK)return e;
 esp_wifi_set_storage(WIFI_STORAGE_RAM);esp_wifi_set_mode(WIFI_MODE_STA);if((e=esp_wifi_start())!=ESP_OK)return e;
 if((e=esp_wifi_set_ps(WIFI_PS_NONE))!=ESP_OK)return e;
 if((e=esp_wifi_set_channel(config.channel,WIFI_SECOND_CHAN_NONE))!=ESP_OK)return e;
 esp_wifi_get_mac(WIFI_IF_STA,local_mac);
 if((e=esp_now_init())!=ESP_OK)return e;
 if((e=esp_now_register_recv_cb(receive))!=ESP_OK)return e;
 if((e=esp_now_register_send_cb(sent))!=ESP_OK)return e;
 esp_now_set_wake_window(65535);
 esp_wifi_connectionless_module_set_wake_interval(100);
 esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
 if(xTaskCreate(network_worker,"espnow_rx",8192,NULL,3,NULL)!=pdPASS)return ESP_ERR_NO_MEM;
 return xTaskCreate(guard_worker,"network_guard",4096,NULL,3,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
