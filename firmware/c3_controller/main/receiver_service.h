#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "event_service.h"
#define RECEIVER_SLOTS 4
/* Desired/observed RF configuration is separate from transport and business. */
typedef struct {uint8_t channel,gain;uint16_t enter_power,exit_power,confirm_ms;} receiver_config_t;
typedef struct {
 bool enabled,online,rf_valid,calibrated,active,have_desired,have_observed;
 bool capture_requested,stop_pending,overflow_baseline,restore_pending,stop_keep_calibrated;uint32_t capture_overflow;int32_t capture_error;
 uint32_t session,polls,errors,restarts;int32_t last_error,config_error;uint64_t updated_us;
 receiver_config_t desired,observed;
 node_time_t clock;event_service_t events;
} receiver_slot_t;
typedef struct {receiver_slot_t slots[RECEIVER_SLOTS];} receiver_service_t;
/* Latch first, execute later. Fault handling must not wait for transport. */
static inline void receiver_request_stop(receiver_service_t *s,uint8_t mask,int32_t reason){
 if(!s)return;
 for(unsigned slot=0;slot<RECEIVER_SLOTS;slot++)if((mask&(1u<<slot)) && s->slots[slot].enabled){
  receiver_slot_t *r=&s->slots[slot];r->capture_requested=false;r->stop_pending=true;r->stop_keep_calibrated=false;r->calibrated=false;r->capture_error=reason;
 }
}
/* A normal pause can retain calibration only with a healthy source. Existing
 * safety stops always take precedence over a later pause request. */
static inline void receiver_request_pause(receiver_service_t *s,uint8_t mask){
 if(!s)return;
 for(unsigned slot=0;slot<RECEIVER_SLOTS;slot++)if((mask&(1u<<slot)) && s->slots[slot].enabled){
  receiver_slot_t *r=&s->slots[slot];
  bool keep=!r->stop_pending && r->online && r->rf_valid && r->calibrated && !r->restore_pending;
  r->capture_requested=false;r->stop_pending=true;r->stop_keep_calibrated=keep;
 }
}
static inline int receiver_pending_stop(const receiver_service_t *s,uint8_t *cursor,uint8_t excluded){
 if(!s || !cursor)return -1;
 for(unsigned n=0;n<RECEIVER_SLOTS;n++){
  unsigned slot=(*cursor+n)%RECEIVER_SLOTS;
  if(!(excluded&(1u<<slot)) && s->slots[slot].enabled && s->slots[slot].stop_pending){*cursor=(slot+1)%RECEIVER_SLOTS;return slot;}
 }
 return -1;
}
/* Keep failed-stop intent busy even after health clears the observed active bit. */
static inline bool receiver_capture_busy(const receiver_slot_t *r){return r && (r->active || r->capture_requested || r->stop_pending || r->restore_pending);}
/* A timeout is an uncertain hardware outcome. Only an acknowledged stop clears
 * the obligation; cached health and reconnect must never clear it. */
static inline void receiver_stop_result(receiver_slot_t *r,int32_t error){
 r->capture_requested=false;r->stop_pending=error!=0;r->capture_error=error;
 if(!error){r->active=false;r->overflow_baseline=false;r->stop_keep_calibrated=false;}
 else {r->calibrated=false;r->stop_keep_calibrated=false;}
}
static inline bool receiver_config_valid(const receiver_config_t *c){return c && c->channel<64 && c->gain<64 && c->enter_power>c->exit_power && c->confirm_ms && c->confirm_ms<=1000;}
static inline bool receiver_desire(receiver_service_t *s,unsigned index,const receiver_config_t *config){
 if(!s || index>=RECEIVER_SLOTS || !receiver_config_valid(config))return false;
 s->slots[index].desired=*config;s->slots[index].have_desired=true;return true;
}
/* Failure invalidates only this slot's live/clock state; old events and desired
 * config remain diagnostic history. Polling/retry policy belongs to adapter. */
static inline bool receiver_health(receiver_service_t *s,unsigned index,int32_t error,uint32_t session,bool rf,bool calibrated,bool active,uint64_t now){
 if(!s || index>=RECEIVER_SLOTS || !s->slots[index].enabled)return false;
 receiver_slot_t *r=&s->slots[index];++r->polls;r->last_error=error;r->updated_us=now;
 if(error || !session){++r->errors;r->online=r->rf_valid=r->calibrated=r->active=false;r->stop_keep_calibrated=false;r->clock.valid=false;return true;}
 if(r->session && r->session!=session){++r->restarts;r->stop_keep_calibrated=false;memset(&r->clock,0,sizeof(r->clock));}
 r->session=session;r->online=true;r->rf_valid=rf;r->calibrated=calibrated;r->active=active;return true;
}
/* Pages106..109, format24: one live cache per slot (not multi-page atomic). */
static inline bool receiver_page(const receiver_service_t *s,unsigned index,uint64_t now,uint8_t out[32]){
 if(!s || !out || index>=RECEIVER_SLOTS)return false;
 const receiver_slot_t *r=&s->slots[index];memset(out,0,32);out[0]=24;out[1]=index;
 out[2]=r->enabled;out[3]=(r->online?1:0)|(r->rf_valid?2:0)|(r->calibrated?4:0)|(r->active?8:0)|(r->have_desired?16:0)|(r->have_observed?32:0);
 memcpy(out+4,&r->session,4);memcpy(out+8,&r->last_error,4);memcpy(out+12,&r->errors,4);
 uint64_t elapsed=now>=r->updated_us?(now-r->updated_us)/1000:UINT64_MAX;uint32_t age=r->polls && elapsed<UINT32_MAX?elapsed:UINT32_MAX;memcpy(out+16,&age,4);
 memcpy(out+20,&r->desired,8);memcpy(out+28,&r->restarts,4);return true;
}

/* A saved profile is usable only after live readback confirms every field.
 * No desired profile is a valid state for discovery-only receivers. */
static inline bool receiver_config_pending(const receiver_slot_t *r){
 return r->have_desired && (!r->online || !r->have_observed || memcmp(&r->desired,&r->observed,8));
}
static inline bool receiver_config_ready(const receiver_slot_t *r){
 return r && !r->config_error && !receiver_config_pending(r);
}

/* Pages110..113 format25: saved desire vs observed. Pending describes config
 * convergence only, not calibration or readiness. */
static inline bool receiver_config_page(const receiver_service_t *s,unsigned slot,uint8_t out[32]){
 if(!s || !out || slot>=RECEIVER_SLOTS)return false;
 const receiver_slot_t *r=&s->slots[slot];memset(out,0,32);out[0]=25;out[1]=slot;out[2]=r->have_desired;out[3]=r->have_observed;
 memcpy(out+4,&r->desired,8);memcpy(out+12,&r->observed,8);memcpy(out+20,&r->config_error,4);
 out[25]=(r->enabled?1:0)|(r->online?2:0)|(r->rf_valid?4:0)|(r->calibrated?8:0)|(r->active?16:0)|(r->capture_requested?32:0)|(r->stop_pending?64:0);memcpy(out+26,&r->capture_error,4);
 out[30]=r->restore_pending;
 out[24]=receiver_config_pending(r);return true;
}
