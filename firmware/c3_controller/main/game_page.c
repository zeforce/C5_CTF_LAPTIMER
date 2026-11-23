#include "game_page.h"
#include <string.h>
static void put32(uint8_t *out,uint32_t v){for(unsigned i=0;i<4;i++)out[i]=v>>(8*i);}
bool game_page_encode(const game_t *g,unsigned page,unsigned offset,uint8_t p[32]){
    if(!g || !p || page>35 || offset>=LAP_RECENT_MAX)return false;
    memset(p,0,32);p[0]=1;p[1]=page;
    const game_config_t *c=(page>=27)?game_session_config(g):&g->config;
    if(page==0){p[2]=game_session_config(g)->mode;p[3]=g->state;p[4]=g->owner;p[5]=g->challenger;p[6]=g->fault;p[7]=g->winner;
        put32(p+8,g->session);put32(p+12,g->elapsed_us/1000);put32(p+16,g->progress_us/1000);
        put32(p+20,g->score_us[0]/1000);put32(p+24,g->score_us[1]/1000);p[28]=g->present_mask;p[29]=g->known_mask;
    } else if(page==1 || page==27){p[2]=c->mode;put32(p+4,c->minimum_lap_ms);put32(p+8,c->duration_ms);put32(p+12,c->capture_ms);put32(p+16,c->freshness_ms);put32(p+20,c->solo_drain_percent);put32(p+24,c->solo_recover_percent);}
    else if((page>=2 && page<=9) || page>=28){unsigned i=page>=28?page-28:page-2;const game_pilot_t *v=&c->pilots[i];p[2]=v->enabled;p[3]=v->channel;p[4]=v->team;p[5]=v->slot;put32(p+6,v->color);memcpy(p+10,v->name,17);}
    else if(page<=17){unsigned i=page-10;p[2]=i;put32(p+4,g->laps[i].total_laps);put32(p+8,g->laps[i].fastest_us/1000);put32(p+12,g->laps[i].fastest_lap_number);put32(p+16,g->captures[i]);put32(p+20,g->pilot_score_us[i]/1000);}
    else if(page<=25){unsigned i=page-18;const lap_timer_t *t=&g->laps[i];p[2]=i;p[4]=offset;p[5]=offset>>8;put32(p+8,t->total_laps);
        for(unsigned n=0;n<5 && offset+n<t->recent_count;n++){unsigned index=(t->recent_next+LAP_RECENT_MAX-1-offset-n)%LAP_RECENT_MAX;put32(p+12+4*n,t->recent_us[index]/1000);++p[3];}}
    else if(page==26){p[2]=g->pilot_owner;p[3]=g->solo_started;p[4]=g->solo_failed;put32(p+8,g->life_us/1000);put32(p+12,(1u<<GAME_MODE_COUNT)-1);p[16]=GAME_PILOTS;}
    if(page>=27)put32(p+28,g->session);
    return true;
}
