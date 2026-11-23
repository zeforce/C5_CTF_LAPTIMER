#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/match.h"
int main(void){
 match_node_t nodes[MATCH_NODES]={0};match_summary_t total;
 for(unsigned i=0;i<9;i++){nodes[i].session=42;nodes[i].mode=1;nodes[i].team_ms[0]=100+i;nodes[i].pilot_ms[7]=UINT32_MAX;}
 match_summarize(42,1,511,nodes,&total);
 assert(total.included_mask==511 && total.missing_mask==0 && total.team_ms[0]==936);
 assert(total.pilot_ms[7]==(uint64_t)UINT32_MAX*9);
 nodes[1].session=41;nodes[2].mode=0;nodes[3].age_ms=5000;nodes[4].fault=3;
 match_summarize(42,1,31,nodes,&total);
 assert(total.missing_mask==14 && total.included_mask==17 && total.fault_count==1);
 assert(total.team_ms[0]==204);
 match_summarize(0,1,511,nodes,&total);assert(total.missing_mask==511 && !total.included_mask);
 match_summarize(42,1,1,nodes,&total);assert(total.expected_mask==1 && total.included_mask==1);
 nodes[0].pilot_captures[0]=3;nodes[1].pilot_captures[0]=5;
 nodes[0].pilot_laps[0]=4;nodes[0].pilot_fastest_ms[0]=1200;
 nodes[1].session=42;nodes[1].mode=1;nodes[1].pilot_laps[0]=3;nodes[1].pilot_fastest_ms[0]=1000;
 match_summarize(42,1,3,nodes,&total);assert(total.pilot_captures[0]==8);assert(total.pilot_laps[0]==7 && total.pilot_fastest_ms[0]==1000);
 nodes[1].age_ms=5000;match_summarize(42,1,3,nodes,&total);assert(total.pilot_laps[0]==4 && total.pilot_fastest_ms[0]==1200 && total.missing_mask==2);
 nodes[1].age_ms=0;nodes[0].pilot_captures[0]=UINT32_MAX;nodes[0].pilot_laps[0]=UINT32_MAX;match_summarize(42,1,3,nodes,&total);assert(total.pilot_laps[0]==UINT32_MAX && total.pilot_captures[0]==UINT32_MAX);
 puts("PASS same-session/mode/freshness/roster/missing/fault/64-bit score aggregation");
}
