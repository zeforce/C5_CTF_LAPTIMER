#pragma once
#include "network_config.h"
#include <stdbool.h>
#include <string.h>
/* Bounded temporary transaction. Commit consumes even an incomplete request. */
typedef struct {network_config_t config;uint8_t mask;int64_t started,deadline;} network_provision_t;
static inline void network_provision_reset(network_provision_t *p){memset(p,0,sizeof(*p));}
static inline bool network_provision_live(network_provision_t *p,int64_t now){
 if(!p->mask || now<p->started || now>=p->deadline){network_provision_reset(p);return false;}return true;
}
static inline bool network_provision_begin(network_provision_t *p,uint8_t role,uint8_t channel,uint32_t network,int64_t now){
 network_provision_reset(p);
 if(now<0 || role>NET_MEMBER || channel<1 || channel>13 || (role && !network))return false;
 p->config.role=role;p->config.channel=channel;p->config.network=network;p->mask=1;
 p->started=now;p->deadline=now>INT64_MAX-120000000?INT64_MAX:now+120000000;return true;
}
static inline bool network_provision_key(network_provision_t *p,unsigned part,const uint8_t bytes[8],int64_t now){
 if(!network_provision_live(p,now) || part>1 || !bytes)return false;
 memcpy(p->config.key+8*part,bytes,8);p->mask|=2u<<part;return true;
}
static inline bool network_provision_host(network_provision_t *p,const uint8_t bytes[6],int64_t now){
 if(!network_provision_live(p,now) || !bytes)return false;
 memcpy(p->config.host,bytes,6);p->mask|=8;return true;
}
static inline bool network_provision_take(network_provision_t *p,network_config_t *out,int64_t now){
 bool valid=network_provision_live(p,now) && (p->mask&7)==7 && (p->config.role!=NET_MEMBER || (p->mask&8));
 if(valid && out)*out=p->config;else valid=false;
 network_provision_reset(p);return valid;
}
