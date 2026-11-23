#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/game_page.h"
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int main(void){
 game_t g={0};uint8_t p[32];
 g.config.mode=5;g.match_config.mode=5;g.state=3;g.session=0x12345678;g.elapsed_us=9876543;g.score_us[1]=1234000;
 assert(game_page_encode(&g,0,0,p));assert(p[2]==5 && p[3]==3 && u32(p+8)==g.session && u32(p+12)==9876 && u32(p+24)==1234);
 g.config.freshness_ms=4000;g.config.solo_drain_percent=200;
 assert(game_page_encode(&g,1,0,p));assert(u32(p+16)==4000 && u32(p+20)==200);
 for(unsigned i=0;i<8;i++){
  g.config.pilots[i].channel=32+i;memcpy(g.config.pilots[i].name,"Pilot",6);
  assert(game_page_encode(&g,2+i,0,p));assert(p[3]==32+i && !strcmp((char*)p+10,"Pilot"));
  g.laps[i].total_laps=210;g.laps[i].recent_count=200;g.laps[i].recent_next=2;
  for(unsigned j=0;j<200;j++)g.laps[i].recent_us[j]=(j+1)*1000;
  assert(game_page_encode(&g,10+i,0,p));assert(u32(p+4)==210);
  assert(game_page_encode(&g,18+i,0,p));assert(p[3]==5 && u32(p+12)==2 && u32(p+16)==1 && u32(p+20)==200);
  assert(game_page_encode(&g,18+i,199,p));assert(p[3]==1 && u32(p+12)==3 && u32(p+16)==0);
 }
 g.solo_started=1;g.solo_failed=1;g.life_us=1234000;
 assert(game_page_encode(&g,26,0,p));assert(p[3]==1 && p[4]==1 && u32(p+8)==1234 && u32(p+12)==63 && p[16]==8);
 g.match_config=g.config;g.config.mode=2;g.config.pilots[0].name[0]='N';
 assert(game_page_encode(&g,0,0,p) && p[2]==5);
 assert(game_page_encode(&g,1,0,p) && p[2]==2);
 assert(game_page_encode(&g,27,0,p) && p[2]==5 && u32(p+28)==g.session);
 assert(game_page_encode(&g,28,0,p) && !strcmp((char*)p+10,"Pilot") && u32(p+28)==g.session);
 assert(game_page_encode(&g,35,0,p) && p[3]==39);
 assert(!game_page_encode(&g,36,0,p));assert(!game_page_encode(&g,0,200,p));
 assert(!game_page_encode(NULL,0,0,p));assert(!game_page_encode(&g,0,0,NULL));
 /* Nonzero SOLO identity must reach the same personal-result wire page. */
 game_init(&g);g.config.mode=GAME_SOLO;g.config.duration_ms=1000;
 g.config.pilots[0].enabled=0;g.config.pilots[7].enabled=1;
 assert(game_start(&g,0,1));game_observe(&g,7,true,true,0);game_tick(&g,1000000);
 assert(g.state==GAME_FINISHED && g.winner==1);
 assert(game_page_encode(&g,17,0,p) && p[2]==7 && u32(p+20)==1000);
 assert(game_page_encode(&g,10,0,p) && !u32(p+20));
 puts("PASS 36 game pages and frozen session config, units, all pilots, wrapped lap rings, last offset and argument bounds");
}
