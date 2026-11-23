#pragma once
#include "result_record.h"
#include <string.h>
/* Verdicts are derived from a sealed terminal record, never live scores.
 * SOLO requires every expected flag node to succeed; any failed node fails
 * the challenge. Lap mode exposes totals only, no invented cross-gate race. */
enum {VERDICT_MISSING=1,VERDICT_TIE=2,VERDICT_TEAM=3,VERDICT_PILOT=4,VERDICT_SOLO_OK=5,VERDICT_SOLO_FAIL=6,VERDICT_SOLO_UNSTARTED=7,VERDICT_SOLO_EARLY=8,VERDICT_LAPS=9,VERDICT_FAULT=10};
static inline void match_verdict_page(const result_record_t *r,unsigned index,uint8_t out[32]){
 memset(out,0,32);out[0]=22;out[1]=index;
 if(!r || !r->revision)return;
 out[2]=1;out[4]=r->pages[0][2];memcpy(out+8,&r->revision,4);memcpy(out+12,r->pages[0]+4,4);
 uint16_t expected;memcpy(&expected,r->pages[0]+8,2);memcpy(out+16,&expected,2);
 uint16_t missing=expected&~r->outcome_mask;uint8_t faults=0;
 for(unsigned i=0;i<9;i++){uint16_t m;memcpy(&m,r->pages[i]+12,2);missing|=m;if((expected&(1u<<i)) && (r->outcome_mask&(1u<<i)) && r->node_outcome[i][3])++faults;}
 memcpy(out+18,&missing,2);out[7]=faults;
 if(missing){out[3]=VERDICT_MISSING;return;}if(faults){out[3]=VERDICT_FAULT;return;}
 if(out[4]==0){out[3]=VERDICT_LAPS;return;}
 if(out[4]==3){bool failed=false,unstarted=false,early=false;
  for(unsigned i=0;i<9;i++)if(expected&(1u<<i)){const uint8_t *o=r->node_outcome[i];failed|=o[5]!=0;unstarted|=!o[4];early|=o[2]!=1;}
  out[3]=failed?VERDICT_SOLO_FAIL:unstarted?VERDICT_SOLO_UNSTARTED:early?VERDICT_SOLO_EARLY:VERDICT_SOLO_OK;return;
 }
 if(out[4]==1 || out[4]==2){uint64_t red,blue;memcpy(&red,r->pages[0]+16,8);memcpy(&blue,r->pages[0]+24,8);
  out[3]=red==blue?VERDICT_TIE:VERDICT_TEAM;out[5]=red==blue?0:red>blue?1:2;return;
 }
 uint64_t best=0;uint8_t leaders=0;
 for(unsigned i=0;i<8;i++)if(r->pilot_identity[i][0]){uint64_t score;memcpy(&score,r->pages[i+1]+16,8);if(score>best){best=score;leaders=1u<<i;}else if(score==best && best)leaders|=1u<<i;}
 out[6]=leaders;out[3]=!best || (leaders&(leaders-1))?VERDICT_TIE:VERDICT_PILOT;
 if(out[3]==VERDICT_PILOT)for(unsigned i=0;i<8;i++)if(leaders&(1u<<i))out[5]=i+1;
}
/* Frozen roster MAC and boot are both required for an in-session report. */
static inline bool match_member_identity(bool frozen,const uint8_t expected_mac[6],uint32_t expected_boot,const uint8_t mac[6],uint32_t boot){return !frozen || (expected_boot && expected_boot==boot && !memcmp(expected_mac,mac,6));}
