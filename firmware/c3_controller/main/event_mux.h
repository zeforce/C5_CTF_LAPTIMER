#pragma once
#include "event_transport.h"
/* Caller serializes all methods. One in-flight sender, independent slot
 * cursors. A blocked ACK blocks all slots; not independent reliable streams. */
typedef struct {uint64_t cursor[4];unsigned next,pending_slot;} event_mux_t;
static inline unsigned event_mux_next(event_mux_t *m){unsigned slot=m->next;m->next=(slot+1)%4;return slot;}
static inline bool event_mux_prepare(event_mux_t *m,event_sender_t *s,unsigned slot,uint32_t seq,const service_event_t *e,const event_delivery_t *d,uint8_t payload[32]){
 if(slot>=4 || s->waiting || d->position<=m->cursor[slot] || !seq)return false;
 event_transport_encode_slot(e,d->lost,slot,payload);s->cursor=m->cursor[slot];
 if(!event_sender_prepare(s,seq,d->position,payload))return false;
 m->pending_slot=slot;return true;
}
static inline bool event_mux_ack(event_mux_t *m,event_sender_t *s,uint32_t host,uint32_t own,uint32_t sender,uint32_t seq){
 if(!event_sender_ack(s,host,own,sender,seq))return false;
 m->cursor[m->pending_slot]=s->cursor;return true;
}
