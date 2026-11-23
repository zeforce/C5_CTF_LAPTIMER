#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_identity.h"
int main(void){
 network_identity_t table[8]={0};uint8_t peer[6]={0};
 for(unsigned i=0;i<8;i++){
  peer[0]=i+1;network_identity_t *slot=network_identity_slot(table,8,peer,100);
  assert(slot);uint8_t token[16]={0};network_identity_issue(slot,peer,i+1,token,100);
 }
 peer[0]=9;
 assert(!network_identity_slot(table,8,peer,2999999)); /* no live exchange evicted */
 assert(network_identity_slot(table,8,peer,3000100)==&table[0]);
 peer[0]=8;assert(network_identity_slot(table,8,peer,3000100)==&table[7]);
 peer[0]=9;assert(network_identity_slot(table,8,peer,99)==&table[0]);
 assert(!network_identity_slot(table,0,peer,3000100));
 network_identity_t s={0};uint8_t mac[6]={1},nonce[16]={3},old[16]={4};
 assert(!network_identity_admitted(&s,7,20,100));
 network_identity_issue(&s,mac,7,nonce,100);
 assert(!network_identity_accept(&s,8,10,nonce,101));
 assert(!network_identity_accept(&s,7,0,nonce,101));
 assert(!network_identity_accept(&s,7,10,old,101));
 assert(!network_identity_accept(&s,7,10,nonce,99));
 assert(!network_identity_accept(&s,7,10,nonce,3000100));
 assert(network_identity_accept(&s,7,10,nonce,101));
 assert(!network_identity_accept(&s,7,11,nonce,102));
 assert(!network_identity_admitted(&s,7,9,102));
 assert(!network_identity_admitted(&s,7,10,102));
 assert(!network_identity_admitted(&s,8,11,102));
 assert(network_identity_admitted(&s,7,11,102));
 assert(!network_identity_admitted(&s,7,11,3000100));
 network_identity_issue(&s,mac,8,old,200);
 assert(!network_identity_accept(&s,7,10,nonce,201));
 assert(!network_identity_accept(&s,8,10,nonce,201));
 assert(network_identity_accept(&s,8,UINT32_MAX,old,201));
 assert(network_identity_admitted(&s,8,1,202));
 assert(!network_identity_admitted(&s,7,12,202));
 puts("PASS boot proof, nonce, expiry, duplicate response, heartbeat sequence floor and reboot challenge");
}
