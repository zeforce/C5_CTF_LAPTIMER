#pragma once
#include "game.h"
#include "receiver_service.h"
/* Local receiver journals have independent consumers. Session start skips old
 * history; delivery loss invalidates the lap baseline before consuming more. */
typedef struct {uint64_t cursor[RECEIVER_SLOTS];} game_receiver_t;
static inline uint8_t game_receiver_mask(const game_config_t *c){
 uint8_t mask=0;for(unsigned p=0;p<GAME_PILOTS;p++)if(c->pilots[p].enabled)mask|=1u<<c->pilots[p].slot;return mask;
}
static inline void game_receiver_bind(game_receiver_t *consumer,const receiver_service_t *s){
 for(unsigned slot=0;slot<RECEIVER_SLOTS;slot++)consumer->cursor[slot]=s->slots[slot].events.published;
}
static inline bool game_receiver_ready(const game_config_t *c,const receiver_service_t *s){
 if(!game_config_valid(c) || c->mode!=GAME_LAPS)return false;
 for(unsigned p=0;p<GAME_PILOTS;p++)if(c->pilots[p].enabled){
  const receiver_slot_t *r=&s->slots[c->pilots[p].slot];
  if(!receiver_config_ready(r) || !r->enabled || !r->online || !r->rf_valid || !r->calibrated || r->stop_pending || r->restore_pending || !r->have_observed || r->observed.channel!=c->pilots[p].channel)return false;
 }return true;
}
static inline unsigned game_receiver_consume(game_receiver_t *consumer,game_t *g,const receiver_service_t *s,unsigned slot,unsigned budget){
 if(slot>=RECEIVER_SLOTS || g->state!=GAME_RUNNING || g->match_config.mode!=GAME_LAPS)return 0;
 unsigned processed=0;service_event_t event;event_delivery_t delivery;
 while(processed<budget && event_service_peek(&s->slots[slot].events,consumer->cursor[slot],&event,&delivery)){
  for(unsigned p=0;p<GAME_PILOTS;p++)if(g->match_config.pilots[p].enabled && g->match_config.pilots[p].slot==slot){
   lap_timer_t *timer=&g->laps[p];if(delivery.lost || delivery.reset || (event.flags&12))lap_timer_invalidate(timer);
   lap_event_t e={event.session,event.sequence,event.source_us,event.edge,(event.flags&1)!=0};
   lap_timer_consume(timer,&e,NULL);
  }
  consumer->cursor[slot]=delivery.position;++processed;
 }return processed;
}
