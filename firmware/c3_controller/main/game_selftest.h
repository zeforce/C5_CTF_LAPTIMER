/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "game.h"

/* Caller provides isolated scratch before radio startup. Never pass live game. */
static inline unsigned game_rule_selftest(game_t *g){
 unsigned failures=0;
 for(unsigned mode=0;mode<GAME_MODE_COUNT;mode++){
  game_init(g);g->config.mode=mode;g->config.capture_ms=1000;
  g->config.duration_ms=10000;g->config.freshness_ms=60000;
  failures+=!game_start(g,0,1);
  if(mode==GAME_LAPS){
   lap_event_t e={.session=1,.sequence=1,.timestamp_us=100,.edge=LAP_ENTER,.valid=true};
   lap_timer_consume(&g->laps[0],&e,0);e.sequence++;e.timestamp_us=200;e.edge=LAP_EXIT;
   lap_timer_consume(&g->laps[0],&e,0);e.sequence++;e.timestamp_us=1000100;e.edge=LAP_ENTER;
   failures+=!lap_timer_consume(&g->laps[0],&e,0);
   failures+=g->laps[0].total_laps!=1 || g->laps[0].fastest_us!=1000000;
   game_stop(g,1000100);failures+=g->state!=GAME_FINISHED;
   continue;
  }
  game_observe(g,0,true,true,0);game_tick(g,1500000);
  if(mode==GAME_FLAG)failures+=g->owner!=1 || g->score_us[0]!=500000;
  if(mode==GAME_QUICK)failures+=g->owner!=1 || g->score_us[0]!=1500000;
  if(mode==GAME_SOLO)failures+=!g->solo_started || g->life_us!=1000000 || g->pilot_score_us[0]!=1500000;
  if(mode==GAME_FFA || mode==GAME_LOCK)failures+=g->pilot_owner!=1 || g->captures[0]!=1 || g->pilot_score_us[0]!=1500000;
  uint64_t elapsed=g->elapsed_us;
  game_pause(g,3,1500000);game_tick(g,9000000);failures+=g->elapsed_us!=elapsed;
  failures+=!game_resume(g,9000000);game_tick(g,10000000);failures+=g->elapsed_us!=elapsed;
  game_observe(g,0,true,true,10000000);game_tick(g,20000000);
  failures+=g->state!=GAME_FINISHED || g->elapsed_us!=10000000 || g->winner!=1;
 }
 /* Loss of a full observation sweep must pause both start and resume. */
 game_init(g);g->config.pilots[1].enabled=1;failures+=game_start(g,0,1);
 game_init(g);g->config.mode=GAME_FLAG;failures+=!game_start(g,10000000,1);
 game_tick(g,11000000);game_tick(g,12000000);game_tick(g,12500001);
 failures+=g->state!=GAME_PAUSED || g->fault!=2 || g->elapsed_us!=0;
 failures+=!game_resume(g,20000000);game_tick(g,22500001);
 failures+=g->state!=GAME_PAUSED || g->fault!=2;
 /* SOLO failure credits only the interval before life exhaustion. */
 game_init(g);g->config.mode=GAME_SOLO;g->config.capture_ms=1000;g->config.freshness_ms=60000;
 failures+=!game_start(g,0,1);game_observe(g,0,true,true,0);game_tick(g,100000);
 game_observe(g,0,false,true,100000);game_tick(g,2000000);
 failures+=!g->solo_failed || g->state!=GAME_FINISHED || g->winner || g->elapsed_us!=1100000;
 return failures;
}
