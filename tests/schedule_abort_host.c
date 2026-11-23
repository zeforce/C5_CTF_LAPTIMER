#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/schedule_abort.h"
#include "../firmware/c3_controller/main/start_schedule.h"
int main(void){
 assert(schedule_abort_matches(42,100,42,90));
 assert(schedule_abort_matches(42,100,42,100));
 assert(!schedule_abort_matches(42,100,42,101)); /* explicit new resume */
 assert(!schedule_abort_matches(42,100,43,90));
 assert(!schedule_abort_matches(0,100,42,90));
 assert(!schedule_abort_matches(42,0,42,90));
 assert(!schedule_abort_matches(42,100,42,0));
 assert(schedule_abort_matches(42,1,42,UINT32_MAX));
 assert(!schedule_abort_matches(42,UINT32_MAX,42,1));
 for(unsigned i=0;i<100;i++)assert(!schedule_abort_matches(42,100,42,101));
 start_schedule_t pending={0};
 assert(start_schedule_prepare_action(&pending,42,5000000,0,2));
 assert(start_schedule_arm(&pending,42,0));
 if(schedule_abort_matches(42,100,pending.session,90))assert(start_schedule_cancel(&pending,42));
 assert(!start_schedule_take(&pending,5000000,true));
 assert(start_schedule_prepare_action(&pending,42,10000000,5000000,2));
 assert(start_schedule_arm(&pending,42,5000000));
 for(unsigned i=0;i<10;i++)if(schedule_abort_matches(42,100,pending.session,101))start_schedule_cancel(&pending,42);
 assert(start_schedule_take(&pending,10000000,true)==42);
 puts("PASS attempt fence, repeated abort vs resume, session isolation, boot reset sentinels and wrap");
}
