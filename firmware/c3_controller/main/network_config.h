#pragma once
#include <stdint.h>
#include <stdbool.h>
enum { NET_STANDALONE=0, NET_HOST=1, NET_MEMBER=2 };
typedef struct { uint8_t role, channel; uint32_t network; uint8_t key[16]; uint8_t host[6]; } network_config_t;

static inline bool network_config_valid(const network_config_t *c){
 if(!c || c->role>NET_MEMBER || c->channel<1 || c->channel>13 || (c->role && !c->network))return false;
 uint8_t key=0,host=0;for(unsigned i=0;i<16;i++)key|=c->key[i];for(unsigned i=0;i<6;i++)host|=c->host[i];
 return (!c->role || key) && (c->role!=NET_MEMBER || (host && !(c->host[0]&1)));
}
/* Invalid persistence falls back to a configurable standalone controller. */
static inline bool network_config_restore(network_config_t *out,const network_config_t *saved,bool complete){
 *out=(network_config_t){.channel=6};
 if(!complete || !network_config_valid(saved))return false;
 *out=*saved;return true;
}
