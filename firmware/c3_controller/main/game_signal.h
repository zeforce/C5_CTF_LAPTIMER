#pragma once
#include "game.h"
#include "receiver_service.h"
#include <string.h>
enum { GAME_SIGNAL_ACCEPTED=1, GAME_SIGNAL_RETRY=2, GAME_SIGNAL_REJECTED=3, GAME_SIGNAL_TRANSPORT=4 };
enum { GAME_SIGNAL_SESSION=1, GAME_SIGNAL_NO_SAMPLES=2, GAME_SIGNAL_QUALITY=4, GAME_SIGNAL_CHANNEL=8, GAME_SIGNAL_IO=16 };
typedef struct {
 uint8_t data[22],status,reason,attempts;
 uint32_t session;int32_t error;uint64_t measured_us;
} game_signal_detail_t;
/* Presence hysteresis belongs to each pilot, not to the receiver currently
 * being scanned. Invalid samples never change the retained presence bit. */
typedef struct {
 bool present[GAME_PILOTS],confirming[GAME_PILOTS];
 uint64_t candidate_us[GAME_PILOTS],last_valid_us[GAME_PILOTS];
 unsigned cursor;game_signal_detail_t detail[GAME_PILOTS];
} game_signal_t;
/* Confirmation spans successive valid visits to this pilot's channel. It is
 * sampled evidence, not proof of continuous presence while scanning others. */
static inline void game_signal_presence(game_signal_t *s,unsigned pilot,const receiver_config_t *trial,uint16_t power,uint64_t now,uint64_t freshness_us){
 bool crossing=s->present[pilot]?power<=trial->exit_power:power>=trial->enter_power;
 if(s->confirming[pilot] && (now<s->last_valid_us[pilot] || now-s->last_valid_us[pilot]>freshness_us))s->confirming[pilot]=false;
 s->last_valid_us[pilot]=now;
 if(!crossing){s->confirming[pilot]=false;return;}
 if(!s->confirming[pilot]){s->confirming[pilot]=true;s->candidate_us[pilot]=now;}
 if(now>=s->candidate_us[pilot] && now-s->candidate_us[pilot]>=(uint64_t)trial->confirm_ms*1000){
  s->present[pilot]=!s->present[pilot];s->confirming[pilot]=false;
 }
}
static inline uint8_t game_signal_reason(const game_t *game,unsigned pilot,const receiver_config_t *trial,uint32_t expected_session,const uint8_t data[22]){
 uint32_t samples=(uint32_t)data[12]|((uint32_t)data[13]<<8)|((uint32_t)data[14]<<16)|((uint32_t)data[15]<<24);
 uint32_t session=(uint32_t)data[16]|((uint32_t)data[17]<<8)|((uint32_t)data[18]<<16)|((uint32_t)data[19]<<24);
 return ((!expected_session || session!=expected_session)?GAME_SIGNAL_SESSION:0) |
  (!samples?GAME_SIGNAL_NO_SAMPLES:0) | (data[20]?GAME_SIGNAL_QUALITY:0) |
  ((pilot>=GAME_PILOTS || data[21]!=trial->channel || trial->channel!=game->match_config.pilots[pilot].channel)?GAME_SIGNAL_CHANNEL:0);
}
static inline void game_signal_record(game_signal_t *s,unsigned pilot,uint32_t session,const uint8_t data[22],uint8_t status,uint8_t reason,unsigned attempts,int32_t error,uint64_t now){
 if(pilot>=GAME_PILOTS)return;
 if(status==GAME_SIGNAL_REJECTED || status==GAME_SIGNAL_TRANSPORT)s->confirming[pilot]=false;
 game_signal_detail_t *d=&s->detail[pilot];memcpy(d->data,data,22);d->status=status;d->reason=reason;d->attempts=attempts;d->session=session;d->error=error;d->measured_us=now;
}
/* Page 123..130; cached diagnostics never perform an RF/SPI exchange. */
static inline bool game_signal_page(const game_signal_t *s,unsigned pilot,unsigned slot,uint64_t now,uint8_t out[32]){
 if(pilot>=GAME_PILOTS || !out)return false;
 const game_signal_detail_t *d=&s->detail[pilot];memset(out,0,32);
 out[0]=30;out[1]=pilot;out[2]=d->status;out[3]=d->data[20];out[4]=d->reason;out[5]=slot;out[6]=d->attempts;out[7]=d->data[21];
 memcpy(out+8,&d->session,4);memcpy(out+12,d->data+8,8);memcpy(out+20,d->data+16,4);
 uint64_t age=d->status && now>=d->measured_us?(now-d->measured_us)/1000:UINT64_MAX;
 uint32_t age_ms=age>UINT32_MAX?UINT32_MAX:(uint32_t)age;
 memcpy(out+24,&age_ms,4);memcpy(out+28,&d->error,4);return true;
}
/* No sleeping under the controller mutex: each due step performs one exchange.
 * Session binding prevents a delayed read from leaking into another match. */
