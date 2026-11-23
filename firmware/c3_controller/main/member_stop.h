#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Network-worker owned latch. A reconnect must not cancel unfinished stop.
 * Acquisition and game pause have separate completion flags. */
typedef struct {bool capture_pending,game_pending;int64_t retry_at;uint32_t requests,attempts;} member_stop_t;
static inline void member_stop_request(member_stop_t *s){s->capture_pending=true;s->game_pending=true;s->retry_at=0;++s->requests;}
static inline bool member_stop_pending(const member_stop_t *s){return s->capture_pending || s->game_pending;}
static inline bool member_stop_due(member_stop_t *s,int64_t now){
 if(!member_stop_pending(s) || now<0 || now<s->retry_at)return false;
 ++s->attempts;s->retry_at=now+250000;return true;
}
static inline void member_stop_complete(member_stop_t *s,bool capture_ok,bool game_ok){
 if(capture_ok){s->capture_pending=false;}
 if(game_ok){s->game_pending=false;}
}
