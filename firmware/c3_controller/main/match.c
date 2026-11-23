#include "match.h"
#include <string.h>
void match_summarize(uint32_t session,uint8_t mode,uint16_t expected,
                     const match_node_t nodes[MATCH_NODES],match_summary_t *out){
    memset(out,0,sizeof(*out));out->session=session;out->mode=mode;
    out->expected_mask=expected&((1u<<MATCH_NODES)-1);
    for(unsigned i=0;i<MATCH_NODES;i++){
        uint16_t bit=1u<<i;if(!(out->expected_mask&bit))continue;
        const match_node_t *n=&nodes[i];
        if(!session || n->session!=session || n->mode!=mode || n->age_ms>=5000){out->missing_mask|=bit;continue;}
        out->included_mask|=bit;out->fault_count+=n->fault!=0;
        for(unsigned j=0;j<2;j++)out->team_ms[j]+=n->team_ms[j];
        for(unsigned j=0;j<8;j++){
            out->pilot_ms[j]+=n->pilot_ms[j];
            uint32_t captures=n->pilot_captures[j];
            out->pilot_captures[j]=UINT32_MAX-out->pilot_captures[j]<captures?UINT32_MAX:out->pilot_captures[j]+captures;
            uint32_t laps=n->pilot_laps[j];
            out->pilot_laps[j]=UINT32_MAX-out->pilot_laps[j]<laps?UINT32_MAX:out->pilot_laps[j]+laps;
            uint32_t fastest=n->pilot_fastest_ms[j];
            if(fastest && (!out->pilot_fastest_ms[j] || fastest<out->pilot_fastest_ms[j]))out->pilot_fastest_ms[j]=fastest;
        }
    }
}