typedef struct {uint32_t session;uint64_t due_us,expires_us;unsigned pilot,attempts;bool pending;} game_signal_scan_t;
static inline void game_signal_scan_begin(game_signal_scan_t *s,uint32_t session,unsigned pilot,uint64_t now){
 *s=(game_signal_scan_t){.session=session,.due_us=now+50000,.expires_us=now+500000,.pilot=pilot,.pending=true};
}
static inline bool game_signal_scan_due(game_signal_scan_t *s,uint32_t session,bool running,uint64_t now){
 if(!running || s->session!=session){s->pending=false;return false;}
 return s->pending && now>=s->due_us;
}
static inline bool game_signal_scan_retry(game_signal_scan_t *s,uint64_t now){
 if(++s->attempts>=6 || now>=s->expires_us){s->pending=false;return false;}
 s->due_us=now+50000;return true;
}
static inline bool game_signal_next(game_signal_t *s,const game_config_t *config,unsigned *pilot){
 for(unsigned n=0;n<GAME_PILOTS;n++){unsigned p=s->cursor++%GAME_PILOTS;if(config->pilots[p].enabled){*pilot=p;return true;}}
 return false;
}
static inline bool game_signal_ready(const game_config_t *config,const receiver_service_t *service){
 if(!game_config_valid(config) || config->mode==GAME_LAPS)return false;
 for(unsigned p=0;p<GAME_PILOTS;p++)if(config->pilots[p].enabled){
  const receiver_slot_t *r=&service->slots[config->pilots[p].slot];
  if(!receiver_config_ready(r) || !r->enabled || !r->online || !r->rf_valid || r->stop_pending || r->restore_pending || !r->have_observed)return false;
 }return true;
}
static inline bool game_signal_sample(game_signal_t *s,game_t *game,unsigned pilot,const receiver_config_t *trial,uint32_t expected_session,const uint8_t data[22],uint64_t now){
 if(pilot>=GAME_PILOTS || game->state!=GAME_RUNNING || now<game->last_us || !game->match_config.pilots[pilot].enabled)return false;
 uint8_t reason=game_signal_reason(game,pilot,trial,expected_session,data);bool valid=!reason;
 unsigned attempts=s->detail[pilot].session==game->session && s->detail[pilot].status==GAME_SIGNAL_RETRY?s->detail[pilot].attempts:0;
 game_signal_record(s,pilot,game->session,data,valid?GAME_SIGNAL_ACCEPTED:GAME_SIGNAL_REJECTED,reason,attempts,0,now);
 if(valid){uint16_t power=(uint16_t)data[8]|((uint16_t)data[9]<<8);game_signal_presence(s,pilot,trial,power,now,(uint64_t)game->match_config.freshness_ms*1000);}
 else s->confirming[pilot]=false;
 game_observe(game,pilot,s->present[pilot],valid,now);return valid;
}
