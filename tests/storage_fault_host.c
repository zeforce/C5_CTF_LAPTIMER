#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/storage_fault_gate.h"
int main(void){storage_fault_gate_t g={0};
 assert(!storage_fault_gate_take(&g,1,0));storage_fault_gate_arm(&g,1,10);
 assert(!storage_fault_gate_take(&g,2,11));assert(storage_fault_gate_take(&g,1,12));assert(!storage_fault_gate_take(&g,1,13));
 storage_fault_gate_arm(&g,3,100);assert(!storage_fault_gate_take(&g,3,30000100));
 storage_fault_gate_arm(&g,2,100);storage_fault_gate_arm(&g,0,101);assert(!storage_fault_gate_take(&g,2,102));
 storage_fault_gate_arm(&g,1,100);assert(storage_fault_gate_take(&g,1,30000099));
 puts("PASS targeted one-shot, unrelated writes preserved, expiry and explicit cancellation");}
