#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/start_schedule.h"
int main(void){
 start_schedule_t s={0};
 assert(!start_schedule_prepare(&s,0,5000000,0));
 assert(!start_schedule_prepare(&s,1,1999999,0));
 assert(start_schedule_prepare(&s,1,5000000,0));
 assert(start_schedule_prepare(&s,1,5000000,1));
 assert(!start_schedule_prepare(&s,2,5000000,0));
 assert(!start_schedule_arm(&s,2,0));
 assert(!start_schedule_arm(&s,1,4000001));
 assert(start_schedule_arm(&s,1,0));
 assert(!start_schedule_take(&s,4999999,true));
 assert(start_schedule_take(&s,5000000,true)==1);
 assert(!start_schedule_take(&s,5000000,true));
 assert(start_schedule_prepare(&s,2,5000000,0));
 assert(!start_schedule_take(&s,5000000,true)); /* no commit */
 assert(start_schedule_prepare(&s,3,5000000,0));
 assert(start_schedule_arm(&s,3,0));
 assert(!start_schedule_take(&s,5000001,false));
 assert(start_schedule_prepare(&s,4,5000000,0));
 assert(start_schedule_arm(&s,4,0));
 assert(!start_schedule_cancel(&s,5));
 assert(start_schedule_cancel(&s,4));
 assert(!start_schedule_take(&s,5000000,true));
 assert(start_schedule_prepare(&s,6,5000000,0));
 assert(start_schedule_arm(&s,6,0));
 assert(!start_schedule_take(&s,5050001,true));
 assert(!start_schedule_prepare_action(&s,7,5000000,0,0));
 assert(start_schedule_prepare_action(&s,7,5000000,0,2));
 assert(s.action==2);
 assert(!start_schedule_prepare_action(&s,7,5000000,0,1));
 assert(start_schedule_prepare_action(&s,7,5000000,0,2));
 assert(start_schedule_arm(&s,7,0));
 assert(s.action==2 && start_schedule_take(&s,5000000,true)==7);
 assert(!s.action && !s.phase);
 /* Repeated abort heartbeats cancel only their exact prepared/armed session. */
 assert(start_schedule_prepare(&s,8,5000000,0));assert(start_schedule_arm(&s,8,0));
 assert(!start_schedule_cancel(&s,7));assert(s.phase==START_ARMED);
 assert(start_schedule_cancel(&s,8));assert(start_schedule_cancel(&s,8));
 assert(!start_schedule_take(&s,5000000,true));
 assert(start_schedule_prepare(&s,9,5000000,0));assert(start_schedule_arm(&s,9,0));
 assert(!start_schedule_cancel(&s,8));assert(start_schedule_take(&s,5000000,true)==9);
 puts("PASS scheduled start phases, barriers, expiry, cancellation, unhealthy abort, exactly once");
}
