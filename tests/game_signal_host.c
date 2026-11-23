#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/game_signal.h"
int main(void){
 game_t diagnostic_game;game_init(&diagnostic_game);assert(game_start(&diagnostic_game,1,15));
 game_signal_t diagnostic={0};receiver_config_t trial={diagnostic_game.match_config.pilots[0].channel,28,20,10,30};
 uint8_t raw[22]={0},page[32];raw[12]=110;raw[16]=42;raw[21]=trial.channel;
 assert(!game_signal_reason(&diagnostic_game,0,&trial,42,raw));
 /* Every quality bit, including unknown future bits, remains a hard gate. */
 for(unsigned q=1;q<256;q++){raw[20]=q;assert(game_signal_reason(&diagnostic_game,0,&trial,42,raw)==GAME_SIGNAL_QUALITY);}
 raw[20]=8;raw[12]=0;raw[16]=41;raw[21]++;
 assert(game_signal_reason(&diagnostic_game,0,&trial,42,raw)==15);
 assert(game_signal_page(&diagnostic,0,0,100,page));assert(page[2]==0 && page[24]==255);
 game_signal_record(&diagnostic,0,123,raw,GAME_SIGNAL_RETRY,15,3,0,1000000);
 assert(game_signal_page(&diagnostic,0,2,1037000,page));
 assert(page[0]==30 && page[1]==0 && page[2]==2 && page[3]==8 && page[4]==15 && page[5]==2 && page[6]==3 && page[7]==raw[21]);
 assert(page[8]==123 && page[20]==41 && page[24]==37 && page[28]==0);
 game_signal_record(&diagnostic,0,123,raw,GAME_SIGNAL_TRANSPORT,GAME_SIGNAL_IO,4,-7,1000000);
 assert(game_signal_page(&diagnostic,0,0,999999,page));assert(page[24]==255 && page[28]==249);
 assert(!game_signal_page(&diagnostic,GAME_PILOTS,0,1000,page));
 game_signal_scan_t scan={0};game_signal_scan_begin(&scan,10,3,1000);
 assert(!game_signal_scan_due(&scan,10,true,50999));assert(game_signal_scan_due(&scan,10,true,51000));
 assert(game_signal_scan_retry(&scan,51000));assert(!game_signal_scan_due(&scan,10,true,51001));
 assert(!game_signal_scan_due(&scan,11,true,200000) && !scan.pending);
 game_signal_scan_begin(&scan,11,2,200000);assert(!game_signal_scan_due(&scan,11,false,300000));
 game_signal_scan_begin(&scan,12,2,300000);assert(!game_signal_scan_retry(&scan,800000));
 game_signal_scan_begin(&scan,12,2,300000);for(unsigned i=0;i<5;i++)assert(game_signal_scan_retry(&scan,350000+i*50000));assert(!game_signal_scan_retry(&scan,600000));
 for(unsigned mode=GAME_FLAG;mode<GAME_MODE_COUNT;mode++){
  game_t game;game_init(&game);receiver_service_t service={0};game_signal_t signal={0};
  game.config.mode=mode;for(unsigned p=0;p<GAME_PILOTS;p++)game.config.pilots[p].enabled=p<(mode==GAME_SOLO?1:4);
  for(unsigned slot=0;slot<4;slot++){receiver_slot_t *r=&service.slots[slot];r->enabled=r->online=r->rf_valid=r->have_observed=true;}
  assert(game_signal_ready(&game.config,&service));
  service.slots[0].have_desired=true;service.slots[0].desired=service.slots[0].observed;
  service.slots[0].desired.gain++;assert(!game_signal_ready(&game.config,&service));
  service.slots[0].desired=service.slots[0].observed;assert(game_signal_ready(&game.config,&service));
  service.slots[0].config_error=263;assert(!game_signal_ready(&game.config,&service));
  service.slots[0].config_error=0;assert(game_signal_ready(&game.config,&service));service.slots[0].stop_pending=true;assert(!game_signal_ready(&game.config,&service));service.slots[0].stop_pending=false;
  assert(game_start(&game,1000,15));unsigned pilot;
  for(unsigned n=0;n<8;n++){
   assert(game_signal_next(&signal,&game.match_config,&pilot));receiver_config_t trial={game.match_config.pilots[pilot].channel,28,20,10,0};
   uint8_t data[22]={0};data[12]=1;data[16]=10;data[21]=trial.channel;data[8]=25;
   assert(game_signal_sample(&signal,&game,pilot,&trial,10,data,2000+n*1000));assert(signal.present[pilot]);
   data[8]=15;assert(game_signal_sample(&signal,&game,pilot,&trial,10,data,2100+n*1000));assert(signal.present[pilot]);
   data[20]=32;data[8]=0;assert(!game_signal_sample(&signal,&game,pilot,&trial,10,data,2200+n*1000));assert(signal.present[pilot]);assert(game.state==GAME_PAUSED);assert(game_resume(&game,2250+n*1000));
   data[20]=0;data[16]=11;assert(!game_signal_sample(&signal,&game,pilot,&trial,10,data,2300+n*1000));assert(signal.present[pilot]);assert(game_resume(&game,2350+n*1000));
   data[16]=10;data[21]++;assert(!game_signal_sample(&signal,&game,pilot,&trial,10,data,2400+n*1000));assert(signal.present[pilot]);assert(game_resume(&game,2450+n*1000));
   data[21]=trial.channel;assert(game_signal_sample(&signal,&game,pilot,&trial,10,data,2500+n*1000));assert(!signal.present[pilot]);
  }
 }
 /* 30ms confirmation cancels on deadband, not merely the opposite threshold.
  * Independent pilots cannot contribute time to one another's candidates. */
 game_signal_t debounce={0};receiver_config_t cfg={36,28,20,10,30};
 game_signal_presence(&debounce,0,&cfg,25,1000,100000);
 game_signal_presence(&debounce,0,&cfg,25,30999,100000);assert(!debounce.present[0]);
 game_signal_presence(&debounce,1,&cfg,25,31000,100000);assert(!debounce.present[1]);
 game_signal_presence(&debounce,0,&cfg,15,31000,100000);assert(!debounce.confirming[0]);
 game_signal_presence(&debounce,0,&cfg,25,32000,100000);
 game_signal_presence(&debounce,0,&cfg,25,62000,100000);assert(debounce.present[0]);
 game_signal_presence(&debounce,0,&cfg,5,63000,100000);assert(debounce.present[0]);
 game_signal_presence(&debounce,0,&cfg,5,92999,100000);assert(debounce.present[0]);
 game_signal_presence(&debounce,0,&cfg,5,93000,100000);assert(!debounce.present[0]);
 game_signal_presence(&debounce,1,&cfg,25,131001,100000);assert(!debounce.present[1]);
 game_signal_presence(&debounce,1,&cfg,25,161001,100000);assert(debounce.present[1]);
 game_signal_presence(&debounce,2,&cfg,25,100000,100000);
 game_signal_presence(&debounce,2,&cfg,25,99000,100000);assert(!debounce.present[2] && debounce.candidate_us[2]==99000);
 /* Invalid observations cancel confirmation; late data cannot mutate it. */
 game_init(&diagnostic_game);diagnostic_game.config.mode=GAME_QUICK;assert(game_start(&diagnostic_game,1000,1));
 diagnostic=(game_signal_t){0};raw[12]=1;raw[16]=42;raw[20]=0;raw[21]=36;raw[8]=25;
 assert(game_signal_sample(&diagnostic,&diagnostic_game,0,&cfg,42,raw,2000));assert(!diagnostic.present[0] && diagnostic.confirming[0]);
 assert(!game_signal_sample(&diagnostic,&diagnostic_game,0,&cfg,42,raw,1500));assert(diagnostic.candidate_us[0]==2000);
 raw[20]=8;assert(!game_signal_sample(&diagnostic,&diagnostic_game,0,&cfg,42,raw,31000));assert(!diagnostic.confirming[0]);
 assert(game_resume(&diagnostic_game,40000));raw[20]=0;
 assert(game_signal_sample(&diagnostic,&diagnostic_game,0,&cfg,42,raw,41000));assert(!diagnostic.present[0]);
 game_signal_record(&diagnostic,0,diagnostic_game.session,raw,GAME_SIGNAL_RETRY,GAME_SIGNAL_QUALITY,1,0,42000);
 assert(diagnostic.confirming[0]); /* Normal retune settling must not restart every visit. */
 assert(game_signal_sample(&diagnostic,&diagnostic_game,0,&cfg,42,raw,71000));assert(diagnostic.present[0]);
 puts("PASS five signal modes, independent enter/exit confirmation, deadband/gap/invalid/late cancellation, channel/session/quality gates");
}
