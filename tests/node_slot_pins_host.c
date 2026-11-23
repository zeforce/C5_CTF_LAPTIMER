#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/node_slot_pins.h"
int main(void){
 node_slot_pins_t s[4]={{7,3,10,true},{0,1,2,true},{0},{0}};
 assert(node_slot_pins_valid(s));s[1].cs=7;assert(!node_slot_pins_valid(s));s[1].cs=0;s[1].ready=0;assert(!node_slot_pins_valid(s));s[1].ready=4;assert(!node_slot_pins_valid(s));s[1].ready=12;assert(!node_slot_pins_valid(s));s[1].ready=22;assert(!node_slot_pins_valid(s));s[1].ready=1;s[2]=(node_slot_pins_t){8,9,18,true};s[3]=(node_slot_pins_t){19,20,21,true};assert(node_slot_pins_valid(s));s[3].interrupt=-2;assert(!node_slot_pins_valid(s));s[3].enabled=false;assert(node_slot_pins_valid(s));assert(!node_slot_pins_valid(NULL));
 s[3].enabled=true;s[3].interrupt=-1;assert(node_slot_pins_valid(s));
 s[1].interrupt=-1;assert(node_slot_pins_valid(s));
 s[1].cs=-1;assert(!node_slot_pins_valid(s));s[1].cs=0;
 s[1].ready=-1;assert(!node_slot_pins_valid(s));s[1].ready=1;
 s[1].interrupt=-2;assert(!node_slot_pins_valid(s));
 puts("PASS pin uniqueness, shared SPI/flash rejection, optional INT and mandatory CS/READY");
}
