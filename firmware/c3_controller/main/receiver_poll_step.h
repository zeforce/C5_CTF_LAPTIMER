#pragma once
#include "receiver_poll.h"
/* A sweep performs at most one transport request per call. Caller restarts
 * phase zero after external mutations; live state publishes only at commit. */
enum {RP_INFO,RP_STOP,RP_OBS,RP_STOP_OBS,RP_CONFIG,RP_STOP_WRITE,RP_WRITE,RP_VERIFY,RP_FINAL};
typedef struct {
 unsigned phase;uint32_t session;bool reboot,was_active,writing,have_target;
 receiver_config_t target;
 uint8_t info[17],obs[16],confirmed[8];
} receiver_poll_step_t;
static inline int32_t receiver_poll_step(receiver_service_t *s,unsigned slot,receiver_poll_step_t *p,
 receiver_exchange_t exchange,void *ctx,uint64_t now,bool *done){
 if(done)*done=false;
 if(!s || slot>=RECEIVER_SLOTS || !p || !exchange || !done || !s->slots[slot].enabled)return -1;
 receiver_slot_t *r=&s->slots[slot];
 /* Desired profiles can change without a transport mutation. Discard the
  * old sweep before it can publish or write against a newly saved profile. */
 if(p->phase && (p->have_target!=r->have_desired || (r->have_desired && memcmp(&p->target,&r->desired,8))))p->phase=RP_INFO;
 int32_t e=0;uint8_t stop[2]={0},final[17]={0};
 switch(p->phase){
 case RP_INFO:
  memset(p,0,sizeof(*p));p->have_target=r->have_desired;p->target=r->desired;p->was_active=r->active || r->capture_requested;
  e=exchange(ctx,slot,1,NULL,0,17,p->info);p->session=receiver_u32(p->info+6);
  if(!e && !p->session)e=-1;
  if(!e){p->reboot=r->session!=p->session;if(p->reboot){r->capture_requested=false;r->overflow_baseline=false;r->stop_pending=true;r->online=r->rf_valid=r->active=r->calibrated=false;r->clock.valid=false;}
   p->phase=(p->reboot || r->stop_pending)?RP_STOP:RP_OBS;}break;
 case RP_STOP:case RP_STOP_OBS:case RP_STOP_WRITE:
  r->stop_pending=true;r->capture_requested=false;
  e=exchange(ctx,slot,8,stop,2,0,NULL);
  if(!e){r->stop_pending=false;r->active=r->calibrated=false;r->overflow_baseline=false;p->obs[13]=p->obs[14]=0;
   p->phase=p->phase==RP_STOP?RP_OBS:p->phase==RP_STOP_OBS?RP_CONFIG:RP_WRITE;}break;
 case RP_OBS:
  e=exchange(ctx,slot,5,NULL,0,16,p->obs);
  if(!e){
   uint32_t overflow=receiver_u32(p->obs+8);
   if(r->capture_requested && (!p->obs[13] || !p->info[11] || p->info[12] || (r->overflow_baseline && overflow!=r->capture_overflow))){r->capture_requested=false;r->stop_pending=true;r->capture_error=-2;}
   else if(r->capture_requested){r->capture_overflow=overflow;r->overflow_baseline=true;}
   p->phase=(r->stop_pending || (p->obs[13] && !r->capture_requested))?RP_STOP_OBS:RP_CONFIG;
  }break;
 case RP_CONFIG:
  e=exchange(ctx,slot,3,NULL,0,8,p->confirmed);
  if(!e){p->writing=r->have_desired && (p->reboot || memcmp(p->confirmed,&r->desired,8));
   p->phase=p->writing?(p->obs[13]?RP_STOP_WRITE:RP_WRITE):RP_FINAL;}break;
 case RP_WRITE:
  r->capture_requested=false;p->obs[14]=0;
  e=exchange(ctx,slot,4,(const uint8_t*)&r->desired,8,0,NULL);r->config_error=e;
  if(!e)p->phase=RP_VERIFY;
  break;
 case RP_VERIFY:
  e=exchange(ctx,slot,3,NULL,0,8,p->confirmed);
  if(!e && memcmp(p->confirmed,&r->desired,8))e=-1;
  r->config_error=e;if(!e)p->phase=RP_FINAL;break;
 case RP_FINAL:
  e=exchange(ctx,slot,1,NULL,0,17,final);
  if(!e && (receiver_u32(final+6)!=p->session || final[10]!=p->confirmed[0]))e=-1;
  if(!e){memcpy(&r->observed,p->confirmed,8);r->have_observed=true;
   if(r->have_desired && !memcmp(&r->desired,p->confirmed,8))r->config_error=0;
   receiver_health(s,slot,0,p->session,final[11] && !final[12],p->obs[14]!=0,p->obs[13]!=0,now);*done=true;p->phase=RP_INFO;
  }break;
 default:e=-1;
 }
 if(e){if(p->was_active || r->active || r->capture_requested || r->stop_pending){r->capture_requested=false;r->stop_pending=true;r->capture_error=e;}
  receiver_health(s,slot,e,p->session,false,false,false,now);p->phase=RP_INFO;*done=true;}
 return e;
}
