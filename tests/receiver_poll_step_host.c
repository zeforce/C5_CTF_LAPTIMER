#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/receiver_poll_step.h"
typedef struct {receiver_config_t config;uint32_t session,overflow;unsigned calls,stops,writes,fail;bool active,calibrated,mismatch;} fake_t;
static int32_t exchange(void *ctx,unsigned slot,uint8_t command,const uint8_t *in,uint8_t len,uint8_t expected,uint8_t *out){
 fake_t *f=ctx;(void)slot;(void)len;f->calls++;if(f->fail==command)return 263;
 if(out)memset(out,0,expected);
 switch(command){
 case 1:memcpy(out+6,&f->session,4);out[10]=f->config.channel;out[11]=1;break;
 case 3:memcpy(out,&f->config,8);break;
 case 4:++f->writes;if(!f->mismatch)memcpy(&f->config,in,8);break;
 case 5:out[13]=f->active;out[14]=f->calibrated;memcpy(out+8,&f->overflow,4);break;
 case 8:++f->stops;f->active=in[0];f->calibrated=in[1];break;
 default:assert(0);
 }return 0;
}
static int32_t step(receiver_service_t *s,receiver_poll_step_t *p,fake_t *f,bool *done){
 unsigned before=f->calls;int32_t e=receiver_poll_step(s,1,p,exchange,f,1000,done);assert(f->calls==before+1);return e;
}
static int32_t sweep(receiver_service_t *s,receiver_poll_step_t *p,fake_t *f){
 bool done=false;unsigned count=0;int32_t e;
 do{e=step(s,p,f,&done);assert(++count<=10);}while(!done);return e;
}
int main(void){
 receiver_service_t s={0};s.slots[1].enabled=true;receiver_slot_t *r=&s.slots[1];
 receiver_config_t want={36,28,20,10,30};receiver_desire(&s,1,&want);
 receiver_poll_step_t p={0};fake_t f={.session=10,.active=true,.config={37,1,3,1,10}};
 assert(!sweep(&s,&p,&f));assert(r->online && !r->active && !r->stop_pending && f.stops==1 && f.writes==1 && !memcmp(&r->observed,&want,8));
 assert(!sweep(&s,&p,&f) && f.stops==1 && f.writes==1);
 r->capture_requested=r->active=true;f.active=true;f.calibrated=true;
 assert(!sweep(&s,&p,&f) && r->active && r->capture_requested);
 f.overflow++;assert(!sweep(&s,&p,&f) && !r->active && !r->capture_requested && r->capture_error);
 /* Every failing command invalidates live health; uncertain stop is latched. */
 const unsigned fail_phases[]={RP_INFO,RP_STOP,RP_OBS,RP_STOP_OBS,RP_CONFIG,RP_STOP_WRITE,RP_WRITE,RP_VERIFY,RP_FINAL};
 const unsigned commands[]={1,8,5,8,3,8,4,3,1};
 for(unsigned i=0;i<9;i++){
  p=(receiver_poll_step_t){.phase=fail_phases[i],.session=10,.was_active=true,.have_target=true,.target=want};r->active=true;r->capture_requested=true;f.fail=commands[i];bool done;
  assert(step(&s,&p,&f,&done)==263 && done && p.phase==RP_INFO && !r->online && r->stop_pending && !r->capture_requested);
  f.fail=0;assert(!sweep(&s,&p,&f) && r->online && !r->stop_pending && !f.active);
 }
 f.session++;bool reboot_done=false;
 assert(!step(&s,&p,&f,&reboot_done) && !reboot_done);
 assert(!r->online && !r->rf_valid && !r->active && r->stop_pending);
 assert(!sweep(&s,&p,&f) && r->restarts==1);
 f.config.gain++;f.mismatch=true;assert(sweep(&s,&p,&f)!=0 && r->config_error && !r->online);
 f.mismatch=false;assert(!sweep(&s,&p,&f) && !r->config_error);
 /* A reboot between reads fails the final identity fence before publication. */
 bool done=false;do{assert(!step(&s,&p,&f,&done) && !done);}while(p.phase!=RP_FINAL);
 f.session++;assert(step(&s,&p,&f,&done)!=0 && done && !r->online);
 assert(!sweep(&s,&p,&f) && r->online);
 /* Saved desired changes restart an in-flight sweep without SPI mutation. */
 assert(!step(&s,&p,&f,&done));r->desired.gain=30;
 assert(!sweep(&s,&p,&f) && r->observed.gain==30);
 /* Replace the target at every phase; no old sweep may report completion. */
 for(unsigned phase=RP_STOP;phase<=RP_FINAL;phase++){
  p=(receiver_poll_step_t){.phase=phase,.session=f.session,.have_target=true,.target=r->desired};
  r->desired.gain=r->desired.gain==30?31:30;
  assert(!step(&s,&p,&f,&done) && !done);
  assert(p.phase==RP_OBS || p.phase==RP_STOP);
  assert(!sweep(&s,&p,&f) && !memcmp(&r->observed,&r->desired,8));
 }
 assert(s.slots[0].polls==0 && s.slots[2].polls==0);
 puts("PASS one exchange per step, reboot/stop/config/readback, all-phase failures/recovery, identity fence and slot isolation");
}
