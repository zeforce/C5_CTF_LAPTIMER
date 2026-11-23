#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/command_policy.h"
int main(void){
 const unsigned allowed[]={1,2,3,8,9,10,12,20,21,22,23,24,28,29,30,31,38,43,47,50,52};
 const unsigned forward[]={3,8,9,10,12,31,52};
 for(unsigned opcode=0;opcode<256;opcode++){
  bool expected=false,broadcast=false;
  for(unsigned i=0;i<sizeof(allowed)/sizeof(*allowed);i++)expected|=opcode==allowed[i];
  for(unsigned i=0;i<sizeof(forward)/sizeof(*forward);i++)broadcast|=opcode==forward[i];
  assert(command_member_allowed(opcode)==expected);assert(command_match_forward(opcode)==broadcast);
  assert(!command_match_forward(opcode) || command_member_allowed(opcode));
  uint8_t frame[17]={1,opcode};
  for(unsigned length=0;length<=17;length++)assert(command_member_frame(frame,length)==(expected && length>=2 && length<=16));
  frame[0]=2;assert(!command_member_frame(frame,16));
 }
 assert(!command_member_frame(NULL,15));
 for(unsigned op=0;op<256;op++)for(unsigned action=0;action<256;action++){
  uint8_t frame[4]={1,op,action,0};
  bool halt=(op==10 && (action==0 || action==3)) || (op==3 && action==0);
  assert(command_is_halt(frame,3)==halt);
  assert(!command_is_halt(frame,2));assert(!command_is_halt(frame,4));
  frame[0]=2;assert(!command_is_halt(frame,3));
 }
 assert(!command_is_halt(NULL,3));
 puts("PASS all 256 opcodes, implicit broadcast subset, independent receiver controls, frame bounds and version");
}
