#include "feedback.h"
#include <string.h>
void feedback_init(feedback_t *f){memset(f,0,sizeof(*f));}
feedback_output_t feedback_tick(feedback_t *f,const feedback_input_t *in,bool pressed,uint64_t now){
 feedback_output_t out={0};
 /* Event priorities: fault, finish, state, capture, lap, countdown. Each
  * pattern is bounded; continuously changing observations cannot hold a tone. */
 uint8_t pattern=0;
 uint32_t second=in->countdown_ms/1000+(in->countdown_ms%1000!=0);
 if(f->initialized){
  const feedback_input_t *old=&f->input;
  if((in->fault && old->fault!=in->fault) || (!in->rf_online && old->rf_online))pattern=5;
  else if(in->state!=old->state)pattern=in->state==3?4:in->state==2?3:1;
  else if(in->owner!=old->owner || in->pilot_owner!=old->pilot_owner)pattern=2;
  else if(in->laps>old->laps)pattern=1;
  else if(in->countdown_phase==2 && second &&
          (old->countdown_phase!=2 || second!=f->countdown_second))pattern=1;
  if(pattern){++f->transitions;f->pattern=pattern;f->pattern_since=now;
   f->beep_until=now+(pattern==5?600:pattern==4?700:pattern==3?400:pattern==2?300:100);}
 }
 f->countdown_second=second;
 f->input=*in;f->initialized=true;
 if(pressed!=f->raw_button){f->raw_button=pressed;f->raw_since=now;}
 if(pressed!=f->stable_button && now-f->raw_since>=30){
  f->stable_button=pressed;
  if(pressed){f->pressed_since=now;f->hold_sent=false;}
  else if(!f->hold_sent){out.button_event=FEEDBACK_CLICK;++f->clicks;}
 }
 if(f->stable_button && !f->hold_sent && now-f->pressed_since>=1500){f->hold_sent=true;out.button_event=FEEDBACK_HOLD;++f->holds;}
 uint64_t age=now-f->pattern_since;
 out.buzzer=now<f->beep_until && (f->pattern==1 ||
  (f->pattern==2 && age%200<100) ||
  (f->pattern==3 && age%300<100) ||
  (f->pattern==4 && (age<100 || (age>=200 && age<300) || age>=400)) ||
  (f->pattern==5 && age%200<100));
 if(in->fault || !in->rf_online){out.rgb=0xff8000;out.led=now%500<250;}
 else if(in->countdown_phase){out.rgb=0xffffff;out.led=in->countdown_ms%1000<150;}
 else if(in->state==2){out.rgb=0xffff00;out.led=now%1000<500;}
 else if(in->state==3){out.rgb=in->winner_rgb_valid?in->winner_rgb:0xffffff;out.led=true;}
 else if(in->state==1){out.rgb=in->owner_rgb_valid?in->owner_rgb:in->owner==1?0xff0000:in->owner==2?0x0000ff:0x00ff00;out.led=true;}
 else {out.rgb=0x008080;out.led=now%2000<100;}
 /* A contested capture pulses faster as completion approaches. */
 if(in->rf_online && !in->fault && !in->countdown_phase && in->state==1 &&
    in->capture_ms && in->capture_progress_ms){
  uint32_t progress=in->capture_progress_ms>in->capture_ms?in->capture_ms:in->capture_progress_ms;
  uint32_t period=800-(uint64_t)progress*600/in->capture_ms;
  out.led=now%period<period/2;
 }
 return out;
}
