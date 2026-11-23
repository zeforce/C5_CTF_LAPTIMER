#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_roster.h"
int main(void){network_roster_t r={.network=513,.count=2,.mac={{0x44,1,2,3,4,5},{0x48,1,2,3,4,5}}};network_roster_seal(&r);assert(network_roster_valid(&r,513));assert(!network_roster_valid(&r,514));r.mac[0][0]=1;network_roster_seal(&r);assert(!network_roster_valid(&r,513));r.mac[0][0]=0x48;network_roster_seal(&r);assert(!network_roster_valid(&r,513));r=(network_roster_t){.network=513,.count=2,.mac={{0x44,1,2,3,4,5},{0x48,1,2,3,4,5}}};network_roster_seal(&r);
 uint8_t fresh[6]={0x50,1,2,3,4,5},old[6];memcpy(old,r.mac[0],6);
 assert(!network_roster_edit(&r,old,r.mac[1]));assert(!memcmp(r.mac[0],old,6));
 assert(network_roster_edit(&r,old,fresh));assert(r.count==2 && !memcmp(r.mac[0],fresh,6));
 assert(network_roster_edit(&r,fresh,NULL));assert(r.count==1 && r.mac[0][0]==0x48);
 assert(network_roster_edit(&r,NULL,old));assert(r.count==2 && r.mac[1][0]==0x44);
 assert(!network_roster_edit(&r,NULL,old));assert(network_roster_edit(&r,old,NULL));
 assert(network_roster_edit(&r,r.mac[0],NULL));assert(r.count==0 && network_roster_valid(&r,513));
 r.count=9;network_roster_seal(&r);assert(!network_roster_valid(&r,513));puts("PASS roster network binding, checksum, duplicate/group address and capacity validation");}
