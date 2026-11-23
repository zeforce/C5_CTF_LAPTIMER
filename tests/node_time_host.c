#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/node_time.h"
int main(void){node_time_t m={0};uint64_t local;uint32_t bound;
 assert(!node_time_map(&m,1,100,100,&local,&bound));
 assert(node_time_sample(&m,1,10000000,20000000,20001000));assert(m.offset_us==10000500);
 assert(node_time_map(&m,1,11000000,21001000,&local,&bound));assert(local==21000500 && bound==1501);
 assert(!node_time_map(&m,2,11000000,21001000,&local,&bound));
 assert(!node_time_map(&m,1,11000000,24001001,&local,&bound));
 assert(!node_time_sample(&m,1,11000000,21000000,21030000));assert(m.rejected==1);
 assert(!node_time_sample(&m,1,9999999,21000000,21001000));
 assert(node_time_sample(&m,2,100,1000,1200));assert(m.samples==1 && m.offset_us==1000);
 assert(node_time_sample(&m,2,200,1300,1500));assert(m.offset_us==1200);
 assert(node_time_sample(&m,3,10000,1000,1200));assert(m.offset_us==-8900);
 assert(!node_time_map(&m,3,1,1200,&local,&bound));
 assert(node_time_map(&m,3,10000,1200,&local,&bound) && local==1100);
 assert(!node_time_map(&m,3,4000001,1200,&local,&bound));
 puts("PASS midpoint offset, RTT/age/distance budgets, session reset, monotonic sample and signed mapping bounds");}
