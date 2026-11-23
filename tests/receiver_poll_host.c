#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/receiver_poll.h"
typedef struct {uint32_t session;receiver_config_t config;unsigned stops,writes;bool active,fail,mismatch;uint64_t now;uint32_t rtt;bool wrong_session;bool calibrated;uint32_t overflow;} fake_t;
static int32_t exchange(void *ctx,unsigned slot,uint8_t command,const uint8_t *in,uint8_t len,uint8_t expected,uint8_t *out){
 fake_t *f=&((fake_t*)ctx)[slot];if(f->fail)return 263;if(out)memset(out,0,expected);
 switch(command){
 case 11:{uint64_t node=f->now;memcpy(out,&node,8);uint32_t session=f->session+(f->wrong_session?1:0);memcpy(out+8,&session,4);f->now+=f->rtt;break;}
 case 1:assert(expected==17);memcpy(out+6,&f->session,4);out[11]=1;break;
 case 8:assert(len==2);if(!in[0])f->stops++;f->active=in[0];f->calibrated=in[1];break;
 case 5:assert(expected==16);out[13]=f->active;out[14]=f->calibrated;memcpy(out+8,&f->overflow,4);break;
 case 3:assert(expected==8);memcpy(out,&f->config,8);break;
 case 4:assert(len==8);f->writes++;if(!f->mismatch)memcpy(&f->config,in,8);break;
 default:assert(0);
 }return 0;
}
static uint64_t now(void *ctx){return ((fake_t*)ctx)[1].now;}
int main(void){
 receiver_service_t s={0};fake_t f[4]={0};receiver_config_t want={36,28,20,10,30};
 for(unsigned i=1;i<4;i++){s.slots[i].enabled=true;receiver_desire(&s,i,&want);f[i].session=10+i;}
 assert(!receiver_poll(&s,1,exchange,f,1000));assert(f[1].stops==1 && f[1].writes==1 && s.slots[1].online);
 assert(!receiver_poll(&s,1,exchange,f,2000));assert(f[1].writes==1 && f[1].stops==1);
 receiver_slot_t saved=s.slots[1];f[2].fail=true;assert(receiver_poll(&s,2,exchange,f,3000)==263);assert(!memcmp(&saved,&s.slots[1],sizeof(saved)) && s.slots[2].errors==1);
 f[2].fail=false;f[2].mismatch=true;assert(receiver_poll(&s,2,exchange,f,4000)!=0 && !s.slots[2].online && s.slots[2].config_error);
 f[2].mismatch=false;assert(!receiver_poll(&s,2,exchange,f,5000) && s.slots[2].config_error==0);
 f[1].session++;f[1].active=true;assert(!receiver_poll(&s,1,exchange,f,6000));assert(s.slots[1].restarts==1 && !s.slots[1].active && f[1].writes==2 && f[1].stops==2);
 f[1].active=true;assert(!receiver_poll(&s,1,exchange,f,7000));assert(f[1].stops==3 && !f[1].active);
 assert(!receiver_poll(&s,3,exchange,f,8000));assert(!memcmp(&s.slots[3].observed,&want,8));
 f[1].now=1000000;f[1].rtt=1000;assert(receiver_poll_clock(&s,1,exchange,now,f));assert(s.slots[1].clock.samples==1);
 assert(receiver_poll_clock(&s,1,exchange,now,f));assert(s.slots[1].clock.samples==1);
 uint8_t page[32];assert(receiver_clock_page(&s,1,f[1].now,page) && page[0]==27 && page[2]);
 f[1].now+=1100000;f[1].wrong_session=true;assert(!receiver_poll_clock(&s,1,exchange,now,f) && !s.slots[1].clock.valid && s.slots[1].online);
 f[1].wrong_session=false;f[1].rtt=30000;assert(!receiver_poll_clock(&s,1,exchange,now,f));f[1].rtt=1000;assert(receiver_poll_clock(&s,1,exchange,now,f));
 receiver_clock_page(&s,1,f[1].now+4000000,page);assert(!page[2]);assert(!receiver_clock_page(&s,4,0,page));
 assert(receiver_capture_action(&s,2,2,exchange,f)!=0);
 s.slots[2].desired.gain++;assert(receiver_capture_action(&s,2,1,exchange,f)!=0);
 s.slots[2].desired=s.slots[2].observed;s.slots[2].config_error=263;assert(receiver_capture_action(&s,2,1,exchange,f)!=0);
 s.slots[2].config_error=0;
 assert(!receiver_capture_action(&s,2,1,exchange,f));assert(s.slots[2].calibrated);
 assert(!receiver_capture_action(&s,2,2,exchange,f));assert(s.slots[2].capture_requested && f[2].active);
 assert(!receiver_poll(&s,2,exchange,f,9000) && s.slots[2].active);
 f[2].overflow++;assert(!receiver_poll(&s,2,exchange,f,10000));assert(!s.slots[2].capture_requested && !f[2].active && s.slots[2].capture_error);
 assert(!receiver_capture_action(&s,2,1,exchange,f));assert(!receiver_capture_action(&s,2,2,exchange,f));
 f[2].fail=true;assert(receiver_poll(&s,2,exchange,f,11000)==263 && s.slots[2].stop_pending && !s.slots[2].capture_requested);
 assert(receiver_capture_action(&s,2,0,exchange,f)==263 && s.slots[2].stop_pending);
 f[2].fail=false;assert(!receiver_poll(&s,2,exchange,f,12000) && !s.slots[2].stop_pending && !f[2].active);
 assert(!receiver_capture_action(&s,2,1,exchange,f));assert(!receiver_capture_action(&s,2,2,exchange,f));f[2].session++;
 assert(!receiver_poll(&s,2,exchange,f,13000) && !s.slots[2].capture_requested && !f[2].active);
 /* Health invalidation must not erase an observed-active stop obligation. */
 s.slots[2].active=true;s.slots[2].capture_requested=false;f[2].active=true;f[2].fail=true;
 assert(receiver_capture_busy(&s.slots[2]));
 assert(receiver_poll(&s,2,exchange,f,14000)==263);
 assert(!s.slots[2].active && s.slots[2].stop_pending && receiver_capture_busy(&s.slots[2]));
 assert(receiver_capture_action(&s,2,2,exchange,f)!=0);
 f[2].fail=false;assert(!receiver_poll(&s,2,exchange,f,15000));
 assert(!f[2].active && !receiver_capture_busy(&s.slots[2]));
 /* A second slot remains available while this slot waits for stop recovery. */
 s.slots[2].stop_pending=true;assert(receiver_capture_busy(&s.slots[2]));
 assert(!receiver_capture_busy(&s.slots[3]));
 assert(!receiver_capture_action(&s,3,1,exchange,f));assert(!receiver_capture_action(&s,3,2,exchange,f));
 assert(receiver_capture_busy(&s.slots[3]));assert(!receiver_capture_action(&s,3,0,exchange,f));
 assert(receiver_capture_action(&s,4,1,exchange,f)!=0);
 assert(receiver_poll(&s,0,exchange,f,9000)!=0);puts("PASS independent slot probes, desired apply/readback mismatch retry, recovery/reboot reapply and unexpected-active stop");
}
