#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_sequence.h"
#include "../firmware/c3_controller/main/result_settle.h"
int main(void){
 uint8_t command[20]={1,3};uint32_t receiver=71;
 memcpy(command+2,&receiver,4);
 assert(network_command_target(command,6,71));
 assert(!network_command_target(command,6,72)); /* command recorded before member reset */
 assert(!network_command_target(command,6,0));
 assert(!network_command_target(command,2,71)); /* old unbound wire format */
 assert(!network_command_target(NULL,6,71));
 assert(!network_command_target(command,21,71));
 memcpy(command+16,&receiver,4);
 assert(network_command_target(command,20,71));
 assert(network_command_current(11,11,100,101));
 assert(!network_command_current(11,0,0,101));
 assert(!network_command_current(10,11,100,101));
 assert(!network_command_current(12,11,100,101));
 assert(!network_command_current(0,0,100,101));
 assert(!network_command_current(11,11,0,101));
 assert(!network_command_current(11,11,100,99));
 assert(network_command_current(11,11,100,5000099));
 assert(!network_command_current(11,11,100,5000100));
 assert(!network_command_current(11,11,100,UINT64_MAX));
 assert(network_sequence_after(1,0));assert(!network_sequence_after(0,0));
 assert(!network_sequence_after(7,7));assert(!network_sequence_after(6,7));
 assert(network_sequence_after(8,7));assert(network_sequence_after(1,UINT32_MAX));
 assert(!network_sequence_after(UINT32_MAX,1));assert(!network_sequence_after(0x80000001,1));
 assert(!network_sequence_after(0,UINT32_MAX));
 assert(result_terminal_score(UINT32_MAX,1));assert(!result_terminal_score(1,UINT32_MAX));
 assert(network_report_fresh(11,10,true));
 assert(!network_report_fresh(11,10,false));
 assert(!network_report_fresh(10,10,true));
 assert(!network_report_fresh(9,10,true));
 assert(network_report_fresh(1,UINT32_MAX,true));
 assert(network_reply_current(11,11,11,22,22,33,33));
 assert(!network_reply_current(11,12,11,22,22,33,33)); /* cached pre-reboot reply */
 assert(!network_reply_current(11,12,12,22,22,33,33)); /* new boot is another transaction */
 assert(!network_reply_current(11,11,12,22,22,33,33));
 assert(!network_reply_current(11,11,11,22,23,33,33));
 assert(!network_reply_current(11,11,11,22,22,33,34));
 assert(!network_reply_current(0,0,0,22,22,33,33));
 assert(!network_reply_current(11,11,11,0,0,33,33));
 assert(!network_reply_current(11,11,11,22,22,0,0));
 assert(network_reply_current(UINT32_MAX,UINT32_MAX,UINT32_MAX,22,22,UINT32_MAX,UINT32_MAX));
 puts("PASS sequence wrap, duplicate, stale, half-range, reserved zero, final score");
}
