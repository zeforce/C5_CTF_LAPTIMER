#include <assert.h>
#include <stdio.h>
#include "../firmware/c3_controller/main/game_feedback.h"
int main(void){
 game_t g;feedback_input_t in={.rf_online=true};feedback_t f;feedback_init(&f);
 for(unsigned mode=GAME_FFA;mode<=GAME_LOCK;mode++){
  game_init(&g);g.config.mode=mode;g.config.pilots[7].enabled=1;g.config.pilots[7].color=0x123456;
  assert(game_start(&g,0,1));g.pilot_owner=8;game_feedback_read(&g,&in);
  assert(in.owner_rgb_valid && in.owner_rgb==0x123456);
  assert(feedback_tick(&f,&in,false,100).rgb==0x123456);
  g.config.pilots[7].color=0xabcdef;game_feedback_read(&g,&in);assert(in.owner_rgb==0x123456);
  g.state=GAME_FINISHED;g.winner=8;game_feedback_read(&g,&in);
  assert(in.winner_rgb_valid && feedback_tick(&f,&in,false,200).rgb==0x123456);
  g.winner=0;game_feedback_read(&g,&in);assert(!in.winner_rgb_valid && feedback_tick(&f,&in,false,300).rgb==0xffffff);
  g.state=GAME_PAUSED;game_feedback_read(&g,&in);assert(feedback_tick(&f,&in,false,400).rgb==0xffff00);
  g.fault=3;game_feedback_read(&g,&in);assert(feedback_tick(&f,&in,false,500).rgb==0xff8000);
 }
 game_init(&g);g.config.mode=GAME_QUICK;assert(game_start(&g,0,1));g.state=GAME_FINISHED;g.winner=2;
 game_feedback_read(&g,&in);assert(feedback_tick(&f,&in,false,600).rgb==0x0000ff);
 g.challenger=1;g.progress_us=1200000;game_feedback_read(&g,&in);
 assert(in.capture_ms==g.match_config.capture_ms && in.capture_progress_ms==1200);
 g.config.capture_ms=1;game_feedback_read(&g,&in);assert(in.capture_ms==g.match_config.capture_ms);
 g.challenger=0;game_feedback_read(&g,&in);assert(in.capture_progress_ms==0);
 g.winner=0;game_feedback_read(&g,&in);assert(!in.winner_rgb_valid);
 game_init(&g);g.config.mode=GAME_FFA;g.config.pilots[0].color=0;assert(game_start(&g,0,1));g.pilot_owner=1;
 game_feedback_read(&g,&in);assert(in.owner_rgb_valid && feedback_tick(&f,&in,false,700).rgb==0);
 g.pilot_owner=9;game_feedback_read(&g,&in);assert(!in.owner_rgb_valid);
 g.pilot_owner=2;game_feedback_read(&g,&in);assert(!in.owner_rgb_valid);
 game_init(&g);g.config.pilots[1].enabled=1;assert(game_start(&g,0,3));g.laps[0].total_laps=3;g.laps[1].total_laps=4;
 game_feedback_read(&g,&in);assert(in.laps==7);
 feedback_init(&f);feedback_tick(&f,&in,false,0);g.laps[1].total_laps++;game_feedback_read(&g,&in);
 assert(feedback_tick(&f,&in,false,20).buzzer && f.transitions==1);
 puts("PASS personal owner/winner frozen colors, team winner, tie, fault/pause priority, black color, invalid identities, multi-pilot lap notification");
}
