/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "game.h"
#include "feedback.h"

/* Caller holds the controller lock; no full game copy or dynamic allocation. */
static inline void game_feedback_read(const game_t *g,feedback_input_t *out){
 const game_config_t *rules=game_session_config(g);
 out->state=g->state;out->owner=g->owner;out->fault=g->fault;
 out->pilot_owner=g->pilot_owner;out->winner=g->winner;
 out->capture_ms=rules->capture_ms;
 out->capture_progress_ms=g->challenger ? g->progress_us/1000 : 0;
 out->owner_rgb_valid=out->winner_rgb_valid=false;
 out->owner_rgb=out->winner_rgb=0;
 out->laps=0;
 for(unsigned i=0;i<GAME_PILOTS;i++)if(rules->pilots[i].enabled)out->laps+=g->laps[i].total_laps;
 if(rules->mode==GAME_FFA || rules->mode==GAME_LOCK){
  if(g->pilot_owner && g->pilot_owner<=GAME_PILOTS && rules->pilots[g->pilot_owner-1].enabled){
   out->owner_rgb_valid=true;out->owner_rgb=rules->pilots[g->pilot_owner-1].color&0xffffff;
  }
  if(g->winner && g->winner<=GAME_PILOTS && rules->pilots[g->winner-1].enabled){
   out->winner_rgb_valid=true;out->winner_rgb=rules->pilots[g->winner-1].color&0xffffff;
  }
 }else if(rules->mode==GAME_FLAG || rules->mode==GAME_QUICK){
  if(g->winner==1 || g->winner==2){out->winner_rgb_valid=true;out->winner_rgb=g->winner==1?0xff0000:0x0000ff;}
 }
}
