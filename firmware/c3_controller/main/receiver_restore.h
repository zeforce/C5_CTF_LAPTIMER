#pragma once
#include "receiver_poll.h"
typedef struct {unsigned phase;receiver_config_t target;} receiver_restore_step_t;
/* Exactly one request per call. Targets stay owned by the caller until done.
 * Restarting after an error begins with STOP, never an unverified SET. */
static inline int32_t receiver_restore_step(receiver_service_t *s,unsigned slot,const receiver_config_t *target,
 receiver_restore_step_t *p,receiver_exchange_t exchange,void *ctx,int32_t mismatch,bool *done){
 if(done)*done=false;
 if(!s || slot>=RECEIVER_SLOTS || !receiver_config_valid(target) || !p || !exchange || !mismatch || !done || !s->slots[slot].enabled)return -1;
 receiver_slot_t *r=&s->slots[slot];r->restore_pending=true;r->capture_requested=false;
 if(!p->phase || memcmp(&p->target,target,8)){p->phase=0;p->target=*target;}
 uint8_t stop[2]={0},confirmed[8];int32_t e;
 if(!p->phase){
  r->stop_pending=true;e=exchange(ctx,slot,8,stop,2,0,NULL);receiver_stop_result(r,e);
  if(!e){r->calibrated=false;p->phase=1;}
 }else if(p->phase==1){
  e=exchange(ctx,slot,4,(const uint8_t*)&p->target,8,0,NULL);if(!e)p->phase=2;
 }else {
  e=exchange(ctx,slot,3,NULL,0,8,confirmed);
  if(!e && memcmp(confirmed,&p->target,8))e=mismatch;
  if(!e){r->observed=p->target;r->have_observed=true;r->restore_pending=false;p->phase=0;*done=true;}
 }
 r->config_error=e;if(e)p->phase=0;
 return e;
}
/* Adapter provides its deadline and mismatch error. A failed operation never
 * discards the target. Do not publish an observed configuration before GET. */
static inline int32_t receiver_restore(receiver_service_t *s,unsigned slot,const receiver_config_t *target,receiver_exchange_t exchange,void *ctx,int32_t mismatch){
 if(!s || slot>=RECEIVER_SLOTS || !receiver_config_valid(target) || !exchange || !mismatch)return -1;
 receiver_slot_t *r=&s->slots[slot];if(!r->enabled)return -1;
 r->restore_pending=true;r->capture_requested=false;
 uint8_t stop[2]={0,0},confirmed[8];
 int32_t e=exchange(ctx,slot,8,stop,2,0,NULL);
 receiver_stop_result(r,e);
 if(!e){r->calibrated=false;e=exchange(ctx,slot,4,(const uint8_t*)target,8,0,NULL);}
 if(!e)e=exchange(ctx,slot,3,NULL,0,8,confirmed);
 if(!e && memcmp(confirmed,target,8))e=mismatch;
 r->config_error=e;
 if(!e){r->observed=*target;r->have_observed=true;r->restore_pending=false;}
 return e;
}
