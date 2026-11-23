#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/receiver_service.h"
int main(void){
 receiver_service_t s={0};for(unsigned i=0;i<4;i++)s.slots[i].enabled=true;
 receiver_config_t c={36,28,20,10,30};assert(receiver_desire(&s,0,&c));c.channel=64;assert(!receiver_desire(&s,1,&c));c.channel=37;assert(receiver_desire(&s,1,&c));
 for(unsigned i=0;i<4;i++){
  assert(receiver_health(&s,i,0,10+i,true,true,true,1000));assert(node_time_sample(&s.slots[i].clock,10+i,1000,1000,1100));
  service_event_t e={.session=10+i,.sequence=1,.source_us=1000,.edge=1,.flags=1};assert(event_service_push(&s.slots[i].events,&e,&s.slots[i].clock,1200));
 }
 receiver_slot_t preserved[3];memcpy(preserved,s.slots+1,sizeof(preserved));
 assert(receiver_health(&s,0,263,0,false,false,false,2000));assert(!s.slots[0].online && !s.slots[0].clock.valid && s.slots[0].events.count==1 && s.slots[0].desired.channel==36);assert(!memcmp(preserved,s.slots+1,sizeof(preserved)));
 assert(receiver_health(&s,0,0,20,true,false,false,3000));assert(s.slots[0].restarts==1 && !s.slots[0].clock.valid && s.slots[0].events.count==1);
 service_event_t event;event_delivery_t d;for(unsigned i=0;i<4;i++){assert(event_service_peek(&s.slots[i].events,0,&event,&d));assert(event.session==10+i && d.position==1);}
 uint8_t b[32];assert(receiver_page(&s,0,4000,b));assert(b[0]==24 && b[2] && b[4]==20 && b[12]==1 && b[20]==36 && b[28]==1);
 assert(receiver_config_page(&s,0,b) && b[0]==25 && b[2] && b[24]);s.slots[0].observed=s.slots[0].desired;s.slots[0].have_observed=true;receiver_config_page(&s,0,b);assert(!b[24]);s.slots[0].online=false;receiver_config_page(&s,0,b);assert(b[24]);
 s.slots[2].capture_requested=true;s.slots[2].stop_pending=true;s.slots[2].capture_error=263;receiver_config_page(&s,2,b);assert((b[25]&96)==96 && b[26]==7 && b[27]==1);
 assert(!receiver_health(&s,4,0,1,true,true,true,1));s.slots[3].enabled=false;assert(!receiver_health(&s,3,0,1,true,true,true,1));
 receiver_slot_t *r=&s.slots[0];r->capture_requested=true;r->active=true;
 receiver_stop_result(r,263);assert(r->stop_pending && !r->capture_requested && receiver_capture_busy(r));
 receiver_health(&s,0,263,0,false,false,false,5000);assert(r->stop_pending && receiver_capture_busy(r));
 receiver_health(&s,0,0,21,true,false,false,6000);assert(r->stop_pending);
 receiver_stop_result(r,263);assert(r->stop_pending);
 receiver_stop_result(r,0);assert(!r->stop_pending && !r->active && !receiver_capture_busy(r));
 r->restore_pending=true;r->config_error=263;receiver_config_page(&s,0,b);assert(b[30]==1 && b[20]==7 && b[21]==1);
 receiver_health(&s,0,263,0,false,false,false,7000);receiver_config_page(&s,0,b);assert(b[30]==1);
 for(unsigned i=0;i<4;i++){s.slots[i].enabled=true;s.slots[i].active=s.slots[i].calibrated=s.slots[i].capture_requested=true;}
 receiver_request_stop(&s,5,6);
 assert(s.slots[0].stop_pending && s.slots[2].stop_pending && !s.slots[0].capture_requested && !s.slots[2].calibrated);
 assert(s.slots[1].capture_requested && s.slots[3].calibrated);
 uint8_t cursor=0;assert(receiver_pending_stop(&s,&cursor,0)==0);assert(receiver_pending_stop(&s,&cursor,0)==2);
 assert(receiver_pending_stop(&s,&cursor,1)==2);
 receiver_stop_result(&s.slots[2],263);assert(receiver_pending_stop(&s,&cursor,1)==2);
 receiver_stop_result(&s.slots[2],0);assert(receiver_pending_stop(&s,&cursor,1)==-1);
 assert(s.slots[0].stop_pending && receiver_capture_busy(&s.slots[0]));
 assert(receiver_pending_stop(NULL,&cursor,0)==-1 && receiver_pending_stop(&s,NULL,0)==-1);
 receiver_stop_result(&s.slots[0],0);s.slots[0].restore_pending=false;s.slots[0].online=s.slots[0].rf_valid=s.slots[0].calibrated=true;
 receiver_request_pause(&s,1);assert(s.slots[0].stop_pending && s.slots[0].stop_keep_calibrated);
 receiver_request_stop(&s,1,3);assert(!s.slots[0].stop_keep_calibrated);
 receiver_request_pause(&s,1);assert(!s.slots[0].stop_keep_calibrated);
 receiver_stop_result(&s.slots[0],0);s.slots[0].calibrated=true;receiver_request_pause(&s,1);
 receiver_stop_result(&s.slots[0],263);assert(s.slots[0].stop_pending && !s.slots[0].stop_keep_calibrated && !s.slots[0].calibrated);
 puts("PASS four-slot config/events, fault invalidation, deferred stop mask/fairness/exclusion and failure retention");
}
