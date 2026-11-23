#pragma once
#include <stdbool.h>
#include <stdint.h>
#define NODE_SPI_SLOTS 4
/* Board profile owns GPIO choices. Shared bus pins are fixed at 4/6/5.
 * This validator prevents collisions, not electrical suitability of a board. */
typedef struct {int cs,ready,interrupt;bool enabled;} node_slot_pins_t;
static inline bool node_slot_pin_valid(int pin){return pin>=0 && pin<=21 && pin!=4 && pin!=5 && pin!=6 && pin!=11 && pin!=12 && pin!=13 && pin!=14 && pin!=15 && pin!=16 && pin!=17;}
static inline bool node_slot_pins_valid(const node_slot_pins_t slots[NODE_SPI_SLOTS]){
 if(!slots)return false;
 for(unsigned i=0;i<NODE_SPI_SLOTS;i++)if(slots[i].enabled){
  int pins[3]={slots[i].cs,slots[i].ready,slots[i].interrupt};
  for(unsigned a=0;a<3;a++){
   if(a==2 && pins[a]==-1)continue; /* Events can be polled without INT. */
   if(!node_slot_pin_valid(pins[a]))return false;
   for(unsigned b=0;b<a;b++)if(pins[a]==pins[b])return false;
   for(unsigned j=0;j<i;j++)if(slots[j].enabled && (pins[a]==slots[j].cs || pins[a]==slots[j].ready || pins[a]==slots[j].interrupt))return false;
  }
 }return true;
}
