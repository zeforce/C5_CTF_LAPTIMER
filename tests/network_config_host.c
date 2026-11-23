#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/network_config.h"
int main(void){
 network_config_t c={.role=NET_HOST,.channel=1,.network=513,.key={1}},out;
 assert(network_config_valid(&c));assert(network_config_restore(&out,&c,true));assert(out.network==513);
 c.role=NET_MEMBER;assert(!network_config_valid(&c));c.host[0]=3;assert(!network_config_valid(&c));c.host[0]=2;assert(network_config_valid(&c));
 c.key[0]=0;assert(!network_config_valid(&c));assert(!network_config_restore(&out,&c,true));assert(out.role==NET_STANDALONE && out.channel==6 && !out.network);
 c.key[0]=1;c.channel=14;assert(!network_config_valid(&c));c.channel=0;assert(!network_config_valid(&c));c.channel=13;assert(network_config_valid(&c));
 assert(!network_config_restore(&out,&c,false));assert(out.channel==6 && !out.key[0]);
 c.network=0;assert(!network_config_valid(&c));c.role=NET_STANDALONE;assert(network_config_valid(&c));
 assert(!network_config_valid(NULL));assert(!network_config_restore(&out,NULL,false));
 c.role=3;assert(!network_config_valid(&c));
 puts("PASS shared network validation and invalid/truncated persistence standalone recovery");
}
