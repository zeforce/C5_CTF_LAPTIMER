#include "network_guard.h"
uint16_t network_guard_missing(uint32_t session,uint8_t mode,uint16_t roster,uint32_t elapsed,
    const uint32_t boot[8],const network_guard_node_t nodes[8]){
 uint16_t missing=0;if(!session)return roster&0x1fe;
 for(unsigned i=0;i<8;i++)if(roster&(1u<<(i+1))){
  const network_guard_node_t *n=&nodes[i];
  if(n->age_ms>=5000 || n->boot!=boot[i] ||
     (elapsed>=5000 && (n->session!=session || n->state!=1 || n->mode!=mode)))missing|=1u<<(i+1);
 }
 return missing;
}
