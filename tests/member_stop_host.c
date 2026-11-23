#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/member_stop.h"
int main(void){member_stop_t s={0};assert(!member_stop_due(&s,0));member_stop_request(&s);assert(member_stop_due(&s,0));
 member_stop_complete(&s,false,true);assert(s.capture_pending && !s.game_pending);assert(!member_stop_due(&s,249999));assert(member_stop_due(&s,250000));
 /* Reconnection does not touch the pending state. */
 member_stop_complete(&s,false,true);assert(member_stop_pending(&s));assert(member_stop_due(&s,500000));member_stop_complete(&s,true,false);assert(!member_stop_pending(&s));
 member_stop_request(&s);assert(member_stop_due(&s,500001));member_stop_complete(&s,true,false);assert(!s.capture_pending && s.game_pending);
 assert(member_stop_due(&s,750001));member_stop_complete(&s,false,true);assert(!member_stop_pending(&s));
 assert(s.requests==2 && s.attempts==5);assert(!member_stop_due(&s,1000000));
 puts("PASS independent stop/pause retries, failed SPI retains latch, reconnect cannot cancel pending stop, 250ms backoff");}
