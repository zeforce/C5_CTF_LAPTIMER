#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/receiver_restore.h"
typedef struct {unsigned calls,fail_at;uint8_t order[3];receiver_config_t hardware;bool mismatch;} fake_t;
static int32_t exchange(void *ctx,unsigned slot,uint8_t cmd,const uint8_t *in,uint8_t len,uint8_t expected,uint8_t *out){
 fake_t *f=ctx;assert(slot==2);assert(f->calls<3);f->order[f->calls++]=cmd;
 if(f->calls==f->fail_at)return 263;
 if(cmd==8){assert(len==2 && !in[0]);}
 else if(cmd==4){assert(len==8);if(!f->mismatch)memcpy(&f->hardware,in,8);}
 else {assert(cmd==3 && expected==8);memcpy(out,&f->hardware,8);}return 0;
}
int main(void){
 receiver_config_t target={36,28,20,10,30};
 for(unsigned fault=1;fault<=4;fault++){
  receiver_service_t s={0};s.slots[2].enabled=true;s.slots[2].active=s.slots[2].capture_requested=true;s.slots[2].calibrated=true;
  fake_t f={.fail_at=fault<=3?fault:0,.mismatch=fault==4};
  receiver_slot_t untouched=s.slots[1];assert(receiver_restore(&s,2,&target,exchange,&f,264)!=0);
  assert(s.slots[2].restore_pending && receiver_capture_busy(&s.slots[2]) && !s.slots[2].capture_requested && !s.slots[2].have_observed);
  assert(s.slots[2].stop_pending==(fault==1));assert(!memcmp(&untouched,&s.slots[1],sizeof(untouched)));
  uint8_t page[32];receiver_config_page(&s,2,page);assert(page[30] && page[20]);
  assert(receiver_capture_action(&s,2,2,exchange,&f)!=0);
  assert(receiver_capture_action(&s,2,1,exchange,&f)!=0);
  /* A user stop can succeed without completing configuration restoration. */
  f=(fake_t){0};assert(!receiver_capture_action(&s,2,0,exchange,&f));assert(s.slots[2].restore_pending && receiver_capture_busy(&s.slots[2]));
  receiver_health(&s,2,263,0,false,false,false,1);assert(s.slots[2].restore_pending);
  f=(fake_t){0};assert(!receiver_restore(&s,2,&target,exchange,&f,264));
  assert(!s.slots[2].restore_pending && !s.slots[2].stop_pending && !s.slots[2].calibrated && !receiver_capture_busy(&s.slots[2]));
  assert(s.slots[2].have_observed && !memcmp(&s.slots[2].observed,&target,8));assert(f.calls==3 && f.order[0]==8 && f.order[1]==4 && f.order[2]==3);
 }
 for(unsigned fault=0;fault<=4;fault++){
  receiver_service_t s={0};s.slots[2].enabled=true;s.slots[2].active=true;
  receiver_restore_step_t p={0};fake_t f={.fail_at=fault<=3?fault:0,.mismatch=fault==4};bool done=false;
  int32_t e=0;
  for(unsigned step=0;step<3;step++){
   unsigned before=f.calls;e=receiver_restore_step(&s,2,&target,&p,exchange,&f,264,&done);
   assert(f.calls==before+1);
   if(e)break;
   assert(done==(step==2));
   assert(s.slots[2].restore_pending==!done);
  }
  if(fault){assert(e && !done && s.slots[2].restore_pending && p.phase==0);}
  else {assert(done && !s.slots[2].restore_pending);continue;}
  f=(fake_t){0};
  for(unsigned step=0;step<3;step++)assert(!receiver_restore_step(&s,2,&target,&p,exchange,&f,264,&done));
  assert(done && !s.slots[2].restore_pending);
 }
 /* A new target mid-restore must restart with STOP and publish only that one. */
 receiver_service_t s={0};s.slots[2].enabled=true;receiver_restore_step_t p={0};fake_t f={0};bool done;
 assert(!receiver_restore_step(&s,2,&target,&p,exchange,&f,264,&done));
 receiver_config_t changed=target;changed.gain=30;f=(fake_t){0};
 assert(!receiver_restore_step(&s,2,&changed,&p,exchange,&f,264,&done) && f.order[0]==8 && !done);
 assert(!receiver_restore_step(&s,2,&changed,&p,exchange,&f,264,&done));
 assert(!receiver_restore_step(&s,2,&changed,&p,exchange,&f,264,&done) && done && s.slots[2].observed.gain==30);
 puts("PASS synchronous and single-request restore: all failures/mismatch, retry, target replacement, slot isolation and readback-only commit");
}
