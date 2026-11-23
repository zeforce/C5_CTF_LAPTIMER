#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/event_host_time.h"
int main(void){
 uint8_t p[32]={0};uint64_t node=1000000;uint32_t error=10000;memcpy(p+16,&node,8);memcpy(p+24,&error,4);p[29]=3;
 event_host_clock_t c={.member_boot=1,.host_boot=2,.uncertainty_us=500,.sampled_us=1100000,.offset_us=2000000,.valid=true};
 event_host_time_t r=event_host_map(p,&c,1,2,3200000,false);assert(!r.reason && r.host_us==3000000 && r.uncertainty_us==10600);
 c.offset_us=-200000; r=event_host_map(p,&c,1,2,1000000,false);assert(!r.reason && r.host_us==800000);
 assert(event_host_map(p,&c,3,2,1000000,false).reason==2);
 assert(event_host_map(p,&c,1,3,1000000,false).reason==2);
 assert(event_host_map(p,&c,1,2,12000000,false).reason==3);
 c.uncertainty_us=20001;assert(event_host_map(p,&c,1,2,1000000,false).reason==4);c.uncertainty_us=500;
 p[29]=1;assert(event_host_map(p,&c,1,2,1000000,false).reason==1);p[29]=11;assert(event_host_map(p,&c,1,2,1000000,false).reason==1);p[29]=3;
 assert(!event_host_map(p,0,1,1,1000000,true).reason);
 c.offset_us=INT64_MIN;assert(event_host_map(p,&c,1,2,1000000,false).reason==4);
 uint64_t out;assert(!event_host_add(UINT64_MAX,1,&out));assert(!event_host_add(1,INT64_MIN,&out));
 error=UINT32_MAX;memcpy(p+24,&error,4);c.offset_us=0;assert(event_host_map(p,&c,1,2,1200000,false).reason==4);
 puts("PASS host clock offset signs, boot isolation, age, quality, source validity and arithmetic bounds");
}
