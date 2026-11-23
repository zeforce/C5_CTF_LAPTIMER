#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_provision.h"
int main(void){
 network_provision_t p={0};network_config_t out={0};uint8_t key[8]={1},host[6]={2};
 assert(!network_provision_take(&p,&out,0));
 assert(network_provision_begin(&p,NET_MEMBER,1,513,10));
 assert(network_provision_key(&p,1,key,11));assert(network_provision_key(&p,0,key,12));
 assert(!network_provision_take(&p,&out,13));assert(!p.mask); /* missing host consumes */
 assert(network_provision_begin(&p,NET_MEMBER,1,513,20));
 assert(network_provision_key(&p,0,key,21));assert(network_provision_key(&p,1,key,22));
 assert(network_provision_host(&p,host,23));assert(network_provision_take(&p,&out,24));
 assert(out.network==513 && out.host[0]==2 && out.key[8]==1 && !p.mask);
 assert(!network_provision_take(&p,&out,25));
 assert(network_provision_begin(&p,NET_HOST,13,513,30));
 assert(!network_provision_key(&p,0,key,120000030));assert(!p.mask && !p.config.key[0]);
 assert(network_provision_begin(&p,NET_HOST,1,513,40));assert(!network_provision_host(&p,host,39));
 assert(!network_provision_begin(&p,NET_HOST,14,513,0));
 assert(!network_provision_begin(&p,NET_HOST,1,0,0));
 assert(network_provision_begin(&p,NET_STANDALONE,1,0,0));
 assert(network_provision_key(&p,0,key,1));network_provision_reset(&p);
 for(unsigned i=0;i<sizeof(p);i++)assert(!((uint8_t*)&p)[i]);
 puts("PASS provisioning expiry, clock rollback, required fragments, member host, consume-on-commit, reset and validation");
}
