#include "start_schedule.h"
#include <string.h>
bool start_schedule_prepare(start_schedule_t *s,uint32_t session,int64_t at,int64_t now){
 return start_schedule_prepare_action(s,session,at,now,1);
}
bool start_schedule_prepare_action(start_schedule_t *s,uint32_t session,int64_t at,int64_t now,uint8_t action){
 if(action!=1 && action!=2)return false;
 if(!s || !session || now<0 || at<=now || at-now<2000000 || at-now>30000000)return false;
 if(s->phase)return s->session==session && s->at_us==at && s->action==action;
 *s=(start_schedule_t){session,at,START_PREPARED,action};return true;
}
bool start_schedule_arm(start_schedule_t *s,uint32_t session,int64_t now){
 if(!s || !session || s->session!=session || !s->phase || s->at_us-now<1000000)return false;
 s->phase=START_ARMED;return true;
}
bool start_schedule_cancel(start_schedule_t *s,uint32_t session){
 if(!s || !session)return false;
 if(s->phase && s->session!=session)return false;
 memset(s,0,sizeof(*s));return true;
}
uint32_t start_schedule_take(start_schedule_t *s,int64_t now,bool healthy){
 if(!s || !s->phase)return 0;
 if(!healthy){memset(s,0,sizeof(*s));return 0;}
 if(now<s->at_us)return 0;
 uint32_t session=s->phase==START_ARMED && now-s->at_us<=50000?s->session:0;
 memset(s,0,sizeof(*s));return session;
}
