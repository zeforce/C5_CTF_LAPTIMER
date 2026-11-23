#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/game_receiver.h"
static void push(receiver_service_t *s,unsigned slot,unsigned seq,uint64_t time,unsigned edge){
 service_event_t e={.session=10,.sequence=seq,.source_us=time,.edge=edge,.flags=1};assert(event_service_push(&s->slots[slot].events,&e,NULL,time));
}
int main(void){
 game_t g;game_init(&g);receiver_service_t s={0};game_receiver_t consumer={0};
 for(unsigned p=0;p<4;p++){g.config.pilots[p].enabled=true;g.config.pilots[p].slot=p;g.config.pilots[p].channel=36+p;
  receiver_slot_t *r=&s.slots[p];r->enabled=r->online=r->rf_valid=r->calibrated=r->have_observed=true;r->observed.channel=36+p;}
 assert(game_receiver_ready(&g.config,&s));
  s.slots[0].have_desired=true;s.slots[0].desired=s.slots[0].observed;
  s.slots[0].desired.gain++;assert(!game_receiver_ready(&g.config,&s));
  s.slots[0].desired=s.slots[0].observed;assert(game_receiver_ready(&g.config,&s));
  s.slots[0].config_error=263;assert(!game_receiver_ready(&g.config,&s));
  s.slots[0].config_error=0;assert(game_receiver_ready(&g.config,&s));s.slots[2].stop_pending=true;assert(!game_receiver_ready(&g.config,&s));s.slots[2].stop_pending=false;
 s.slots[2].observed.channel=1;assert(!game_receiver_ready(&g.config,&s));s.slots[2].observed.channel=38;
 s.slots[1].calibrated=false;assert(!game_receiver_ready(&g.config,&s));s.slots[1].calibrated=true;
 assert(game_start(&g,0,game_receiver_mask(&g.config)));push(&s,0,1,1,LAP_ENTER);game_receiver_bind(&consumer,&s);
 for(unsigned slot=0;slot<4;slot++){
  unsigned seq=slot?1:2;push(&s,slot,seq,1000000,LAP_ENTER);push(&s,slot,seq+1,1100000,LAP_EXIT);push(&s,slot,seq+2,3000000+slot*100000,LAP_ENTER);
  assert(game_receiver_consume(&consumer,&g,&s,slot,2)==2);assert(!g.laps[slot].total_laps);
  assert(game_receiver_consume(&consumer,&g,&s,slot,2)==1);assert(g.laps[slot].total_laps==1 && g.laps[slot].fastest_us==2000000+slot*100000);
  assert(!game_receiver_consume(&consumer,&g,&s,slot,16));
 }
 /* Retention overrun breaks this slot's baseline without touching others. */
 for(unsigned n=0;n<40;n++)push(&s,2,4+n,4000000+n*100000,LAP_ENTER);
 assert(game_receiver_consume(&consumer,&g,&s,2,16)==16 && g.laps[2].total_laps==1 && g.laps[1].total_laps==1);
 game_pause(&g,0,9000000);push(&s,1,4,10000000,LAP_EXIT);assert(!game_receiver_consume(&consumer,&g,&s,1,16));
 game_receiver_bind(&consumer,&s);assert(game_resume(&g,11000000));assert(!game_receiver_consume(&consumer,&g,&s,1,16));
 puts("PASS four receiver lap consumers, independent source sequences, bounded delivery, history exclusion, loss and pause");
}
