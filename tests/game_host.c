#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/c3_controller/main/game.h"
#include "../firmware/common/fpv_channels.h"
#include "../firmware/c3_controller/main/game_identity.h"
int main(void){
 assert(game_name_valid("pilot",6));assert(game_name_valid("\xe9\xa3\x9e\xe6\x89\x8b",7));
 assert(game_name_valid("\xf0\x9f\x9a\x80",5));
 assert(!game_name_valid("",1));assert(!game_name_valid("1234567890123",14));
 const char *invalid[]={"\x80","\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe9\xa3","\xe9\x41\x80","\xf5\x80\x80\x80"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)assert(!game_name_valid(invalid[i],13));
 char unterminated[12];memset(unterminated,'A',sizeof(unterminated));assert(!game_name_valid(unterminated,sizeof(unterminated)));
 game_t identity;game_init(&identity);identity.config.pilots[7].color=0x1000000;assert(!game_config_valid(&identity.config));
 identity.config.pilots[7].color=0;assert(game_config_valid(&identity.config));
 identity.config.pilots[7].name[0]=0;assert(!game_config_valid(&identity.config));
 /* Different band indices at one frequency cannot identify two pilots. */
 game_t aliases;game_init(&aliases);aliases.config.pilots[1].enabled=1;
 for(unsigned mode=0;mode<GAME_MODE_COUNT;mode++){
  if(mode==GAME_SOLO)continue;
  aliases.config.mode=mode;
  for(unsigned a=0;a<64;a++)for(unsigned b=0;b<64;b++){
   aliases.config.pilots[0].channel=a;aliases.config.pilots[1].channel=b;
   assert(game_config_valid(&aliases.config)==(fpv_channel_frequency(a)!=fpv_channel_frequency(b)));
  }
 }
 aliases.config.mode=GAME_FLAG;aliases.config.pilots[0].channel=36;aliases.config.pilots[1].channel=44;
 assert(!game_start(&aliases,0,3));aliases.config.pilots[1].enabled=0;assert(game_config_valid(&aliases.config));
 game_t g;game_init(&g);g.config.mode=GAME_FLAG;g.config.capture_ms=1000;g.config.freshness_ms=60000;g.config.duration_ms=10000;
 g.config.pilots[1].enabled=1;
 assert(game_start(&g,0,1));char savedName=g.match_config.pilots[0].name[0];g.config.pilots[0].name[0]='X';assert(g.match_config.pilots[0].name[0]==savedName);game_observe(&g,0,true,true,0);game_observe(&g,1,false,true,0);
 game_tick(&g,1500000);assert(g.owner==1 && g.score_us[0]==500000);
 game_observe(&g,1,true,true,1500000);game_tick(&g,2500000);assert(g.owner==1 && g.progress_us==0 && g.score_us[0]==1500000);
 game_observe(&g,0,false,true,2500000);game_tick(&g,3000000);assert(g.progress_us==500000);
 game_observe(&g,1,false,true,3000000);game_tick(&g,3250000);assert(g.progress_us==250000);
 game_observe(&g,1,true,true,3250000);game_tick(&g,4000000);assert(g.owner==2 && g.progress_us==0);
 assert(g.score_us[0]==3000000 && g.score_us[1]==0);
 game_tick(&g,4500000);assert(g.score_us[1]==500000);
 game_pause(&g,3,4500000);game_tick(&g,9000000);assert(g.elapsed_us==4500000);
 assert(game_resume(&g,9000000));game_tick(&g,10000000);assert(g.elapsed_us==4500000);
 game_observe(&g,0,false,true,10000000);game_observe(&g,1,true,true,10000000);
 game_tick(&g,20000000);assert(g.state==GAME_FINISHED && g.elapsed_us==10000000 && g.winner==2);
 game_init(&g);g.config.pilots[1].enabled=1;assert(!game_start(&g,0,1));
 g.config.pilots[1].enabled=0;assert(game_start(&g,0,1));assert(!game_start(&g,1,1));
 game_init(&g);g.config.mode=GAME_FLAG;assert(game_start(&g,0,1));game_observe(&g,0,true,true,0);game_tick(&g,3000000);assert(g.state==GAME_PAUSED && g.fault==2 && g.elapsed_us==0);
 game_init(&g);g.config.mode=GAME_QUICK;g.config.freshness_ms=60000;assert(game_start(&g,0,1));game_observe(&g,0,true,true,0);game_tick(&g,1000);assert(g.owner==1);
 game_init(&g);g.config.mode=GAME_SOLO;g.config.capture_ms=1000;g.config.freshness_ms=60000;assert(game_start(&g,0,1));game_observe(&g,0,true,true,0);game_tick(&g,500000);assert(g.solo_started);
 game_observe(&g,0,false,true,500000);game_tick(&g,2000000);assert(g.solo_failed && g.state==GAME_FINISHED);
 game_init(&g);g.config.mode=GAME_FFA;g.config.freshness_ms=60000;g.config.pilots[7].enabled=1;assert(game_start(&g,0,1));game_observe(&g,0,false,true,0);game_observe(&g,7,true,true,0);game_tick(&g,10000);assert(g.pilot_owner==8 && g.captures[7]==1);
 game_observe(&g,7,false,true,10000);game_observe(&g,0,true,true,10000);game_tick(&g,20000);assert(g.pilot_owner==1 && g.captures[0]==1);
 game_init(&g);g.config.mode=GAME_LOCK;g.config.freshness_ms=60000;g.config.pilots[1].enabled=1;assert(game_start(&g,0,1));game_observe(&g,0,true,true,0);game_observe(&g,1,false,true,0);game_tick(&g,1000);assert(g.pilot_owner==1);
 game_observe(&g,0,false,true,1000);game_observe(&g,1,true,true,1000);game_tick(&g,2000);assert(g.pilot_owner==1);
 game_init(&g);g.config.mode=GAME_SOLO;g.config.capture_ms=1000;g.config.freshness_ms=60000;g.config.solo_drain_percent=200;g.config.solo_recover_percent=50;
 assert(game_start(&g,0,1));game_observe(&g,0,true,true,0);game_tick(&g,100000);
 game_observe(&g,0,false,true,100000);game_tick(&g,300000);assert(g.life_us==600000);
 game_observe(&g,0,true,true,300000);game_tick(&g,500000);assert(g.life_us==700000);
 game_observe(&g,0,false,true,500000);game_tick(&g,1000000);assert(g.solo_failed && g.state==GAME_FINISHED && g.elapsed_us==850000);
 game_init(&g);g.config.solo_drain_percent=0;assert(!game_config_valid(&g.config));
 g.config.solo_drain_percent=1001;assert(!game_config_valid(&g.config));
 /* Missing initial observations must time out even while tick keeps advancing. */
 game_init(&g);g.config.mode=GAME_FLAG;assert(game_start(&g,10000000,1));
 game_tick(&g,11000000);game_tick(&g,12000000);assert(g.state==GAME_RUNNING && !g.elapsed_us);
 game_tick(&g,12500001);assert(g.state==GAME_PAUSED && g.fault==2 && !g.elapsed_us);
 assert(game_resume(&g,20000000));game_tick(&g,22500000);assert(g.state==GAME_RUNNING);
 game_tick(&g,22500001);assert(g.state==GAME_PAUSED && g.fault==2);
 /* A partial sweep and refreshed first pilot cannot hide a missing second. */
 game_init(&g);g.config.mode=GAME_FLAG;g.config.pilots[1].enabled=1;assert(game_start(&g,0,1));
 game_observe(&g,0,true,true,1000000);game_observe(&g,0,true,true,2000000);
 game_tick(&g,2500001);assert(g.state==GAME_PAUSED && !g.elapsed_us);
 /* A late observation cannot move timestamps or presence backward. */
 game_init(&g);g.config.mode=GAME_FLAG;assert(game_start(&g,10000000,1));
 game_observe(&g,0,true,true,11000000);game_observe(&g,0,false,true,10500000);
 assert(g.seen_us[0]==11000000 && g.present_mask==1);
 /* Solo success requires triggered play and reaching the configured duration. */
 game_init(&g);g.config.mode=GAME_SOLO;g.config.duration_ms=1000;g.config.freshness_ms=60000;
 assert(game_start(&g,0,1));game_stop(&g,100);assert(g.winner==0);
 assert(game_start(&g,200,1));game_observe(&g,0,true,true,200);game_tick(&g,500200);game_stop(&g,500200);assert(g.solo_started && g.winner==0);
 assert(game_start(&g,1000000,1));game_observe(&g,0,true,true,1000000);game_tick(&g,2000000);assert(g.state==GAME_FINISHED && g.winner==1);
 /* Draft changes cannot alter the finished session or active scoring rules. */
 game_config_t frozen=g.match_config;g.config.mode=GAME_LOCK;g.config.duration_ms=9000;g.config.pilots[0].name[0]='Z';
 assert(game_session_config(&g)->mode==GAME_SOLO && game_session_config(&g)->duration_ms==1000);
 assert(game_session_config(&g)->pilots[0].name[0]==frozen.pilots[0].name[0]);
 assert(game_start(&g,3000000,1));assert(g.match_config.mode==GAME_LOCK && g.match_config.duration_ms==9000);
 g.config.mode=GAME_SOLO;game_observe(&g,0,true,true,3000000);game_tick(&g,4000000);assert(g.pilot_owner==1 && g.pilot_score_us[0]==1000000);
 uint64_t revision=g.fault_revision;game_pause(&g,3,4000001);assert(g.fault_revision==revision+1);
 game_pause(&g,3,4000002);assert(g.fault_revision==revision+1);
 assert(game_resume(&g,4000003));assert(g.fault_revision==revision+1);
 game_pause(&g,0,4000004);assert(g.fault_revision==revision+1);
 game_stop(&g,4000005);assert(game_start(&g,5000000,1));assert(g.fault_revision==revision+1);
 game_pause(&g,4,5000001);assert(g.fault_revision==revision+2);
 /* SOLO result belongs to the selected identity, including nonzero indices.
  * Pause time is excluded; failure credits only the life remaining interval. */
 game_init(&g);g.config.mode=GAME_SOLO;g.config.pilots[0].enabled=0;g.config.pilots[7].enabled=1;
 g.config.capture_ms=1000;g.config.freshness_ms=60000;g.config.solo_drain_percent=200;
 assert(game_start(&g,0,1));game_observe(&g,7,true,true,0);game_tick(&g,100000);
 assert(g.pilot_score_us[7]==100000 && !g.pilot_score_us[0]);
 game_pause(&g,0,100000);assert(game_resume(&g,9000000));game_observe(&g,7,false,true,9000000);
 game_tick(&g,10000000);assert(g.solo_failed && g.state==GAME_FINISHED);
 assert(g.elapsed_us==600000 && g.score_us[0]==600000 && g.pilot_score_us[7]==600000);
 for(unsigned i=0;i<7;i++)assert(!g.pilot_score_us[i]);
 /* SOLO life must be invariant to tick partition, even at fractional rates. */
 game_t fine,coarse;game_init(&fine);fine.config.mode=GAME_SOLO;
 fine.config.capture_ms=100;fine.config.freshness_ms=60000;
 fine.config.solo_drain_percent=333;fine.config.solo_recover_percent=17;
 assert(game_start(&fine,0,1));game_observe(&fine,0,true,true,0);
 game_tick(&fine,1);game_observe(&fine,0,false,true,1);coarse=fine;
 for(unsigned t=2;t<=502;t++)game_tick(&fine,t);
 game_tick(&coarse,502);
 assert(fine.life_us==coarse.life_us && fine.life_fraction==coarse.life_fraction);
 assert(fine.life_us==98331 && fine.life_fraction==67);
 game_observe(&fine,0,true,true,502);game_observe(&coarse,0,true,true,502);
 for(unsigned t=503;t<=1003;t++)game_tick(&fine,t);
 game_tick(&coarse,1003);
 assert(fine.life_us==coarse.life_us && fine.life_fraction==coarse.life_fraction);
 assert(fine.life_us==98416 && fine.life_fraction==84);
 game_pause(&fine,0,1003);assert(game_resume(&fine,2000));
 game_observe(&fine,0,false,true,2000);game_observe(&coarse,0,false,true,1003);
 game_tick(&fine,1002000);game_tick(&coarse,1001003);
 assert(fine.solo_failed && coarse.solo_failed);
 assert(fine.elapsed_us==coarse.elapsed_us && fine.elapsed_us==30558);
 /* The selected tick partition must also preserve fractional depletion at 1%. */
 game_init(&fine);fine.config.mode=GAME_SOLO;fine.config.capture_ms=100;
 fine.config.freshness_ms=60000;fine.config.solo_drain_percent=1;
 assert(game_start(&fine,0,1));game_observe(&fine,0,true,true,0);game_tick(&fine,1);
 game_observe(&fine,0,false,true,1);coarse=fine;
 for(unsigned t=2;t<=101;t++)game_tick(&fine,t);
 game_tick(&coarse,101);assert(fine.life_us==99999 && !fine.life_fraction);
 assert(fine.life_us==coarse.life_us && fine.life_fraction==coarse.life_fraction);
 /* Recovery saturates exactly, clearing any fractional remainder. */
 game_init(&fine);fine.config.mode=GAME_SOLO;fine.config.solo_drain_percent=333;
 assert(game_start(&fine,0,1));game_observe(&fine,0,true,true,0);game_tick(&fine,1);
 game_observe(&fine,0,false,true,1);game_tick(&fine,2);assert(fine.life_fraction==67);
 game_observe(&fine,0,true,true,2);game_tick(&fine,100);
 assert(fine.life_us==3000000 && !fine.life_fraction);
 puts("PASS all six modes, eight pilots, capture/contest/decay/scoring/expiry/fault/slot validation and SOLO identity results");return 0;
}
