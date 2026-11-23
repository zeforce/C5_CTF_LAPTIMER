#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint8_t target;uint64_t expires;} storage_fault_gate_t;
static inline void storage_fault_gate_arm(storage_fault_gate_t *g,uint8_t target,uint64_t now){g->target=target;g->expires=target?now+30000000:0;}
static inline bool storage_fault_gate_take(storage_fault_gate_t *g,uint8_t target,uint64_t now){
 if(g->target && now>=g->expires){g->target=0;g->expires=0;}
 if(!target || g->target!=target)return false;
 g->target=0;g->expires=0;return true;
}
