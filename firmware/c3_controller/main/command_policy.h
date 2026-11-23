#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Member control is host-owned. Provisioning, roster, diagnostics and result
 * browsing never enter this route. Payload value validation stays at execution. */
static inline bool command_member_allowed(uint8_t opcode){
 switch(opcode){
 case 1:case 2:case 3:case 8:case 9:case 10:case 12:
 case 20:case 21:case 22:case 23:case 24:
 case 28:case 29:case 30:case 31:case 38:case 43:case 47:case 50:case 52:return true;
 default:return false;
 }
}
/* Only shared match rules/actions are implicitly broadcast. Receiver settings
 * use explicit targeted commands because each node has independent hardware. */
static inline bool command_match_forward(uint8_t opcode){
 switch(opcode){case 3:case 8:case 9:case 10:case 12:case 31:case 52:return true;default:return false;}
}
static inline bool command_member_frame(const uint8_t *data,size_t length){
 return data && length>=2 && length<=16 && data[0]==1 && command_member_allowed(data[1]);
}
/* Halt must still execute after cancelling a pending start/resume. */
static inline bool command_is_halt(const uint8_t *data,size_t length){
 return data && length==3 && data[0]==1 &&
  ((data[1]==10 && (data[2]==0 || data[2]==3)) || (data[1]==3 && data[2]==0));
}
