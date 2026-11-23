#pragma once
#include "receiver_poll.h"
/* One selected queue per worker turn; no low-slot priority under load. */
static inline int receiver_event_next(const receiver_service_t *s,uint8_t *next){
 if(!s || !next)return -1;
 for(unsigned n=0;n<RECEIVER_SLOTS;n++){
  unsigned slot=(*next+n)%RECEIVER_SLOTS;const receiver_slot_t *r=&s->slots[slot];
  if(r->enabled && r->online && r->active && !r->stop_pending && !r->restore_pending){*next=(slot+1)%RECEIVER_SLOTS;return slot;}
 }
 return -1;
}
/* A bounded drain preserves source identity. It never starts RF acquisition.
 * Caller serializes the slot and explicitly authorizes draining its queue.
 * Rejected old/duplicate journal entries still ACK: they are already consumed.
 * Malformed edges are not ACKed and return a protocol error. */
static inline int32_t receiver_drain_events(receiver_service_t *s,unsigned slot,receiver_exchange_t exchange,receiver_now_t now,void *ctx,unsigned budget,unsigned *drained){
 if(drained)*drained=0;
 if(!s || slot>=RECEIVER_SLOTS || !exchange || !now || !drained || !budget || budget>16)return -1;
 receiver_slot_t *r=&s->slots[slot];if(!r->enabled || !r->online || !r->session)return -1;
 for(unsigned n=0;n<budget;n++){
  uint8_t data[18]={0};int32_t error=exchange(ctx,slot,6,NULL,0,18,data);if(error)return error;
  uint32_t sequence=receiver_u32(data);if(!sequence)return 0;
  if(data[16]<1 || data[16]>2 || data[17]>1)return -1;
  service_event_t event={.session=r->session,.sequence=sequence,.source_us=(uint64_t)receiver_u32(data+4)|((uint64_t)receiver_u32(data+8)<<32),.edge=data[16],.flags=data[17]?1:0};
  event_service_push(&r->events,&event,&r->clock,now(ctx));
  /* ACK only after journal processing; lost ACK may repeat the same source. */
  error=exchange(ctx,slot,7,data,4,0,NULL);if(error)return error;
  ++*drained;
 }return 0;
}
