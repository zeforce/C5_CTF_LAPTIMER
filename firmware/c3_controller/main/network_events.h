#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "event_host_time.h"
#define NETWORK_EVENTS_CAPACITY 32
#define NETWORK_EVENTS_FIRST_PAGE 256
#define NETWORK_EVENTS_LAST_PAGE 352
typedef struct {uint8_t mac[6],payload[32];uint32_t boot,sequence;uint64_t received_us,position;event_host_time_t time;} network_event_t;
typedef struct {network_event_t ring[32];uint8_t count,next;uint32_t accepted,overwritten;uint64_t published;} network_events_t;
typedef struct {network_events_t events;uint32_t revision;uint64_t captured_us;} network_events_snapshot_t;
/* Caller serializes producer, snapshot and page reads. Arrival order only:
 * source timestamps remain in each node's own clock domain. */
static inline void network_events_push(network_events_t *j,const uint8_t mac[6],uint32_t boot,uint32_t seq,const uint8_t payload[32],uint64_t now){
 network_event_t *e=&j->ring[j->next];memcpy(e->mac,mac,6);memcpy(e->payload,payload,32);memset(&e->time,0,sizeof(e->time));e->time.reason=2;e->boot=boot;e->sequence=seq;e->received_us=now;e->position=++j->published;
 j->next=(j->next+1)%32;if(j->count<32)++j->count;else ++j->overwritten;++j->accepted;
}
static inline void network_events_capture(network_events_snapshot_t *s,const network_events_t *j,uint64_t now){
 s->events=*j;if(!++s->revision)++s->revision;s->captured_us=now;
}
static inline bool network_events_page(const network_events_snapshot_t *s,unsigned page,uint64_t now,uint8_t out[32]){
 if(page<256 || page>352)return false;
 memset(out,0,32);
 if(page==256){out[0]=20;out[1]=s->events.count;out[2]=32;out[3]=s->revision!=0;memcpy(out+4,&s->revision,4);memcpy(out+8,&s->events.overwritten,4);memcpy(out+12,&s->events.accepted,4);memcpy(out+16,&s->captured_us,8);uint32_t age=s->revision?(now-s->captured_us)/1000:UINT32_MAX;memcpy(out+24,&age,4);return true;}
 if(page>=321){unsigned index=page-321;out[0]=23;out[1]=index;if(index>=s->events.count)return true;
 const network_event_t *e=&s->events.ring[(s->events.next+31-index)%32];out[2]=e->time.reason==0;out[3]=e->time.reason;memcpy(out+4,&e->time.host_boot,4);memcpy(out+8,&e->time.host_us,8);memcpy(out+16,&e->time.uncertainty_us,4);memcpy(out+20,&s->revision,4);memcpy(out+24,&e->position,8);return true;}
 unsigned index=(page-257)/2;if(index>=s->events.count)return true;
 const network_event_t *e=&s->events.ring[(s->events.next+31-index)%32];
 if((page-257)%2){memcpy(out,e->payload,32);return true;}
 out[0]=21;out[1]=index;memcpy(out+2,e->mac,6);memcpy(out+8,&e->boot,4);memcpy(out+12,&e->sequence,4);memcpy(out+16,&e->received_us,8);memcpy(out+24,&e->position,8);return true;
}
