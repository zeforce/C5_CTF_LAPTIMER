/* SPDX-License-Identifier: GPL-3.0-only */
#include "game.h"
#include "game_identity.h"
#include "../../common/fpv_channels.h"
#include <string.h>
static uint8_t enabled(const game_t *g) {
    uint8_t mask=0; for(unsigned i=0;i<GAME_PILOTS;i++) if(game_session_config(g)->pilots[i].enabled) mask|=1u<<i; return mask;
}
void game_init(game_t *g) {
    memset(g,0,sizeof(*g)); g->config.minimum_lap_ms=1000;
    g->config.solo_drain_percent=100;g->config.solo_recover_percent=100;
    g->config.duration_ms=180000; g->config.capture_ms=3000; g->config.freshness_ms=2500;
    for(unsigned i=0;i<GAME_PILOTS;i++) {
        g->config.pilots[i]=(game_pilot_t){.enabled=i==0,.channel=32+(i+4)%8,.team=i%2+1,.slot=i%4,.color=i%2?0x3388ff:0xff4444};
        g->config.pilots[i].name[0]='1'+i; lap_timer_init(&g->laps[i],1000000);
    }
}
bool game_config_valid(const game_config_t *c) {
    if(!c || c->mode>=GAME_MODE_COUNT || c->minimum_lap_ms<1 || c->minimum_lap_ms>3600000 ||
       c->duration_ms<1000 || c->duration_ms>86400000 || c->capture_ms<100 || c->capture_ms>600000 ||
       c->freshness_ms<100 || c->freshness_ms>60000 ||
       c->solo_drain_percent<1 || c->solo_drain_percent>1000 || c->solo_recover_percent<1 || c->solo_recover_percent>1000) return false;
    unsigned count=0;
    for(unsigned i=0;i<GAME_PILOTS;i++) {
        const game_pilot_t *p=&c->pilots[i];
        if(p->enabled>1 || p->channel>=64 || p->team<1 || p->team>2 || p->slot>=4 || p->color>0xffffff || !game_name_valid(p->name,sizeof(p->name))) return false;
        if(!p->enabled) continue;
        ++count;
        for(unsigned j=0;j<i;j++) if(c->pilots[j].enabled &&
            (fpv_channel_frequency(p->channel)==fpv_channel_frequency(c->pilots[j].channel) ||
             (c->mode==GAME_LAPS && p->slot==c->pilots[j].slot))) return false;
    }
    return count>0 && (c->mode!=GAME_SOLO || count==1);
}
bool game_start(game_t *g,uint64_t now,uint8_t slots) {
    if(g->state==GAME_RUNNING || g->state==GAME_PAUSED || !game_config_valid(&g->config)) return false;
    if(g->config.mode==GAME_LAPS) for(unsigned i=0;i<GAME_PILOTS;i++)
        if(g->config.pilots[i].enabled && !(slots&(1u<<g->config.pilots[i].slot))) return false;
    game_config_t c=g->config;uint64_t fault_revision=g->fault_revision; uint32_t session=g->session+1; if(!session)session=1;
    memset(g,0,sizeof(*g));g->fault_revision=fault_revision;g->config=c;g->session=session;g->state=GAME_RUNNING;g->match_config=c;g->last_us=now;g->life_us=(uint64_t)c.capture_ms*1000;
    for(unsigned i=0;i<GAME_PILOTS;i++) {lap_timer_init(&g->laps[i],c.minimum_lap_ms*1000u);g->seen_us[i]=now;if(c.pilots[i].enabled)lap_timer_start(&g->laps[i]);}
    return true;
}
void game_tick(game_t *g,uint64_t now) {
    if(g->state!=GAME_RUNNING || now<g->last_us)return;
    uint64_t dt=now-g->last_us;g->last_us=now;
    if(g->match_config.mode==GAME_LAPS)return;
    uint8_t mask=enabled(g);
    /* Warm-up and resume require a complete fresh sweep before time can run. */
    for(unsigned i=0;i<GAME_PILOTS;i++) if((mask&(1u<<i)) && now-g->seen_us[i]>(uint64_t)g->match_config.freshness_ms*1000) {
        game_pause(g,2,now);return;
    }
    if((g->known_mask&mask)!=mask)return;
    uint64_t remaining=(uint64_t)g->match_config.duration_ms*1000-g->elapsed_us;if(dt>remaining)dt=remaining;
    if(g->match_config.mode==GAME_SOLO) {
        bool present=(g->present_mask&mask)!=0;
        if(present) {
            g->solo_started=1;
            uint64_t maximum=(uint64_t)g->match_config.capture_ms*1000;
            uint64_t life=g->life_us*100+g->life_fraction+dt*g->match_config.solo_recover_percent;
            if(life>=maximum*100){g->life_us=maximum;g->life_fraction=0;}
            else {g->life_us=life/100;g->life_fraction=life%100;}
        } else if(g->solo_started) {
            uint64_t life=g->life_us*100+g->life_fraction;
            uint64_t drain=dt*g->match_config.solo_drain_percent;
            if(drain>=life){dt=(life+g->match_config.solo_drain_percent-1)/g->match_config.solo_drain_percent;g->life_us=0;g->life_fraction=0;g->solo_failed=1;}
            else {life-=drain;g->life_us=life/100;g->life_fraction=life%100;}
        }
        if(g->solo_started){
            g->elapsed_us+=dt;g->score_us[0]+=dt;
            /* SOLO accepts any one enabled pilot, not necessarily slot zero.
             * Export the same survival duration through personal results. */
            for(unsigned i=0;i<GAME_PILOTS;i++)if(mask&(1u<<i))g->pilot_score_us[i]+=dt;
        }
        if(g->solo_failed || g->elapsed_us>=(uint64_t)g->match_config.duration_ms*1000)game_stop(g,now);
        return;
    }
    if(g->match_config.mode==GAME_FFA || g->match_config.mode==GAME_LOCK) {
        uint8_t present=g->present_mask&mask;
        /* A single identified pilot can take a flag. Concurrent presence freezes
         * changes, avoiding deterministic low-index preference. */
        if(present && !(present&(present-1)) && (g->match_config.mode!=GAME_LOCK || !g->pilot_owner)) {
            unsigned pilot=0;while(!(present&(1u<<pilot)))++pilot;
            if(g->pilot_owner!=pilot+1){g->pilot_owner=pilot+1;++g->captures[pilot];}
        }
        if(g->pilot_owner)g->pilot_score_us[g->pilot_owner-1]+=dt;
        g->elapsed_us+=dt;
        if(g->elapsed_us==(uint64_t)g->match_config.duration_ms*1000)game_stop(g,now);
        return;
    }
    uint8_t teams=0;for(unsigned i=0;i<GAME_PILOTS;i++)if(g->present_mask&(1u<<i))teams|=g->match_config.pilots[i].team==1?1:2;
    uint8_t challenge=teams==1?1:teams==2?2:0;
    uint64_t before=dt; uint8_t old_owner=g->owner;
    if(g->match_config.mode==GAME_QUICK && challenge && challenge!=g->owner) {
        g->owner=challenge;g->challenger=0;g->progress_us=0;before=0;
    } else if(challenge && challenge!=g->owner) {
        if(g->challenger!=challenge){g->challenger=challenge;g->progress_us=0;}
        uint64_t needed=(uint64_t)g->match_config.capture_ms*1000-g->progress_us;
        if(dt>=needed){before=needed;g->owner=challenge;g->challenger=0;g->progress_us=0;}
        else g->progress_us+=dt;
    } else if(teams==0 || (challenge && challenge==g->owner)) {
        if(g->progress_us>dt)g->progress_us-=dt;else {g->progress_us=0;g->challenger=0;}
    } /* Both teams: freeze capture, retain owner scoring. */
    if(old_owner)g->score_us[old_owner-1]+=before;
    if(g->owner && dt>before)g->score_us[g->owner-1]+=dt-before;
    g->elapsed_us+=dt;
    if(g->elapsed_us==(uint64_t)g->match_config.duration_ms*1000)game_stop(g,now);
}
void game_observe(game_t *g,unsigned pilot,bool present,bool valid,uint64_t now) {
    if(g->state!=GAME_RUNNING || pilot>=GAME_PILOTS || !g->match_config.pilots[pilot].enabled || now<g->last_us)return;
    game_tick(g,now);if(g->state!=GAME_RUNNING)return;
    if(!valid){game_pause(g,1,now);return;}
    g->seen_us[pilot]=now;g->known_mask|=1u<<pilot;
    if(present)g->present_mask|=1u<<pilot;else g->present_mask&=~(1u<<pilot);
}
void game_pause(game_t *g,uint8_t fault,uint64_t now) {
    if(g->state!=GAME_RUNNING)return;
    if(fault && !++g->fault_revision)++g->fault_revision;
    /* Fault intervals are not credited. */
    g->last_us=now;g->state=GAME_PAUSED;g->fault=fault;g->known_mask=g->present_mask=0;
    for(unsigned i=0;i<GAME_PILOTS;i++)lap_timer_invalidate(&g->laps[i]);
}
bool game_resume(game_t *g,uint64_t now) {
    if(g->state!=GAME_PAUSED)return false;
    g->state=GAME_RUNNING;g->fault=0;g->last_us=now;g->known_mask=g->present_mask=0;for(unsigned i=0;i<GAME_PILOTS;i++)g->seen_us[i]=now;return true;
}
void game_stop(game_t *g,uint64_t now) {
    if(g->state!=GAME_RUNNING && g->state!=GAME_PAUSED)return;
    /* Caller ticks before manual stop. No recursive tick at timed expiry. */
    g->state=GAME_FINISHED;g->last_us=now;
    g->winner=g->score_us[0]==g->score_us[1]?0:g->score_us[0]>g->score_us[1]?1:2;
    if(g->match_config.mode==GAME_SOLO)g->winner=g->solo_started && !g->solo_failed && g->elapsed_us>=(uint64_t)g->match_config.duration_ms*1000?1:0;
    if(g->match_config.mode==GAME_FFA || g->match_config.mode==GAME_LOCK){
        uint64_t best=0;g->winner=0;
        for(unsigned i=0;i<GAME_PILOTS;i++){
            if(g->pilot_score_us[i]>best){best=g->pilot_score_us[i];g->winner=i+1;}
            else if(best && g->pilot_score_us[i]==best)g->winner=0;
        }
    }
    for(unsigned i=0;i<GAME_PILOTS;i++)lap_timer_stop(&g->laps[i]);
}
