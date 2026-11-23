#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/match_verdict.h"
int main(void){result_record_t r={.revision=1,.outcome_mask=3};uint8_t p[32];
 for(unsigned i=0;i<9;i++){r.pages[i][8]=3;r.pages[i][10]=3;r.pages[i][4]=42;}
 r.pages[0][2]=1;r.pages[0][16]=9;r.pages[0][24]=8;
 match_verdict_page(&r,0,p);assert(p[3]==VERDICT_TEAM && p[5]==1);
 r.pages[0][24]=9;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_TIE);
 r.outcome_mask=1;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_MISSING && p[18]==2);r.outcome_mask=3;
 r.node_outcome[1][3]=6;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_FAULT);r.node_outcome[1][3]=0;
 r.pages[0][2]=4;r.pilot_identity[0][0]=r.pilot_identity[1][0]=1;r.pages[1][16]=10;r.pages[2][16]=10;r.pages[3][16]=100;
 match_verdict_page(&r,0,p);assert(p[3]==VERDICT_TIE && p[6]==3);
 r.pages[2][16]=11;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_PILOT && p[5]==2 && p[6]==2);
 r.pages[0][2]=5;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_PILOT);
 r.pages[0][2]=3;for(unsigned i=0;i<2;i++){r.node_outcome[i][4]=1;r.node_outcome[i][2]=1;}
 match_verdict_page(&r,0,p);assert(p[3]==VERDICT_SOLO_OK);
 r.node_outcome[1][2]=0;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_SOLO_EARLY);
 r.node_outcome[1][4]=0;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_SOLO_UNSTARTED);
 r.node_outcome[0][5]=1;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_SOLO_FAIL);
 r.pages[0][2]=0;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_LAPS);
 r.pages[8][12]=2;match_verdict_page(&r,0,p);assert(p[3]==VERDICT_MISSING);
 uint8_t a[6]={2,3},b[6]={2,4};assert(match_member_identity(true,a,12,a,12));assert(!match_member_identity(true,a,12,a,13));assert(!match_member_identity(true,a,12,b,12));assert(match_member_identity(false,a,12,b,13));
 puts("PASS six-mode final verdicts, missing/fault gates, enabled pilot ties, all-node SOLO and frozen MAC/boot identity");}
