#pragma once
#include "receiver_service.h"
/* Transport-neutral poll used by additional slots. Adapter serializes access.
 * No automatic re-arm after reboot or transport failure. Callback returns 0 on success. */
typedef int32_t (*receiver_exchange_t)(void *,unsigned,uint8_t,const uint8_t *,uint8_t,uint8_t,uint8_t *);
static inline uint32_t receiver_u32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline int32_t receiver_poll(receiver_service_t *s,unsigned slot,receiver_exchange_t exchange,void *ctx,uint64_t now){
 if(!s || slot>=RECEIVER_SLOTS || !s->slots[slot].enabled || !exchange)return -1;
 receiver_slot_t *r=&s->slots[slot];bool was_active=r->active || r->capture_requested;uint8_t info[17]={0},obs[16]={0},confirmed[8]={0},stop[2]={0};
 int32_t e=exchange(ctx,slot,1,NULL,0,17,info);uint32_t session=receiver_u32(info+6);
 if(!e && !session)e=-1;
 bool reboot=!r->session || r->session!=session;
 if(!e && reboot){r->capture_requested=false;r->overflow_baseline=false;}
 if(!e && (reboot || r->stop_pending))e=exchange(ctx,slot,8,stop,2,0,NULL);
 if(!e && (reboot || r->stop_pending))r->stop_pending=false;
 if(!e)e=exchange(ctx,slot,5,NULL,0,16,obs);
 if(!e && r->capture_requested){
  uint32_t overflow=receiver_u32(obs+8);
  if(!obs[13] || !info[11] || info[12] || (r->overflow_baseline && overflow!=r->capture_overflow)){
   r->capture_requested=false;r->stop_pending=true;r->capture_error=-2;
  }else {r->capture_overflow=overflow;r->overflow_baseline=true;}
 }
 if(!e && obs[13] && (!r->capture_requested || r->stop_pending)){e=exchange(ctx,slot,8,stop,2,0,NULL);if(!e){obs[13]=obs[14]=0;r->stop_pending=false;}}
 if(!e)e=exchange(ctx,slot,3,NULL,0,8,confirmed);
 if(!e && r->have_desired && (reboot || memcmp(confirmed,&r->desired,8))){
  if(obs[13]){e=exchange(ctx,slot,8,stop,2,0,NULL);if(!e)obs[13]=0;}
  if(!e)e=exchange(ctx,slot,4,(const uint8_t*)&r->desired,8,0,NULL);
  if(!e)e=exchange(ctx,slot,3,NULL,0,8,confirmed);
  if(!e && memcmp(confirmed,&r->desired,8))e=-1;
  r->config_error=e;obs[14]=0;r->capture_requested=false;
 }
 if(!e){memcpy(&r->observed,confirmed,8);r->have_observed=true;if(r->have_desired && !memcmp(confirmed,&r->desired,8))r->config_error=0;}
 receiver_health(s,slot,e,session,!e && info[11] && !info[12],!e && obs[14],!e && obs[13],now);
 if(e && (was_active || r->capture_requested)){r->capture_requested=false;r->stop_pending=true;r->capture_error=e;}
 return e;
}

/* Clock sampling failure affects mapping quality, not RF/config health. */
typedef uint64_t (*receiver_now_t)(void *);
static inline bool receiver_poll_clock(receiver_service_t *s,unsigned slot,receiver_exchange_t exchange,receiver_now_t get_time,void *ctx){
 if(!s || slot>=RECEIVER_SLOTS || !exchange || !get_time)return false;
 receiver_slot_t *r=&s->slots[slot];if(!r->enabled || !r->online || !r->session){r->clock.valid=false;return false;}
 uint64_t start=get_time(ctx);
 if(r->clock.valid && start>=r->clock.sampled_us && start-r->clock.sampled_us<1000000)return true;
 uint8_t data[12]={0};int32_t error=exchange(ctx,slot,11,NULL,0,12,data);uint64_t end=get_time(ctx);
 if(error || receiver_u32(data+8)!=r->session){r->clock.valid=false;++r->clock.rejected;return false;}
 uint64_t node=(uint64_t)receiver_u32(data)|((uint64_t)receiver_u32(data+4)<<32);
 return node_time_sample(&r->clock,r->session,node,start,end);
}
static inline bool receiver_clock_page(const receiver_service_t *s,unsigned slot,uint64_t now,uint8_t out[32]){
 if(!s || slot>=RECEIVER_SLOTS || !out)return false;
 const node_time_t *c=&s->slots[slot].clock;uint64_t mapped;uint32_t uncertainty;
 memset(out,0,32);out[0]=27;out[1]=slot;out[2]=node_time_map(c,s->slots[slot].session,c->node_us,now,&mapped,&uncertainty);
 memcpy(out+4,&c->session,4);memcpy(out+8,&c->rtt_us,4);
 uint64_t elapsed=now>=c->sampled_us?(now-c->sampled_us)/1000:UINT64_MAX;uint32_t age=c->valid && elapsed<UINT32_MAX?elapsed:UINT32_MAX;
 memcpy(out+12,&age,4);memcpy(out+16,&c->samples,4);memcpy(out+20,&c->rejected,4);memcpy(out+24,&c->offset_us,8);return true;
}

/* action0 stop/retain calibration, 1 calibrate stopped, 2 start, 3 uncalibrate.
 * Failure while stopping retains a latch consumed by the poller. */
static inline int32_t receiver_capture_action(receiver_service_t *s,unsigned slot,unsigned action,receiver_exchange_t exchange,void *ctx){
 if(!s || slot>=RECEIVER_SLOTS || action>3 || !exchange)return -1;
 receiver_slot_t *r=&s->slots[slot];if(!r->enabled)return -1;
 if(action==0 || action==3){r->capture_requested=false;r->stop_pending=true;}
 else if(!receiver_config_ready(r) || !r->online || !r->rf_valid || r->stop_pending || r->restore_pending || (action==1 && r->active) || (action==2 && !r->calibrated))return -1;
 uint8_t input[2]={action==2,action==1 || action==2 || (action==0 && r->calibrated)};
 int32_t e=exchange(ctx,slot,8,input,2,0,NULL);r->capture_error=e;
 if(e){if(action==2){r->capture_requested=false;r->stop_pending=true;}return e;}
 r->active=action==2;r->calibrated=input[1];r->capture_requested=action==2;r->stop_pending=false;r->overflow_baseline=false;return 0;
}
