#include "network_roster.h"
#include <stddef.h>
#include <string.h>
static uint32_t hash(const network_roster_t *r){uint32_t h=2166136261u;const uint8_t *p=(const uint8_t*)r;for(unsigned i=0;i<offsetof(network_roster_t,checksum);i++)h=(h^p[i])*16777619u;return h;}
void network_roster_seal(network_roster_t *r){r->version=1;r->checksum=hash(r);}
bool network_roster_valid(const network_roster_t *r,uint32_t network){
 if(!r || r->version!=1 || !network || r->network!=network || r->count>8 || r->checksum!=hash(r))return false;
 for(unsigned i=0;i<r->count;i++){
  uint8_t any=0;for(unsigned j=0;j<6;j++)any|=r->mac[i][j];
  if(!any || (r->mac[i][0]&1))return false;
  for(unsigned j=0;j<i;j++)if(!memcmp(r->mac[i],r->mac[j],6))return false;
 }
 return true;
}

bool network_roster_edit(network_roster_t *r,const uint8_t *old,const uint8_t *replacement){
 if(!r || !network_roster_valid(r,r->network) || (!old && !replacement))return false;
 network_roster_t next=*r;unsigned index=next.count;
 if(old){for(unsigned i=0;i<next.count;i++)if(!memcmp(next.mac[i],old,6)){index=i;break;}if(index==next.count)return false;}
 if(replacement){if(index>=8)return false;memcpy(next.mac[index],replacement,6);if(index==next.count)++next.count;}
 else {for(unsigned i=index+1;i<next.count;i++)memcpy(next.mac[i-1],next.mac[i],6);--next.count;memset(next.mac[next.count],0,6);}
 network_roster_seal(&next);if(!network_roster_valid(&next,next.network))return false;*r=next;return true;
}
