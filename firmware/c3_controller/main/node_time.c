#include "node_time.h"
#include <limits.h>
#include <string.h>
bool node_time_sample(node_time_t *m,uint32_t session,uint64_t node,uint64_t start,uint64_t end){
 if(!m)return false;
 if(m->session!=session){memset(m,0,sizeof(*m));m->session=session;}
 if(!session || end<start || end-start>20000 || end>INT64_MAX || node>INT64_MAX || (m->valid && (end<=m->sampled_us || node<=m->node_us))){++m->rejected;return false;}
 uint64_t mid=start+(end-start)/2;
 m->offset_us=(int64_t)mid-(int64_t)node;m->rtt_us=end-start;m->sampled_us=end;m->node_us=node;m->valid=true;++m->samples;return true;
}
bool node_time_map(const node_time_t *m,uint32_t session,uint64_t node,uint64_t now,uint64_t *local,uint32_t *uncertainty){
 if(!m || !local || !uncertainty || !m->valid || m->session!=session || now<m->sampled_us || now-m->sampled_us>3000000 || node>INT64_MAX)return false;
 uint64_t distance=node>m->node_us?node-m->node_us:m->node_us-node;
 if(distance>3000000)return false;
 if(m->offset_us<0 && node<(uint64_t)(-m->offset_us))return false;
 if(m->offset_us>0 && node>(uint64_t)INT64_MAX-(uint64_t)m->offset_us)return false;
 *local=(uint64_t)((int64_t)node+m->offset_us);
 *uncertainty=(m->rtt_us+1)/2+1+(distance+999)/1000;
 return true;
}
