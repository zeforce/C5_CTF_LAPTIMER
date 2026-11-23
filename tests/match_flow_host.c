#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/match_snapshot.h"
#include "../firmware/c3_controller/main/network_score.h"
#include "../firmware/c3_controller/main/match_verdict.h"
#include "../firmware/c3_controller/main/result_settle.h"

/* Deterministic observations are test input only, never a device command. */
static void play(game_t *g,unsigned mode,unsigned node){
 game_init(g);g->config.mode=mode;g->config.capture_ms=100;
 g->config.duration_ms=1000;g->config.freshness_ms=60000;
 if(mode!=GAME_SOLO){g->config.pilots[1].enabled=1;g->config.pilots[1].slot=1;}
 assert(game_start(g,1000,15));g->session=42;
 if(mode==GAME_LAPS){
  for(unsigned pilot=0;pilot<2;pilot++)for(unsigned edge=0;edge<5;edge++){
   lap_event_t event={.session=9+node,.sequence=edge+1,.timestamp_us=1000+(uint64_t)edge*(600000+node*100000),.edge=edge%2?LAP_EXIT:LAP_ENTER,.valid=true};
   lap_timer_consume(&g->laps[pilot],&event,NULL);
  }
  game_stop(g,4000000);return;
 }
 game_observe(g,0,node==0 || mode==GAME_SOLO,true,1000);
 if(mode!=GAME_SOLO)game_observe(g,1,node==1,true,1000);
 game_tick(g,1001000);assert(g->state==GAME_FINISHED);
}
static void collect(const game_t *g,unsigned node,score_snapshot_t *snapshot){
 match_node_t *n=&snapshot->nodes[node];n->session=g->session;n->mode=g->match_config.mode;n->state=g->state;n->fault=g->fault;
 for(unsigned j=0;j<2;j++)n->team_ms[j]=g->score_us[j]/1000;
 for(unsigned j=0;j<8;j++){
  network_score_t sent={.session=g->session,.pilot=j,.owned_ms=g->pilot_score_us[j]/1000,.captures=g->captures[j],.laps=g->laps[j].total_laps,.fastest_ms=g->laps[j].fastest_us/1000},received;
  uint8_t wire[21];assert(network_score_encode(&sent,wire));assert(network_score_decode(wire,21,&received));
  assert(network_score_current(&received,42,101+j,100));assert(result_terminal_score(100,101+j));
  n->pilot_ms[j]=received.owned_ms;n->pilot_captures[j]=received.captures;n->pilot_laps[j]=received.laps;n->pilot_fastest_ms[j]=received.fastest_ms;
 }
 snapshot->pilot_valid[node]=255;snapshot->outcome_mask|=1u<<node;
 uint8_t *o=snapshot->node_outcome[node];o[0]=g->state;o[1]=g->match_config.mode;o[2]=g->winner;o[3]=g->fault;o[4]=g->solo_started;o[5]=g->solo_failed;o[6]=g->solo_failed?3:g->elapsed_us>=g->match_config.duration_ms*1000ull?2:1;
}
static void seal(const score_snapshot_t *s,result_record_t *r){
 memset(r,0,sizeof(*r));r->revision=1;r->network=513;r->outcome_mask=s->outcome_mask;
 memcpy(r->node_outcome,s->node_outcome,sizeof(r->node_outcome));memcpy(r->pilot_identity,s->pilot_identity,sizeof(r->pilot_identity));
 for(unsigned page=0;page<9;page++)encode_scores(s,r->pages[page],page);
 result_record_seal(r);assert(result_record_valid(r));
}
int main(void){
 for(unsigned mode=0;mode<6;mode++){
  score_snapshot_t s={.session=42,.mode=mode,.state=GAME_FINISHED,.roster=3};game_t g;
  for(unsigned node=0;node<2;node++){play(&g,mode,node);collect(&g,node,&s);}
  for(unsigned j=0;j<8;j++){s.pilot_identity[j][0]=g.match_config.pilots[j].enabled;s.pilot_identity[j][1]=g.match_config.pilots[j].channel;s.pilot_identity[j][2]=g.match_config.pilots[j].team;s.pilot_identity[j][3]=g.match_config.pilots[j].slot;}
  result_record_t record;seal(&s,&record);assert(!result_record_missing(&record));
  result_history_t history={.version=1};assert(result_history_push(&history,&record));assert(result_history_valid(&history));
  uint8_t verdict[32];match_verdict_page(&history.records[0],0,verdict);
  assert(verdict[3]==(mode==0?VERDICT_LAPS:mode==3?VERDICT_SOLO_OK:VERDICT_TIE));
  if(mode==0){uint32_t laps,fastest;memcpy(&laps,record.pages[1]+24,4);memcpy(&fastest,record.pages[1]+28,4);assert(laps==4 && fastest==1200);}
  if(mode==1 || mode==2){uint64_t red,blue;memcpy(&red,record.pages[0]+16,8);memcpy(&blue,record.pages[0]+24,8);assert(red==(mode==1?900:1000) && red==blue);}
  if(mode>=4){uint64_t owned;memcpy(&owned,record.pages[1]+16,8);assert(owned==1000 && verdict[6]==3);}
  /* A genuinely asymmetric second gate changes the final winner. */
  if(mode==1 || mode==2){s.nodes[1].team_ms[1]--;seal(&s,&record);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_TEAM && verdict[5]==1);s.nodes[1].team_ms[1]++;}
  if(mode>=4){s.nodes[1].pilot_ms[1]--;seal(&s,&record);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_PILOT && verdict[5]==1);s.nodes[1].pilot_ms[1]++;}
  if(mode==GAME_SOLO){
   /* One failed node defeats the challenge despite success at the other. */
   game_init(&g);g.config.mode=GAME_SOLO;g.config.capture_ms=100;g.config.duration_ms=1000;g.config.freshness_ms=60000;
   assert(game_start(&g,1000,1));g.session=42;game_observe(&g,0,true,true,1000);game_tick(&g,101000);
   game_observe(&g,0,false,true,101000);game_tick(&g,301000);assert(g.solo_failed);
   collect(&g,1,&s);seal(&s,&record);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_SOLO_FAIL);
  }
  /* Lost final pilot report and expired heartbeat cannot produce a winner. */
  s.pilot_valid[1]&=~1u;seal(&s,&record);assert(result_record_missing(&record)==2);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_MISSING);
  assert(!result_settle_ready(9999999,2) && result_settle_ready(10000000,2));
  s.pilot_valid[1]=255;s.nodes[1].age_ms=5000;seal(&s,&record);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_MISSING);
  s.nodes[1].age_ms=0;s.node_outcome[1][3]=3;seal(&s,&record);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_FAULT);
 }
 /* Full supported roster: one host plus eight members, each with eight
  * independent terminal pilot reports. This is capacity of the production
  * aggregation path, not an ESP-NOW radio/load or scheduler simulation. */
 for(unsigned mode=0;mode<GAME_MODE_COUNT;mode++){
  score_snapshot_t s={.session=42,.mode=mode,.state=GAME_FINISHED,.roster=511};game_t g;
  for(unsigned node=0;node<MATCH_NODES;node++){play(&g,mode,mode==GAME_LAPS?node:node?1:0);collect(&g,node,&s);}
  for(unsigned j=0;j<8;j++){
   s.pilot_identity[j][0]=g.match_config.pilots[j].enabled;
   s.pilot_identity[j][1]=g.match_config.pilots[j].channel;
   s.pilot_identity[j][2]=g.match_config.pilots[j].team;
   s.pilot_identity[j][3]=g.match_config.pilots[j].slot;
  }
  result_record_t record;seal(&s,&record);assert(!result_record_missing(&record));
  uint16_t expected,included;memcpy(&expected,record.pages[0]+8,2);memcpy(&included,record.pages[0]+10,2);
  assert(expected==511 && included==511);
  uint8_t verdict[32];match_verdict_page(&record,0,verdict);
  assert(verdict[3]==(mode==GAME_LAPS?VERDICT_LAPS:mode==GAME_SOLO?VERDICT_SOLO_OK:mode<=GAME_QUICK?VERDICT_TEAM:VERDICT_PILOT));
  if(mode==GAME_FLAG || mode==GAME_QUICK){
   uint64_t red,blue;memcpy(&red,record.pages[0]+16,8);memcpy(&blue,record.pages[0]+24,8);
   assert(red==(mode==GAME_FLAG?900:1000));assert(blue==red*8 && verdict[5]==2);
  }
  if(mode==GAME_LAPS){uint32_t laps;memcpy(&laps,record.pages[1]+24,4);assert(laps==18);}
  if(mode>=GAME_FFA){uint64_t owned;memcpy(&owned,record.pages[2]+16,8);assert(owned==8000 && verdict[5]==2);}
  result_history_t history={.version=1};assert(result_history_push(&history,&record));assert(result_history_valid(&history));
  /* Every roster bit, including the highest member bit, must independently
   * block a verdict if any pilot report or terminal outcome is absent. */
  for(unsigned node=0;node<MATCH_NODES;node++){
   const uint16_t bit=1u<<node;
   s.pilot_valid[node]&=~128u;seal(&s,&record);assert(result_record_missing(&record)==bit);
   match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_MISSING);s.pilot_valid[node]=255;
   s.outcome_mask&=~bit;seal(&s,&record);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_MISSING);s.outcome_mask|=bit;
   s.node_outcome[node][3]=6;seal(&s,&record);match_verdict_page(&record,0,verdict);assert(verdict[3]==VERDICT_FAULT && verdict[7]==1);s.node_outcome[node][3]=0;
  }
  seal(&s,&record);assert(!result_record_missing(&record));
 }
 puts("PASS six modes: two-node and nine-node scoring, score wire, production aggregation, sealing/history/verdict, per-node missing/fault gates; synthetic host inputs only");
}
