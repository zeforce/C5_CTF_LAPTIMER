#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_guard.h"
int main(void){
 uint32_t boots[8]={11,12};network_guard_node_t nodes[8]={{.boot=11,.session=42,.state=1,.mode=2},{.boot=12,.session=42,.state=1,.mode=2}};
 assert(!network_guard_missing(42,2,7,6000,boots,nodes));
 nodes[0].age_ms=5000;assert(network_guard_missing(42,2,7,100,boots,nodes)==2);
 nodes[0].age_ms=0;nodes[1].boot=13;assert(network_guard_missing(42,2,7,100,boots,nodes)==4);
 nodes[1].boot=12;nodes[1].state=2;assert(!network_guard_missing(42,2,7,4999,boots,nodes));
 assert(network_guard_missing(42,2,7,5000,boots,nodes)==4);
 nodes[1].state=1;nodes[1].session=41;assert(network_guard_missing(42,2,7,5000,boots,nodes)==4);
 assert(!network_guard_missing(42,2,1,6000,boots,nodes));
 nodes[1].session=42;nodes[1].mode=3;
 assert(!network_guard_missing(42,2,7,4999,boots,nodes));
 assert(network_guard_missing(42,2,7,5000,boots,nodes)==4);
 nodes[1].mode=2;assert(!network_guard_missing(42,2,7,5000,boots,nodes));
 for(unsigned mode=0;mode<6;mode++){
  nodes[0].mode=nodes[1].mode=mode;
  assert(!network_guard_missing(42,mode,7,5000,boots,nodes));
  nodes[1].mode=(mode+1)%6;
  assert(network_guard_missing(42,mode,7,5000,boots,nodes)==4);
  assert(!network_guard_missing(42,mode,3,5000,boots,nodes));
 }
 puts("PASS host guard: stale/reboot/session/state mismatch, start grace, roster isolation");
}
