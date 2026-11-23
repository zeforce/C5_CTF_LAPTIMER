#include "event_service.h"
#include <string.h>
bool event_service_push(event_service_t *s,const service_event_t *e,const node_time_t *map,uint64_t now){
 if(!s || !e || !e->session || !e->sequence)return false;
 bool reset=s->session!=e->session;
 if(!reset && s->last_sequence){uint32_t d=e->sequence-s->last_sequence;
  if(!d){++s->duplicates;return false;}if(d>=UINT32_C(0x80000000)){++s->stale;return false;}}
 service_event_t value=*e;value.flags&=1;value.local_us=0;value.uncertainty_us=UINT32_MAX;
 if(!reset && s->last_sequence){uint32_t expected=s->last_sequence+1;if(!expected)expected=1;
  if(e->sequence!=expected){value.flags|=4;++s->gaps;}
  if(e->source_us<=s->last_time){value.flags|=8;++s->regressions;}}
 if(node_time_map(map,e->session,e->source_us,now,&value.local_us,&value.uncertainty_us))value.flags|=2;
 s->session=e->session;s->last_sequence=e->sequence;
 if(reset || e->source_us>s->last_time)s->last_time=e->source_us;
 s->ring[s->next]=value;s->next=(s->next+1)%EVENT_SERVICE_CAPACITY;
 if(s->count<EVENT_SERVICE_CAPACITY)++s->count;else ++s->overwritten;
 ++s->accepted;++s->published;return true;
}
bool event_service_get(const event_service_t *s,unsigned newest,service_event_t *e){
 if(!s || !e || newest>=s->count)return false;
 *e=s->ring[(s->next+EVENT_SERVICE_CAPACITY-1-newest)%EVENT_SERVICE_CAPACITY];return true;
}
void event_service_page(const event_service_t *s,unsigned page,uint8_t out[32]){
 memset(out,0,32);out[0]=1;out[1]=page;
 if(page==38){out[2]=s->count;out[3]=EVENT_SERVICE_CAPACITY;
  uint32_t data[7]={s->session,s->accepted,s->duplicates,s->stale,s->gaps,s->regressions,s->overwritten};memcpy(out+4,data,28);
 }else if(page>=39 && page<71){service_event_t e;if(event_service_get(s,page-39,&e)){
  out[2]=e.edge;out[3]=e.flags;memcpy(out+4,&e.session,4);memcpy(out+8,&e.sequence,4);
  memcpy(out+12,&e.source_us,8);memcpy(out+20,&e.local_us,8);memcpy(out+28,&e.uncertainty_us,4);
 }}
}

void event_snapshot_capture(event_snapshot_t *s,const event_service_t *events,uint64_t now){
 s->events=*events;if(!++s->revision)++s->revision;s->captured_us=now;
}
bool event_snapshot_page(const event_snapshot_t *s,unsigned page,uint64_t now,uint8_t out[32]){
 if(!s || !out || page<71 || page>104)return false;
 memset(out,0,32);out[0]=1;out[1]=page;
 if(page==71){out[2]=s->revision!=0;out[3]=s->events.count;memcpy(out+4,&s->revision,4);
  uint32_t age=s->revision && now>=s->captured_us?(now-s->captured_us)/1000:UINT32_MAX;
  memcpy(out+8,&age,4);memcpy(out+12,&s->events.accepted,4);memcpy(out+16,&s->events.overwritten,4);
  memcpy(out+20,&s->events.session,4);memcpy(out+24,&s->captured_us,8);
 }else if(s->revision){event_service_page(&s->events,page==72?38:page-34,out);out[1]=page;}
 return true;
}

bool event_service_peek(const event_service_t *s,uint64_t cursor,service_event_t *e,event_delivery_t *d){
 if(!s || !e || !d)return false;
 memset(d,0,sizeof(*d));
 if(!s->count || cursor==s->published)return false;
 uint64_t oldest=s->published-s->count+1;
 d->reset=cursor>s->published;
 uint64_t next=d->reset?oldest:cursor+1;
 if(next<oldest){d->lost=oldest-next;next=oldest;}
 if(!event_service_get(s,(unsigned)(s->published-next),e))return false;
 d->position=next;return true;
}
