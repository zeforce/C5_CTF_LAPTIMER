#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/receiver_events.h"
typedef struct {unsigned next,total,acks;bool drop_ack,bad,read_error;} fake_t;
static uint64_t now(void *ctx){(void)ctx;return 1100;}
static int32_t exchange(void *ctx,unsigned slot,uint8_t command,const uint8_t *in,uint8_t len,uint8_t expected,uint8_t *out){
 fake_t *f=&((fake_t*)ctx)[slot];
 if(command==6){assert(expected==18);if(f->read_error)return 263;memset(out,0,18);if(f->next>f->total)return 0;uint32_t seq=f->next;uint64_t stamp=1000+seq;memcpy(out,&seq,4);memcpy(out+4,&stamp,8);out[16]=f->bad?3:1;out[17]=1;return 0;}
 assert(command==7 && len==4);assert(receiver_u32(in)==f->next);f->acks++;if(f->drop_ack){f->drop_ack=false;return 263;}f->next++;return 0;
}
int main(void){receiver_service_t s={0};fake_t f[4]={0};unsigned drained;
 for(unsigned i=0;i<4;i++){s.slots[i].enabled=s.slots[i].online=true;s.slots[i].session=10;f[i].next=1;f[i].total=6;node_time_sample(&s.slots[i].clock,10,1000,1000,1100);}
 f[1].drop_ack=true;assert(receiver_drain_events(&s,1,exchange,now,f,4,&drained)==263 && drained==0 && s.slots[1].events.count==1);
 assert(!receiver_drain_events(&s,1,exchange,now,f,4,&drained) && drained==4 && s.slots[1].events.count==4 && s.slots[1].events.duplicates==1);
 assert(!receiver_drain_events(&s,1,exchange,now,f,4,&drained) && drained==2 && s.slots[1].events.count==6);
 assert(!receiver_drain_events(&s,2,exchange,now,f,2,&drained) && drained==2 && s.slots[2].events.count==2 && !s.slots[0].events.count);
 f[3].bad=true;assert(receiver_drain_events(&s,3,exchange,now,f,4,&drained)!=0 && !f[3].acks && !s.slots[3].events.count);
 f[3].bad=false;f[3].read_error=true;assert(receiver_drain_events(&s,3,exchange,now,f,4,&drained)==263 && !f[3].acks);
 assert(receiver_drain_events(&s,4,exchange,now,f,4,&drained)!=0);assert(receiver_drain_events(&s,0,exchange,now,f,17,&drained)!=0);
 uint8_t cursor=0;assert(receiver_event_next(&s,&cursor)==-1);
 for(unsigned i=0;i<4;i++)s.slots[i].active=true;
 for(unsigned i=0;i<12;i++)assert(receiver_event_next(&s,&cursor)==(int)(i%4));
 s.slots[0].stop_pending=true;s.slots[1].restore_pending=true;s.slots[2].online=false;
 assert(receiver_event_next(&s,&cursor)==3 && receiver_event_next(&s,&cursor)==3);
 s.slots[3].enabled=false;assert(receiver_event_next(&s,&cursor)==-1);
 assert(receiver_event_next(NULL,&cursor)==-1 && receiver_event_next(&s,NULL)==-1);
 service_event_t event;assert(event_service_get(&s.slots[1].events,0,&event) && (event.flags&2));
 /* A permanently busy queue cannot consume another slot's worker turns.
  * Each turn journals and ACKs exactly one edge. */
 memset(&s,0,sizeof(s));memset(f,0,sizeof(f));cursor=0;
 for(unsigned i=0;i<4;i++){s.slots[i].enabled=s.slots[i].online=s.slots[i].active=true;s.slots[i].session=10;f[i].next=1;f[i].total=100;}
 for(unsigned turn=0;turn<16;turn++){
  int slot=receiver_event_next(&s,&cursor);assert(slot==(int)(turn%4));
  assert(!receiver_drain_events(&s,slot,exchange,now,f,1,&drained) && drained==1);
 }
 for(unsigned i=0;i<4;i++)assert(f[i].acks==4 && s.slots[i].events.count==4);
 puts("PASS bounded slot drain, journal-before-ACK, dropped ACK dedup, malformed/read-error no ACK, round-robin and unavailable-slot exclusion");}
