#pragma once
#include "match.h"
#include "game.h"
#include <string.h>
/* Shared by firmware and the multi-node business integration test. */
typedef struct {
 uint32_t session;uint8_t mode,state;uint16_t roster;
 match_node_t nodes[MATCH_NODES];uint8_t pilot_valid[MATCH_NODES];
 uint8_t pilot_identity[8][24],node_mac[9][6];
 uint16_t outcome_mask;uint8_t node_outcome[9][24];
} score_snapshot_t;
static inline void encode_scores(const score_snapshot_t *snapshot,uint8_t out[32],unsigned subpage){
 match_node_t nodes[MATCH_NODES];memcpy(nodes,snapshot->nodes,sizeof(nodes));
 if(subpage)for(unsigned i=0;i<MATCH_NODES;i++)if(!(snapshot->pilot_valid[i]&(1u<<(subpage-1))))nodes[i].age_ms=UINT32_MAX;
 match_summary_t total;match_summarize(snapshot->session,snapshot->mode,snapshot->roster,nodes,&total);
 memset(out,0,32);out[0]=4;out[1]=subpage;out[2]=total.mode;out[3]=total.fault_count;
 memcpy(out+4,&total.session,4);memcpy(out+8,&total.expected_mask,2);memcpy(out+10,&total.included_mask,2);memcpy(out+12,&total.missing_mask,2);
 if(!subpage){memcpy(out+16,&total.team_ms[0],8);memcpy(out+24,&total.team_ms[1],8);}
 else {out[14]=snapshot->mode==GAME_LAPS?1:2;memcpy(out+16,&total.pilot_ms[subpage-1],8);if(out[14]==1){memcpy(out+24,&total.pilot_laps[subpage-1],4);memcpy(out+28,&total.pilot_fastest_ms[subpage-1],4);}else memcpy(out+24,&total.pilot_captures[subpage-1],4);}
}
