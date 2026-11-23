#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "event_service.h"
#include "network_sequence.h"
typedef struct {uint32_t boot,sequence,accepted,duplicates,rejected;uint64_t lost;uint8_t payload[32];} event_receiver_t;
/* 32-byte payload: C5 session/sequence, source/C3 times, uncertainty,
 * edge, flags, saturated number of overwritten source events before this one. */
static inline void event_transport_encode(const service_event_t *e,uint64_t lost,uint8_t p[32]){
 memcpy(p,&e->session,4);memcpy(p+4,&e->sequence,4);memcpy(p+8,&e->source_us,8);memcpy(p+16,&e->local_us,8);memcpy(p+24,&e->uncertainty_us,4);p[28]=e->edge;p[29]=e->flags;
 uint16_t count=lost>UINT16_MAX?UINT16_MAX:lost;memcpy(p+30,&count,2);
}
/* Byte29 low nibble is source quality; bits4..5 are receiver slot0..3.
 * Reserved bits6..7 are rejected. All four slots share one wire sequence. */
static inline bool event_transport_encode_slot(const service_event_t *e,uint64_t lost,unsigned slot,uint8_t p[32]){
 if(slot>=4)return false;
 event_transport_encode(e,lost,p);p[29]=(e->flags&15u)|(slot<<4);return true;
}
static inline unsigned event_transport_slot(const uint8_t p[32]){return (p[29]>>4)&3u;}
/* Return 1 new, 0 exact duplicate (ACK both), -1 invalid/old/conflicting. */
static inline int event_transport_receive(event_receiver_t *r,uint32_t boot,uint32_t seq,const uint8_t p[32]){
 uint32_t session,source;memcpy(&session,p,4);memcpy(&source,p+4,4);
 if(!boot || !seq || !session || !source || p[28]<1 || p[28]>2 || (p[29]&~63u)){++r->rejected;return -1;}
 if(r->boot==boot && r->sequence==seq){
  if(memcmp(r->payload,p,32)){++r->rejected;return -1;}
  ++r->duplicates;return 0;
 }
 if(r->boot==boot && !network_sequence_after(seq,r->sequence)){++r->rejected;return -1;}
 r->boot=boot;r->sequence=seq;memcpy(r->payload,p,32);++r->accepted;
 uint16_t lost;memcpy(&lost,p+30,2);r->lost+=lost;return 1;
}


/* Single in-flight delivery. The payload survives cache overwrite and host
 * restart. Cursor commits only on the currently bound host's matching ACK.
 * All calls belong to the network worker, never an ISR or another task. */
typedef struct {
 uint64_t cursor,position;
 int64_t retry_at;
 uint32_t host_boot,sequence,sent,retries,acked;
 bool waiting;
 uint8_t payload[32];
} event_sender_t;
static inline void event_sender_bind(event_sender_t *s,uint32_t host_boot){s->host_boot=host_boot;}
static inline bool event_sender_prepare(event_sender_t *s,uint32_t seq,uint64_t position,const uint8_t payload[32]){
 if(s->waiting || !seq || position<=s->cursor)return false;
 s->sequence=seq;s->position=position;memcpy(s->payload,payload,32);s->waiting=true;s->retry_at=0;return true;
}
static inline bool event_sender_due(event_sender_t *s,int64_t now){
 if(!s->waiting || !s->host_boot || now<0 || now<s->retry_at)return false;
 if(s->retry_at)++s->retries;
 ++s->sent;s->retry_at=now+250000;return true;
}
static inline bool event_sender_ack(event_sender_t *s,uint32_t host_boot,uint32_t own_boot,uint32_t ack_sender,uint32_t seq){
 if(!s->waiting || !host_boot || host_boot!=s->host_boot || !own_boot || ack_sender!=own_boot || seq!=s->sequence)return false;
 s->cursor=s->position;s->waiting=false;++s->acked;return true;
}
